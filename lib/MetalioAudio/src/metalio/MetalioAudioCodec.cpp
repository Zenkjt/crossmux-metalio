#include "MetalioAudioCodec.h"

#if FREEINK_DEVICE_METALIO_EINK4

#include "MetalioAudio.h"

#include <Arduino.h>
#include <driver/i2s_std.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include <algorithm>

namespace {

constexpr const char* kTag = "METALIO-AUDIO-CODEC";

constexpr std::size_t kIoChunkSamples = 960;

constexpr uint32_t kAudioBufferCaps =
    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

}  // namespace

int MetalioAudioCodec::inputOwnerToken_ = 0;
int MetalioAudioCodec::outputOwnerToken_ = 0;

MetalioAudioCodec::MetalioAudioCodec() = default;

MetalioAudioCodec::~MetalioAudioCodec() {
  stopInput();
  stopOutput();
}

bool MetalioAudioCodec::startInput() {
  if (inputRunning_) {
    ESP_LOGD(kTag, "startInput: already running");
    return true;
  }

  if (inputBuffer_ == nullptr) {
    const std::size_t bytes =
        kIoChunkSamples * sizeof(int32_t);

    inputBuffer_ = static_cast<int32_t*>(
        heap_caps_malloc(bytes, kAudioBufferCaps));

    ESP_LOGI(
        kTag,
        "startInput: input buffer %u bytes, ptr=%p",
        static_cast<unsigned>(bytes),
        static_cast<void*>(inputBuffer_));

    if (inputBuffer_ == nullptr) {
      ESP_LOGE(
          kTag,
          "startInput: input buffer allocation FAILED");
      return false;
    }
  }

  ESP_LOGI(kTag, "startInput: acquireAudio(RX)");

  if (!freeink::metalio::acquireAudio(
          &inputOwnerToken_,
          true)) {
    ESP_LOGE(
        kTag,
        "startInput: acquireAudio(RX) FAILED");

    heap_caps_free(inputBuffer_);
    inputBuffer_ = nullptr;
    return false;
  }

  auto& bus = freeink::metalio::audioBus();

  ESP_LOGI(
      kTag,
      "startInput: RX handle=%p TX handle=%p",
      static_cast<void*>(bus.rx),
      static_cast<void*>(bus.tx));

  if (!freeink::metalio::startAudio(
          &inputOwnerToken_,
          true)) {
    ESP_LOGE(
        kTag,
        "startInput: startAudio(RX) FAILED");

    freeink::metalio::releaseAudio(
        &inputOwnerToken_,
        true);

    heap_caps_free(inputBuffer_);
    inputBuffer_ = nullptr;
    return false;
  }

  ESP_LOGI(
      kTag,
      "startInput: RX started successfully");

  inputRunning_ = true;
  return true;
}

void MetalioAudioCodec::stopInput() {
  if (inputRunning_) {
    ESP_LOGI(kTag, "stopInput: stopping RX");

    freeink::metalio::stopAudio(
        &inputOwnerToken_,
        true);

    freeink::metalio::releaseAudio(
        &inputOwnerToken_,
        true);

    inputRunning_ = false;
  }

  if (inputBuffer_ != nullptr) {
    heap_caps_free(inputBuffer_);
    inputBuffer_ = nullptr;

    ESP_LOGD(
        kTag,
        "stopInput: input buffer freed");
  }
}

bool MetalioAudioCodec::startOutput() {
  if (outputRunning_) {
    ESP_LOGD(kTag, "startOutput: already running");
    return true;
  }

  if (outputBuffer_ == nullptr) {
    const std::size_t bytes =
        kIoChunkSamples *
        2 *
        sizeof(int32_t);

    outputBuffer_ = static_cast<int32_t*>(
        heap_caps_malloc(bytes, kAudioBufferCaps));

    ESP_LOGI(
        kTag,
        "startOutput: output buffer %u bytes, ptr=%p",
        static_cast<unsigned>(bytes),
        static_cast<void*>(outputBuffer_));

    if (outputBuffer_ == nullptr) {
      ESP_LOGE(
          kTag,
          "startOutput: output buffer allocation FAILED");
      return false;
    }
  }

  ESP_LOGI(kTag, "startOutput: acquireAudio(TX)");

  if (!freeink::metalio::acquireAudio(
          &outputOwnerToken_,
          false)) {
    ESP_LOGE(
        kTag,
        "startOutput: acquireAudio(TX) FAILED");

    heap_caps_free(outputBuffer_);
    outputBuffer_ = nullptr;
    return false;
  }

  auto& bus = freeink::metalio::audioBus();

  ESP_LOGI(
      kTag,
      "startOutput: RX handle=%p TX handle=%p",
      static_cast<void*>(bus.rx),
      static_cast<void*>(bus.tx));

  if (!freeink::metalio::startAudio(
          &outputOwnerToken_,
          false)) {
    ESP_LOGE(
        kTag,
        "startOutput: startAudio(TX) FAILED");

    freeink::metalio::releaseAudio(
        &outputOwnerToken_,
        false);

    heap_caps_free(outputBuffer_);
    outputBuffer_ = nullptr;
    return false;
  }

  ESP_LOGI(
      kTag,
      "startOutput: TX started, enabling amplifier");

  if (!freeink::metalio::setAmplifier(true)) {
    ESP_LOGE(
        kTag,
        "startOutput: setAmplifier(true) FAILED");

    freeink::metalio::stopAudio(
        &outputOwnerToken_,
        false);

    freeink::metalio::releaseAudio(
        &outputOwnerToken_,
        false);

    heap_caps_free(outputBuffer_);
    outputBuffer_ = nullptr;
    return false;
  }

  ESP_LOGI(
      kTag,
      "startOutput: TX started successfully");

  outputRunning_ = true;
  return true;
}

void MetalioAudioCodec::stopOutput() {
  if (outputRunning_) {
    ESP_LOGI(
        kTag,
        "stopOutput: disabling amplifier");

    freeink::metalio::setAmplifier(false);

    ESP_LOGI(
        kTag,
        "stopOutput: stopping TX");

    freeink::metalio::stopAudio(
        &outputOwnerToken_,
        false);

    freeink::metalio::releaseAudio(
        &outputOwnerToken_,
        false);

    outputRunning_ = false;
  }

  if (outputBuffer_ != nullptr) {
    heap_caps_free(outputBuffer_);
    outputBuffer_ = nullptr;

    ESP_LOGD(
        kTag,
        "stopOutput: output buffer freed");
  }
}

std::size_t MetalioAudioCodec::read(
    int16_t* dest,
    std::size_t samples,
    uint32_t timeoutMs) {
  if (!inputRunning_ ||
      dest == nullptr ||
      samples == 0 ||
      inputBuffer_ == nullptr) {
    ESP_LOGW(
        kTag,
        "read: invalid state running=%d dest=%p "
        "samples=%u buffer=%p",
        inputRunning_,
        static_cast<void*>(dest),
        static_cast<unsigned>(samples),
        static_cast<void*>(inputBuffer_));

    return 0;
  }

  const std::size_t frames =
      std::min(samples, kIoChunkSamples);

  auto& bus = freeink::metalio::audioBus();

  if (bus.rx == nullptr) {
    ESP_LOGE(
        kTag,
        "read: RX handle is NULL");

    return 0;
  }

  std::size_t bytesRead = 0;

  const std::size_t requestedBytes =
      frames * sizeof(int32_t);

  const esp_err_t err =
      i2s_channel_read(
          bus.rx,
          inputBuffer_,
          requestedBytes,
          &bytesRead,
          pdMS_TO_TICKS(timeoutMs));

  if (err != ESP_OK) {
    ESP_LOGE(
        kTag,
        "i2s_channel_read FAILED: err=%s (0x%x) "
        "rx=%p enabled=%d requested=%u read=%u "
        "timeout=%u ms",
        esp_err_to_name(err),
        static_cast<unsigned>(err),
        static_cast<void*>(bus.rx),
        bus.rx != nullptr ? 1 : 0,
        static_cast<unsigned>(requestedBytes),
        static_cast<unsigned>(bytesRead),
        static_cast<unsigned>(timeoutMs));

    return 0;
  }

  if (bytesRead == 0) {
    ESP_LOGW(
        kTag,
        "i2s_channel_read returned 0 bytes: "
        "rx=%p requested=%u timeout=%u ms",
        static_cast<void*>(bus.rx),
        static_cast<unsigned>(requestedBytes),
        static_cast<unsigned>(timeoutMs));

    return 0;
  }

  const std::size_t count =
      std::min(
          frames,
          bytesRead / sizeof(int32_t));

  for (std::size_t i = 0; i < count; ++i) {
    dest[i] =
        freeink::metalio::inputSample(
            inputBuffer_[i]);
  }

  ESP_LOGD(
      kTag,
      "read: %u samples / %u bytes",
      static_cast<unsigned>(count),
      static_cast<unsigned>(bytesRead));

  return count;
}

std::size_t MetalioAudioCodec::write(
    const int16_t* data,
    std::size_t samples,
    uint32_t timeoutMs) {
  if (!outputRunning_ ||
      data == nullptr ||
      samples == 0 ||
      outputBuffer_ == nullptr) {
    ESP_LOGW(
        kTag,
        "write: invalid state running=%d data=%p "
        "samples=%u buffer=%p",
        outputRunning_,
        static_cast<const void*>(data),
        static_cast<unsigned>(samples),
        static_cast<void*>(outputBuffer_));

    return 0;
  }

  const std::size_t frames =
      std::min(samples, kIoChunkSamples);

  for (std::size_t i = 0; i < frames; ++i) {
    const int32_t pcm =
        freeink::metalio::outputSample(
            data[i],
            outputVolume_);

    outputBuffer_[i * 2] = pcm;
    outputBuffer_[i * 2 + 1] = pcm;
  }

  auto& bus = freeink::metalio::audioBus();

  if (bus.tx == nullptr) {
    ESP_LOGE(
        kTag,
        "write: TX handle is NULL");

    return 0;
  }

  std::size_t bytesWritten = 0;

  const std::size_t requestedBytes =
      frames *
      2 *
      sizeof(int32_t);

  const esp_err_t err =
      i2s_channel_write(
          bus.tx,
          outputBuffer_,
          requestedBytes,
          &bytesWritten,
          pdMS_TO_TICKS(timeoutMs));

  if (err != ESP_OK) {
    ESP_LOGE(
        kTag,
        "i2s_channel_write FAILED: err=%s (0x%x) "
        "tx=%p requested=%u written=%u "
        "timeout=%u ms",
        esp_err_to_name(err),
        static_cast<unsigned>(err),
        static_cast<void*>(bus.tx),
        static_cast<unsigned>(requestedBytes),
        static_cast<unsigned>(bytesWritten),
        static_cast<unsigned>(timeoutMs));

    return 0;
  }

  if (bytesWritten == 0) {
    ESP_LOGW(
        kTag,
        "i2s_channel_write returned 0 bytes: "
        "tx=%p requested=%u timeout=%u ms",
        static_cast<void*>(bus.tx),
        static_cast<unsigned>(requestedBytes),
        static_cast<unsigned>(timeoutMs));

    return 0;
  }

  const std::size_t writtenFrames =
      std::min(
          frames,
          bytesWritten /
              (2 * sizeof(int32_t)));

  ESP_LOGD(
      kTag,
      "write: %u frames / %u bytes",
      static_cast<unsigned>(writtenFrames),
      static_cast<unsigned>(bytesWritten));

  return writtenFrames;
}

void MetalioAudioCodec::setOutputVolume(
    uint8_t volume) {
  outputVolume_ =
      volume > 100 ? 100 : volume;

  ESP_LOGD(
      kTag,
      "output volume=%u",
      static_cast<unsigned>(outputVolume_));
}

uint8_t MetalioAudioCodec::outputVolume() const {
  return outputVolume_;
}

bool MetalioAudioCodec::inputRunning() const {
  return inputRunning_;
}

bool MetalioAudioCodec::outputRunning() const {
  return outputRunning_;
}

#else

int MetalioAudioCodec::inputOwnerToken_ = 0;
int MetalioAudioCodec::outputOwnerToken_ = 0;

MetalioAudioCodec::MetalioAudioCodec() = default;
MetalioAudioCodec::~MetalioAudioCodec() = default;

bool MetalioAudioCodec::startInput() {
  return false;
}

void MetalioAudioCodec::stopInput() {}

bool MetalioAudioCodec::startOutput() {
  return false;
}

void MetalioAudioCodec::stopOutput() {}

std::size_t MetalioAudioCodec::read(
    int16_t*,
    std::size_t,
    uint32_t) {
  return 0;
}

std::size_t MetalioAudioCodec::write(
    const int16_t*,
    std::size_t,
    uint32_t) {
  return 0;
}

void MetalioAudioCodec::setOutputVolume(
    uint8_t volume) {
  outputVolume_ =
      volume > 100 ? 100 : volume;
}

uint8_t MetalioAudioCodec::outputVolume() const {
  return outputVolume_;
}

bool MetalioAudioCodec::inputRunning() const {
  return false;
}

bool MetalioAudioCodec::outputRunning() const {
  return false;
}

#endif
