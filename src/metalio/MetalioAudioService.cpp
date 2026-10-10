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

#include "MetalioAudioCodec.h"

namespace metalio_audio_service {
namespace {

constexpr std::size_t kMaxPlaybackQueue = 4;
constexpr uint32_t kWorkerPollMs = 5;

struct PlaybackRequest {
  std::vector<int16_t> pcm;
  bool done = false;
  bool success = false;
  std::mutex mutex;
  std::condition_variable cv;
};

MetalioAudioCodec codec;
std::mutex serviceMutex;
std::deque<std::shared_ptr<PlaybackRequest>> playbackQueue;
TaskHandle_t outputTaskHandle = nullptr;
bool serviceStarted = false;
bool stopRequested = false;

void outputTask(void*) {
  for (;;) {
    std::shared_ptr<PlaybackRequest> request;
    {
      std::lock_guard<std::mutex> lock(serviceMutex);
      if (!playbackQueue.empty()) {
        request = playbackQueue.front();
        playbackQueue.pop_front();
      } else if (stopRequested) {
        break;
      }
    }

    if (!request) {
      vTaskDelay(pdMS_TO_TICKS(kWorkerPollMs));
      continue;
    }

    bool success = codec.startOutput();
    if (success) {
      std::size_t offset = 0;
      while (offset < request->pcm.size()) {
        const std::size_t written =
            codec.write(request->pcm.data() + offset,
                        request->pcm.size() - offset, 1000);
        if (written == 0) {
          success = false;
          break;
        }
        offset += written;
      }
      codec.stopOutput();
    }

    {
      std::lock_guard<std::mutex> lock(request->mutex);
      request->success = success;
      request->done = true;
    }
    request->cv.notify_one();
  }

  codec.stopOutput();
  outputTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

}  // namespace

bool start() {
  std::lock_guard<std::mutex> lock(serviceMutex);
  if (serviceStarted) return true;

  stopRequested = false;
  if (xTaskCreate(outputTask, "metalio_audio", 4096, nullptr, 4,
                  &outputTaskHandle) != pdPASS) {
    outputTaskHandle = nullptr;
    LOG_ERR("METALIO-AUDIO", "Failed to create audio output task");
    return false;
  }

  serviceStarted = true;
  LOG_INF("METALIO-AUDIO", "Audio service started");
  return true;
}

void stop() {
  {
    std::lock_guard<std::mutex> lock(serviceMutex);
    if (!serviceStarted) return;
    stopRequested = true;
  }

  for (int i = 0; i < 200 && outputTaskHandle != nullptr; ++i)
    vTaskDelay(pdMS_TO_TICKS(5));

  {
    std::lock_guard<std::mutex> lock(serviceMutex);
    playbackQueue.clear();
    serviceStarted = false;
    stopRequested = false;
  }
  LOG_INF("METALIO-AUDIO", "Audio service stopped");
}

bool queuePcm(const int16_t* samples, std::size_t count) {
  if (!samples || !count || !start()) return false;

  auto request = std::make_shared<PlaybackRequest>();
  request->pcm.assign(samples, samples + count);

  std::lock_guard<std::mutex> lock(serviceMutex);
  if (playbackQueue.size() >= kMaxPlaybackQueue) return false;
  playbackQueue.push_back(std::move(request));
  return true;
}

bool playPcm(const int16_t* samples, std::size_t count, uint32_t timeoutMs) {
  if (!samples || !count || !start()) return false;

  auto request = std::make_shared<PlaybackRequest>();
  request->pcm.assign(samples, samples + count);

  {
    std::lock_guard<std::mutex> lock(serviceMutex);
    if (playbackQueue.size() >= kMaxPlaybackQueue) return false;
    playbackQueue.push_back(request);
  }

  std::unique_lock<std::mutex> lock(request->mutex);
  if (!request->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                            [&request] { return request->done; })) {
    LOG_ERR("METALIO-AUDIO", "Playback request timed out");
    return false;
  }
  return request->success;
}

bool recordPcm(std::vector<int16_t>& samples, std::size_t count,
               uint32_t timeoutMs) {
  samples.resize(count);
  MetalioAudioCodec inputCodec;
  if (!inputCodec.startInput()) {
    LOG_ERR("METALIO-AUDIO", "Failed to start Metalio microphone codec");
    samples.clear();
    return false;
  }

  std::size_t offset = 0;
  while (offset < count) {
    const std::size_t read =
        inputCodec.read(samples.data() + offset, count - offset, timeoutMs);
    if (!read) {
      inputCodec.stopInput();
      samples.clear();
      return false;
    }
    offset += read;
  }
  inputCodec.stopInput();
  return true;
}

void flushPlayback() {
  std::lock_guard<std::mutex> lock(serviceMutex);
  playbackQueue.clear();
}

void setOutputVolume(uint8_t volume) {
  std::lock_guard<std::mutex> lock(serviceMutex);
  codec.setOutputVolume(volume);
}

uint8_t outputVolume() {
  std::lock_guard<std::mutex> lock(serviceMutex);
  return codec.outputVolume();
}

void playTestTone() {
  constexpr uint32_t kRate = 16000;
  constexpr std::size_t kFrames = kRate / 2;
  constexpr double kPi = 3.14159265358979323846;
  static std::vector<int16_t> tone(kFrames);

  for (std::size_t i = 0; i < tone.size(); ++i)
    tone[i] = static_cast<int16_t>(
        std::sin(2.0 * kPi * 1000.0 * i / kRate) * 8000.0);

  LOG_INF("METALIO-AUDIO", "Playing local 1 kHz speaker test");
  playPcm(tone.data(), tone.size(), 2000);
}

}  // namespace metalio_audio_service

#else

namespace metalio_audio_service {
bool start() { return false; }
void stop() {}
bool playPcm(const int16_t*, std::size_t, uint32_t) { return false; }
bool recordPcm(std::vector<int16_t>& s, std::size_t, uint32_t) {
  s.clear(); return false;
}
bool queuePcm(const int16_t*, std::size_t) { return false; }
void flushPlayback() {}
void setOutputVolume(uint8_t) {}
uint8_t outputVolume() { return 0; }
void playTestTone() {}
}

#endif
