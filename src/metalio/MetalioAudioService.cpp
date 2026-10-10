#include "MetalioAudioService.h"

#if FREEINK_DEVICE_METALIO_EINK4

#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <deque>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "MetalioAudioCodec.h"

namespace metalio_audio_service {
namespace {

constexpr std::size_t kMaxPlaybackQueue = 4;
constexpr std::size_t kMaxCaptureRequests = 4;
constexpr std::size_t kMaxCaptureFrames = 4;
constexpr std::size_t kCaptureFrameSamples = 960;  // 60 ms @ 16 kHz

constexpr uint32_t kTaskStackWords = 4096;
constexpr UBaseType_t kOutputTaskPriority = 4;
constexpr UBaseType_t kInputTaskPriority = 5;
constexpr uint32_t kTaskStopTimeoutMs = 1500;

struct PlaybackRequest {
  std::vector<int16_t> pcm;
  bool completed = false;
  bool success = false;
  bool cancelled = false;
  std::mutex mutex;
  std::condition_variable cv;
};

struct CaptureRequest {
  std::size_t samples = 0;
};

struct CaptureFrame {
  std::vector<int16_t> samples;
};

MetalioAudioCodec outputCodec;
MetalioAudioCodec inputCodec;

std::mutex serviceMutex;
std::condition_variable serviceCv;
std::deque<std::shared_ptr<PlaybackRequest>> playbackQueue;
std::deque<CaptureRequest> captureRequests;
std::deque<CaptureFrame> captureQueue;

TaskHandle_t outputTaskHandle = nullptr;
TaskHandle_t inputTaskHandle = nullptr;

bool serviceStarted = false;
bool stopRequested = false;

void completePlayback(const std::shared_ptr<PlaybackRequest>& request,
                      bool success) {
  {
    std::lock_guard<std::mutex> lock(request->mutex);
    if (request->completed) return;
    request->success = success;
    request->completed = true;
  }
  request->cv.notify_all();
}

bool playbackCancelled(const std::shared_ptr<PlaybackRequest>& request) {
  std::lock_guard<std::mutex> lock(request->mutex);
  return request->cancelled;
}

void cancelPlayback(const std::shared_ptr<PlaybackRequest>& request) {
  {
    std::lock_guard<std::mutex> lock(request->mutex);
    request->cancelled = true;
  }
  completePlayback(request, false);
}

void outputTask(void*) {
  for (;;) {
    std::shared_ptr<PlaybackRequest> request;

    {
      std::unique_lock<std::mutex> lock(serviceMutex);
      serviceCv.wait(lock, [] {
        return stopRequested || !playbackQueue.empty();
      });

      if (playbackQueue.empty() && stopRequested) break;

      if (!playbackQueue.empty()) {
        request = std::move(playbackQueue.front());
        playbackQueue.pop_front();
      }
    }

    if (!request) continue;
    if (playbackCancelled(request)) {
      completePlayback(request, false);
      continue;
    }

    bool success = outputCodec.startOutput();
    if (success) {
      std::size_t offset = 0;
      while (offset < request->pcm.size()) {
        if (playbackCancelled(request)) {
          success = false;
          break;
        }

        const std::size_t written =
            outputCodec.write(request->pcm.data() + offset,
                              request->pcm.size() - offset, 1000);
        if (written == 0) {
          success = false;
          break;
        }
        offset += written;
      }
      outputCodec.stopOutput();
    }

    completePlayback(request, success);
  }

  outputCodec.stopOutput();

  {
    std::lock_guard<std::mutex> lock(serviceMutex);
    outputTaskHandle = nullptr;
  }
  serviceCv.notify_all();
  vTaskDelete(nullptr);
}

void inputTask(void*) {
  for (;;) {
    CaptureRequest request;

    {
      std::unique_lock<std::mutex> lock(serviceMutex);
      serviceCv.wait(lock, [] {
        return stopRequested || !captureRequests.empty();
      });

      if (stopRequested) break;

      request = captureRequests.front();
      captureRequests.pop_front();
    }

    if (!inputCodec.startInput()) {
      LOG_ERR("METALIO-AUDIO", "Failed to start Metalio microphone codec");
      continue;
    }

    CaptureFrame frame;
    frame.samples.resize(request.samples);

    std::size_t offset = 0;
    while (offset < request.samples) {
      const std::size_t read =
          inputCodec.read(frame.samples.data() + offset,
                          request.samples - offset, 1000);
      if (read == 0) break;
      offset += read;
    }

    inputCodec.stopInput();

    if (offset != request.samples) continue;

    {
      std::lock_guard<std::mutex> lock(serviceMutex);
      if (captureQueue.size() >= kMaxCaptureFrames) {
        captureQueue.pop_front();
      }
      captureQueue.push_back(std::move(frame));
    }
    serviceCv.notify_all();
  }

  inputCodec.stopInput();

  {
    std::lock_guard<std::mutex> lock(serviceMutex);
    inputTaskHandle = nullptr;
  }
  serviceCv.notify_all();
  vTaskDelete(nullptr);
}

bool waitForTasksToStop(uint32_t timeoutMs) {
  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds(timeoutMs);

  std::unique_lock<std::mutex> lock(serviceMutex);
  return serviceCv.wait_until(lock, deadline, [] {
    return outputTaskHandle == nullptr && inputTaskHandle == nullptr;
  });
}

bool ensureServiceStarted() {
  {
    std::lock_guard<std::mutex> lock(serviceMutex);
    if (serviceStarted) return true;
    stopRequested = false;
  }

  TaskHandle_t outputHandle = nullptr;
  if (xTaskCreate(outputTask, "metalio_audio_out", kTaskStackWords, nullptr,
                  kOutputTaskPriority, &outputHandle) != pdPASS) {
    LOG_ERR("METALIO-AUDIO", "Failed to create audio output task");
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(serviceMutex);
    outputTaskHandle = outputHandle;
    serviceStarted = true;
  }

  TaskHandle_t inputHandle = nullptr;
  if (xTaskCreate(inputTask, "metalio_audio_in", kTaskStackWords, nullptr,
                  kInputTaskPriority, &inputHandle) != pdPASS) {
    LOG_ERR("METALIO-AUDIO", "Failed to create audio input task");

    {
      std::lock_guard<std::mutex> lock(serviceMutex);
      stopRequested = true;
    }
    serviceCv.notify_all();

    if (!waitForTasksToStop(kTaskStopTimeoutMs)) {
      LOG_ERR("METALIO-AUDIO",
              "Audio output task did not stop after input task failure");
    }

    {
      std::lock_guard<std::mutex> lock(serviceMutex);
      serviceStarted = false;
      stopRequested = false;
      playbackQueue.clear();
      captureRequests.clear();
      captureQueue.clear();
    }
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(serviceMutex);
    inputTaskHandle = inputHandle;
  }

  LOG_INF("METALIO-AUDIO", "PCM audio service started");
  serviceCv.notify_all();
  return true;
}

}  // namespace

bool start() {
  return ensureServiceStarted();
}

void stop() {
  {
    std::lock_guard<std::mutex> lock(serviceMutex);
    if (!serviceStarted) return;
    stopRequested = true;
  }
  serviceCv.notify_all();

  if (!waitForTasksToStop(kTaskStopTimeoutMs)) {
    LOG_ERR("METALIO-AUDIO", "Audio tasks did not stop cleanly");
  }

  std::deque<std::shared_ptr<PlaybackRequest>> dropped;

  {
    std::lock_guard<std::mutex> lock(serviceMutex);
    dropped.swap(playbackQueue);
    captureRequests.clear();
    captureQueue.clear();
    serviceStarted = false;
    stopRequested = false;
  }

  for (auto& request : dropped) {
    cancelPlayback(request);
  }

  LOG_INF("METALIO-AUDIO", "PCM audio service stopped");
}

bool queuePcm(const int16_t* samples, std::size_t count) {
  if (!samples || count == 0 || !ensureServiceStarted()) return false;

  auto request = std::make_shared<PlaybackRequest>();
  request->pcm.assign(samples, samples + count);

  {
    std::lock_guard<std::mutex> lock(serviceMutex);
    if (stopRequested || playbackQueue.size() >= kMaxPlaybackQueue) {
      return false;
    }
    playbackQueue.push_back(request);
  }

  serviceCv.notify_one();
  return true;
}

bool playPcm(const int16_t* samples, std::size_t count,
             uint32_t timeoutMs) {
  if (!samples || count == 0 || !ensureServiceStarted()) return false;

  auto request = std::make_shared<PlaybackRequest>();
  request->pcm.assign(samples, samples + count);

  {
    std::lock_guard<std::mutex> lock(serviceMutex);
    if (stopRequested || playbackQueue.size() >= kMaxPlaybackQueue) {
      return false;
    }
    playbackQueue.push_back(request);
  }

  serviceCv.notify_one();

  std::unique_lock<std::mutex> lock(request->mutex);
  if (!request->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                            [&request] { return request->completed; })) {
    request->cancelled = true;
    LOG_ERR("METALIO-AUDIO", "Playback request timed out");
    return false;
  }

  return request->success;
}

void flushPlayback() {
  std::deque<std::shared_ptr<PlaybackRequest>> dropped;

  {
    std::lock_guard<std::mutex> lock(serviceMutex);
    dropped.swap(playbackQueue);
  }

  for (auto& request : dropped) {
    cancelPlayback(request);
  }
}

bool startCapture() {
  return ensureServiceStarted();
}

void stopCapture() {
  std::lock_guard<std::mutex> lock(serviceMutex);
  captureRequests.clear();
  captureQueue.clear();
}

bool readCapturedPcm(std::vector<int16_t>& samples,
                     std::size_t maxSamples,
                     uint32_t timeoutMs) {
  samples.clear();
  if (maxSamples == 0 || !ensureServiceStarted()) return false;

  const std::size_t requested =
      std::min(maxSamples, kCaptureFrameSamples);

  {
    std::lock_guard<std::mutex> lock(serviceMutex);
    if (stopRequested || captureRequests.size() >= kMaxCaptureRequests) {
      return false;
    }
    captureRequests.push_back(CaptureRequest{requested});
  }
  serviceCv.notify_one();

  std::unique_lock<std::mutex> lock(serviceMutex);
  if (!serviceCv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [] {
        return stopRequested || !captureQueue.empty();
      })) {
    return false;
  }

  if (captureQueue.empty()) return false;

  samples = std::move(captureQueue.front().samples);
  captureQueue.pop_front();
  return true;
}

bool recordPcm(std::vector<int16_t>& samples,
               std::size_t count,
               uint32_t timeoutMs) {
  samples.clear();
  if (count == 0 || !ensureServiceStarted()) return false;

  samples.reserve(count);

  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds(timeoutMs);

  while (samples.size() < count) {
    const std::size_t wanted =
        std::min(kCaptureFrameSamples, count - samples.size());

    {
      std::lock_guard<std::mutex> lock(serviceMutex);
      if (stopRequested || captureRequests.size() >= kMaxCaptureRequests) {
        return false;
      }
      captureRequests.push_back(CaptureRequest{wanted});
    }
    serviceCv.notify_one();

    std::unique_lock<std::mutex> lock(serviceMutex);
    if (!serviceCv.wait_until(lock, deadline, [] {
          return stopRequested || !captureQueue.empty();
        })) {
      return false;
    }

    if (captureQueue.empty()) return false;

    auto frame = std::move(captureQueue.front().samples);
    captureQueue.pop_front();
    samples.insert(samples.end(), frame.begin(), frame.end());
  }

  samples.resize(count);
  return true;
}

void setOutputVolume(uint8_t volume) {
  outputCodec.setOutputVolume(volume);
}

uint8_t outputVolume() {
  return outputCodec.outputVolume();
}

void playTestTone() {
  constexpr uint32_t kRate = 16000;
  constexpr std::size_t kFrames = kRate / 2;
  constexpr double kPi = 3.14159265358979323846;
  constexpr double kFrequency = 1000.0;
  constexpr double kAmplitude = 8000.0;

  static std::vector<int16_t> tone(kFrames);
  static bool initialized = false;

  if (!initialized) {
    for (std::size_t i = 0; i < tone.size(); ++i) {
      tone[i] = static_cast<int16_t>(
          std::sin(2.0 * kPi * kFrequency * static_cast<double>(i) /
                   static_cast<double>(kRate)) *
          kAmplitude);
    }
    initialized = true;
  }

  LOG_INF("METALIO-AUDIO", "Playing local 1 kHz speaker test");
  (void)playPcm(tone.data(), tone.size(), 2000);
}

}  // namespace metalio_audio_service

#else

namespace metalio_audio_service {

bool start() { return false; }
void stop() {}

bool playPcm(const int16_t*, std::size_t, uint32_t) { return false; }
bool queuePcm(const int16_t*, std::size_t) { return false; }
void flushPlayback() {}

bool startCapture() { return false; }
void stopCapture() {}

bool readCapturedPcm(std::vector<int16_t>& samples,
                     std::size_t, uint32_t) {
  samples.clear();
  return false;
}

bool recordPcm(std::vector<int16_t>& samples, std::size_t, uint32_t) {
  samples.clear();
  return false;
}

void setOutputVolume(uint8_t) {}
uint8_t outputVolume() { return 0; }

void playTestTone() {}

}  // namespace metalio_audio_service

#endif
