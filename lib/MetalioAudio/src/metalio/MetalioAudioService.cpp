#include "MetalioAudioService.h"

#if FREEINK_DEVICE_METALIO_EINK4

#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <deque>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "MetalioAudioCodec.h"
#include "MetalioOpusCodec.h"

namespace metalio_audio_service {
namespace {

constexpr std::size_t kMaxPlaybackQueue = 4;
constexpr std::size_t kMaxEncodeQueue = 4;
constexpr std::size_t kMaxDecodeQueue = 32;
constexpr std::size_t kMaxSendQueue = 32;
constexpr std::size_t kMaxCaptureFrames = 4;
constexpr std::size_t kFrameSamples = metalio_opus::kFrameSamples;

constexpr uint32_t kTaskStackWords = 4096;
constexpr UBaseType_t kInputPriority = 5;
constexpr UBaseType_t kOutputPriority = 4;
constexpr UBaseType_t kOpusPriority = 3;
constexpr uint32_t kStopTimeoutMs = 2000;

enum class EncodeTarget {
  SendQueue,
  Synchronous,
};

struct PlaybackRequest {
  std::vector<int16_t> pcm;
  bool done = false;
  bool success = false;
  bool cancelled = false;
  std::mutex mutex;
  std::condition_variable cv;
};

struct EncodeRequest {
  std::vector<int16_t> pcm;
  EncodeTarget target = EncodeTarget::SendQueue;
  std::vector<uint8_t> packet;
  bool done = false;
  bool success = false;
  std::mutex mutex;
  std::condition_variable cv;
};

struct DecodeRequest {
  std::vector<uint8_t> packet;
  std::vector<int16_t> pcm;
};


struct CaptureFrame {
  std::vector<int16_t> samples;
};

MetalioAudioCodec outputCodec;
MetalioAudioCodec inputCodec;
metalio_opus::Encoder opusEncoder;
metalio_opus::Decoder opusDecoder;

std::mutex queueMutex;
std::condition_variable queueCv;
std::mutex codecMutex;

std::deque<std::shared_ptr<PlaybackRequest>> playbackQueue;
std::deque<std::shared_ptr<EncodeRequest>> encodeQueue;
std::deque<DecodeRequest> decodeQueue;
std::deque<std::vector<uint8_t>> sendQueue;
std::deque<CaptureFrame> captureQueue;

TaskHandle_t inputTaskHandle = nullptr;
TaskHandle_t outputTaskHandle = nullptr;
TaskHandle_t opusTaskHandle = nullptr;

bool started = false;
bool stopRequested = false;
bool captureRunning = false;
bool captureEncode = false;

void finishEncode(const std::shared_ptr<EncodeRequest>& request, bool success) {
  {
    std::lock_guard<std::mutex> lock(request->mutex);
    if (request->done) return;
    request->success = success;
    request->done = true;
  }
  request->cv.notify_all();
}

void finishPlayback(const std::shared_ptr<PlaybackRequest>& request,
                    bool success) {
  {
    std::lock_guard<std::mutex> lock(request->mutex);
    if (request->done) return;
    request->success = success;
    request->done = true;
  }
  request->cv.notify_all();
}

bool isCancelled(const std::shared_ptr<PlaybackRequest>& request) {
  std::lock_guard<std::mutex> lock(request->mutex);
  return request->cancelled;
}

void cancelPlayback(const std::shared_ptr<PlaybackRequest>& request) {
  {
    std::lock_guard<std::mutex> lock(request->mutex);
    request->cancelled = true;
  }
  finishPlayback(request, false);
}

void inputTask(void*) {
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(queueMutex);
      queueCv.wait_for(lock, std::chrono::milliseconds(100), [] {
        return stopRequested || captureRunning;
      });
      if (stopRequested) break;
      if (!captureRunning) {
        continue;
      }
    }

    if (!inputCodec.startInput()) {
      LOG_ERR("METALIO-AUDIO", "microphone start failed");
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    std::vector<int16_t> pcm(kFrameSamples);
    std::size_t offset = 0;
    while (offset < kFrameSamples) {
      const std::size_t n = inputCodec.read(
          pcm.data() + offset, kFrameSamples - offset, 1000);
      if (n == 0) break;
      offset += n;
    }

    if (offset != kFrameSamples) {
      continue;
    }

    {
      std::lock_guard<std::mutex> lock(queueMutex);
      if (captureQueue.size() >= kMaxCaptureFrames) {
        captureQueue.pop_front();
      }
      captureQueue.push_back(CaptureFrame{std::move(pcm)});

      if (captureEncode && encodeQueue.size() < kMaxEncodeQueue) {
        auto request = std::make_shared<EncodeRequest>();
        request->target = EncodeTarget::SendQueue;
        request->pcm = captureQueue.back().samples;
        encodeQueue.push_back(std::move(request));
      }
    }
    queueCv.notify_all();
  }

  inputCodec.stopInput();
  {
    std::lock_guard<std::mutex> lock(queueMutex);
    inputTaskHandle = nullptr;
  }
  queueCv.notify_all();
  vTaskDelete(nullptr);
}

void outputTask(void*) {
  for (;;) {
    std::shared_ptr<PlaybackRequest> request;

    {
      std::unique_lock<std::mutex> lock(queueMutex);
      queueCv.wait(lock, [] {
        return stopRequested || !playbackQueue.empty();
      });
      if (playbackQueue.empty() && stopRequested) break;

      if (!playbackQueue.empty()) {
        request = std::move(playbackQueue.front());
        playbackQueue.pop_front();
      }
    }

    if (!request) continue;
    if (isCancelled(request)) {
      finishPlayback(request, false);
      continue;
    }

    bool success = outputCodec.startOutput();
    if (success) {
      std::size_t offset = 0;
      while (offset < request->pcm.size()) {
        if (isCancelled(request)) {
          success = false;
          break;
        }

        const std::size_t written = outputCodec.write(
            request->pcm.data() + offset,
            request->pcm.size() - offset, 1000);
        if (written == 0) {
          success = false;
          break;
        }
        offset += written;
      }
      outputCodec.stopOutput();
    }

    finishPlayback(request, success);
  }

  outputCodec.stopOutput();
  {
    std::lock_guard<std::mutex> lock(queueMutex);
    outputTaskHandle = nullptr;
  }
  queueCv.notify_all();
  vTaskDelete(nullptr);
}

void opusTask(void*) {
  for (;;) {
    std::shared_ptr<EncodeRequest> encode;
    DecodeRequest decode;
    bool doEncode = false;
    bool doDecode = false;

    {
      std::unique_lock<std::mutex> lock(queueMutex);
      queueCv.wait(lock, [] {
        return stopRequested ||
               (!encodeQueue.empty() && sendQueue.size() < kMaxSendQueue) ||
               (!decodeQueue.empty() && playbackQueue.size() < kMaxPlaybackQueue);
      });

      if (stopRequested) break;

      if (!decodeQueue.empty() &&
          playbackQueue.size() < kMaxPlaybackQueue) {
        decode = std::move(decodeQueue.front());
        decodeQueue.pop_front();
        doDecode = true;
      } else if (!encodeQueue.empty() &&
                 sendQueue.size() < kMaxSendQueue) {
        encode = std::move(encodeQueue.front());
        encodeQueue.pop_front();
        doEncode = true;
      }
    }

    if (doDecode) {
      std::vector<int16_t> pcm;
      bool ok = false;
      {
        std::lock_guard<std::mutex> lock(codecMutex);
        ok = opusDecoder.decode(decode.packet.data(), decode.packet.size(), pcm);
      }
      if (ok && !pcm.empty()) {
        auto request = std::make_shared<PlaybackRequest>();
        request->pcm = std::move(pcm);
        {
          std::lock_guard<std::mutex> lock(queueMutex);
          if (!stopRequested &&
              playbackQueue.size() < kMaxPlaybackQueue) {
            playbackQueue.push_back(std::move(request));
          }
        }
        queueCv.notify_all();
      } else {
        LOG_ERR("METALIO-AUDIO", "Opus decode failed");
      }
      continue;
    }

    if (doEncode) {
      std::vector<uint8_t> packet;
      bool ok = false;
      {
        std::lock_guard<std::mutex> lock(codecMutex);
        ok = opusEncoder.encode(encode->pcm.data(), encode->pcm.size(), packet);
      }

      if (encode->target == EncodeTarget::Synchronous) {
        {
          std::lock_guard<std::mutex> lock(encode->mutex);
          encode->packet = std::move(packet);
          encode->success = ok;
          encode->done = true;
        }
        encode->cv.notify_all();
      } else if (ok) {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (sendQueue.size() >= kMaxSendQueue) {
          sendQueue.pop_front();
        }
        sendQueue.push_back(std::move(packet));
      }
      queueCv.notify_all();
    }
  }

  {
    std::lock_guard<std::mutex> lock(queueMutex);
    opusTaskHandle = nullptr;
  }
  queueCv.notify_all();
  vTaskDelete(nullptr);
}

bool waitTasksStopped(uint32_t timeoutMs) {
  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds(timeoutMs);

  std::unique_lock<std::mutex> lock(queueMutex);
  return queueCv.wait_until(lock, deadline, [] {
    return inputTaskHandle == nullptr &&
           outputTaskHandle == nullptr &&
           opusTaskHandle == nullptr;
  });
}

bool ensureStarted() {
  {
    std::lock_guard<std::mutex> lock(queueMutex);
    if (started) return true;
    stopRequested = false;
  }

  TaskHandle_t input = nullptr;
  if (xTaskCreate(inputTask, "metalio_audio_in", kTaskStackWords, nullptr,
                  kInputPriority, &input) != pdPASS) {
    LOG_ERR("METALIO-AUDIO", "failed to create input task");
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(queueMutex);
    inputTaskHandle = input;
  }

  TaskHandle_t output = nullptr;
  if (xTaskCreate(outputTask, "metalio_audio_out", kTaskStackWords, nullptr,
                  kOutputPriority, &output) != pdPASS) {
    LOG_ERR("METALIO-AUDIO", "failed to create output task");
    {
      std::lock_guard<std::mutex> lock(queueMutex);
      stopRequested = true;
    }
    queueCv.notify_all();
    (void)waitTasksStopped(kStopTimeoutMs);
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(queueMutex);
    outputTaskHandle = output;
  }

  TaskHandle_t opus = nullptr;
  if (xTaskCreate(opusTask, "metalio_opus", kTaskStackWords, nullptr,
                  kOpusPriority, &opus) != pdPASS) {
    LOG_ERR("METALIO-AUDIO", "failed to create Opus task");
    {
      std::lock_guard<std::mutex> lock(queueMutex);
      stopRequested = true;
    }
    queueCv.notify_all();
    (void)waitTasksStopped(kStopTimeoutMs);
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(queueMutex);
    opusTaskHandle = opus;
    started = true;
  }

  LOG_INF("METALIO-AUDIO",
          "audio service started: input/output/Opus tasks");
  queueCv.notify_all();
  return true;
}

}  // namespace

bool start() {
  return ensureStarted();
}

void stop() {
  {
    std::lock_guard<std::mutex> lock(queueMutex);
    if (!started) return;
    stopRequested = true;
  }
  queueCv.notify_all();

  if (!waitTasksStopped(kStopTimeoutMs)) {
    LOG_ERR("METALIO-AUDIO", "audio tasks did not stop cleanly");
  }

  std::deque<std::shared_ptr<PlaybackRequest>> dropped;
  std::deque<std::shared_ptr<EncodeRequest>> droppedEncode;

  {
    std::lock_guard<std::mutex> lock(queueMutex);
    dropped.swap(playbackQueue);
    droppedEncode.swap(encodeQueue);
    decodeQueue.clear();
    sendQueue.clear();
    captureQueue.clear();
    captureRunning = false;
    captureEncode = false;
    started = false;
    stopRequested = false;
  }

  for (auto& request : dropped) cancelPlayback(request);
  for (auto& request : droppedEncode) finishEncode(request, false);

  opusEncoder.reset();
  opusDecoder.reset();
  LOG_INF("METALIO-AUDIO", "audio service stopped");
}

bool queuePcm(const int16_t* samples, std::size_t count) {
  if (samples == nullptr || count == 0 || !ensureStarted()) return false;

  auto request = std::make_shared<PlaybackRequest>();
  request->pcm.assign(samples, samples + count);

  {
    std::lock_guard<std::mutex> lock(queueMutex);
    if (stopRequested || playbackQueue.size() >= kMaxPlaybackQueue) {
      return false;
    }
    playbackQueue.push_back(request);
  }
  queueCv.notify_all();
  return true;
}

bool playPcm(const int16_t* samples, std::size_t count,
             uint32_t timeoutMs) {
  if (!samples || count == 0 || !ensureStarted()) return false;

  auto request = std::make_shared<PlaybackRequest>();
  request->pcm.assign(samples, samples + count);

  {
    std::lock_guard<std::mutex> lock(queueMutex);
    if (stopRequested || playbackQueue.size() >= kMaxPlaybackQueue) {
      return false;
    }
    playbackQueue.push_back(request);
  }
  queueCv.notify_all();

  std::unique_lock<std::mutex> lock(request->mutex);
  if (!request->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                           [&request] { return request->done; })) {
    request->cancelled = true;
    LOG_ERR("METALIO-AUDIO", "PCM playback timed out");
    return false;
  }
  return request->success;
}

void flushPlayback() {
  std::deque<std::shared_ptr<PlaybackRequest>> dropped;
  {
    std::lock_guard<std::mutex> lock(queueMutex);
    dropped.swap(playbackQueue);
  }
  for (auto& request : dropped) cancelPlayback(request);
}

bool startCapture(bool encodeToOpus) {
  if (!ensureStarted()) return false;
  {
    std::lock_guard<std::mutex> lock(queueMutex);
    captureRunning = true;
    captureEncode = encodeToOpus;
  }
  queueCv.notify_all();
  return true;
}

void stopCapture() {
  {
    std::lock_guard<std::mutex> lock(queueMutex);
    captureRunning = false;
    captureEncode = false;
    captureQueue.clear();
  }
  inputCodec.stopInput();
  queueCv.notify_all();
}

bool readCapturedPcm(std::vector<int16_t>& samples,
                     std::size_t maxSamples,
                     uint32_t timeoutMs) {
  samples.clear();
  if (maxSamples == 0 || !ensureStarted()) return false;

  if (!startCapture(false)) return false;

  std::unique_lock<std::mutex> lock(queueMutex);
  if (!queueCv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [] {
        return stopRequested || !captureQueue.empty();
      })) {
    return false;
  }
  if (captureQueue.empty()) return false;

  samples = std::move(captureQueue.front().samples);
  captureQueue.pop_front();
  if (samples.size() > maxSamples) samples.resize(maxSamples);
  return true;
}

bool recordPcm(std::vector<int16_t>& samples,
               std::size_t count,
               uint32_t timeoutMs) {
  samples.clear();
  if (count == 0 || !ensureStarted()) return false;
  if (!startCapture(false)) return false;

  samples.reserve(count);
  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds(timeoutMs);

  while (samples.size() < count) {
    std::unique_lock<std::mutex> lock(queueMutex);
    if (!queueCv.wait_until(lock, deadline, [] {
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

bool encodePcmFrame(const int16_t* samples, std::size_t count,
                    std::vector<uint8_t>& packet) {
  packet.clear();
  if (!samples || count != kFrameSamples || !ensureStarted()) return false;

  auto request = std::make_shared<EncodeRequest>();
  request->target = EncodeTarget::Synchronous;
  request->pcm.assign(samples, samples + count);

  {
    std::lock_guard<std::mutex> lock(queueMutex);
    if (stopRequested || encodeQueue.size() >= kMaxEncodeQueue) {
      return false;
    }
    encodeQueue.push_back(request);
  }
  queueCv.notify_all();

  std::unique_lock<std::mutex> lock(request->mutex);
  if (!request->cv.wait_for(lock, std::chrono::milliseconds(2000),
                           [&request] { return request->done; })) {
    return false;
  }
  if (!request->success) return false;
  packet = std::move(request->packet);
  return true;
}

bool decodeOpusPacket(const uint8_t* packet, std::size_t packetBytes,
                      std::vector<int16_t>& samples) {
  samples.clear();
  if (!packet || packetBytes == 0 || !ensureStarted()) return false;

  std::lock_guard<std::mutex> lock(codecMutex);
  return opusDecoder.decode(packet, packetBytes, samples);
}


bool pushOpusPacket(const uint8_t* packet, std::size_t packetBytes, bool wait) {
  if (!packet || packetBytes == 0 || !ensureStarted()) return false;

  std::unique_lock<std::mutex> lock(queueMutex);
  if (wait) {
    queueCv.wait(lock, [] {
      return stopRequested || decodeQueue.size() < kMaxDecodeQueue;
    });
  } else if (decodeQueue.size() >= kMaxDecodeQueue) {
    return false;
  }

  if (stopRequested) return false;
  decodeQueue.push_back(
      DecodeRequest{std::vector<uint8_t>(packet, packet + packetBytes), {}});
  lock.unlock();
  queueCv.notify_all();
  return true;
}

bool popEncodedPacket(std::vector<uint8_t>& packet) {
  packet.clear();
  std::lock_guard<std::mutex> lock(queueMutex);
  if (sendQueue.empty()) return false;
  packet = std::move(sendQueue.front());
  sendQueue.pop_front();
  queueCv.notify_all();
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

  static std::array<int16_t, kFrames> tone{};
  static bool initialized = false;

  if (!initialized) {
    for (std::size_t i = 0; i < tone.size(); ++i) {
      tone[i] = static_cast<int16_t>(
          std::sin(2.0 * kPi * static_cast<double>(i) /
                   static_cast<double>(kRate) * kFrequency) *
          kAmplitude);
    }
    initialized = true;
  }

  LOG_INF("METALIO-AUDIO", "local 1 kHz speaker test");
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
bool startCapture(bool) { return false; }
void stopCapture() {}
bool readCapturedPcm(std::vector<int16_t>& s, std::size_t, uint32_t) {
  s.clear(); return false;
}
bool recordPcm(std::vector<int16_t>& s, std::size_t, uint32_t) {
  s.clear(); return false;
}
bool encodePcmFrame(const int16_t*, std::size_t, std::vector<uint8_t>& p) {
  p.clear(); return false;
}
bool decodeOpusPacket(const uint8_t*, std::size_t, std::vector<int16_t>& s) {
  s.clear(); return false;
}
bool pushOpusPacket(const uint8_t*, std::size_t, bool) { return false; }
bool popEncodedPacket(std::vector<uint8_t>& p) { p.clear(); return false; }
void setOutputVolume(uint8_t) {}
uint8_t outputVolume() { return 0; }
void playTestTone() {}
}  // namespace metalio_audio_service

#endif
