#include "MetalioAudioCodec.h"

#if FREEINK_DEVICE_METALIO_EINK4

#include "MetalioAudio.h"

#include <Arduino.h>
#include <driver/i2s_std.h>
#include <esp_heap_caps.h>

#include <algorithm>

namespace {

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
    return true;
  }

  if (inputBuffer_ == nullptr) {
    inputBuffer_ = static_cast<int32_t*>(
        heap_caps_malloc(
            kIoChunkSamples * sizeof(int32_t),
            kAudioBufferCaps));

    if (inputBuffer_ == nullptr) {
      return false;
    }
  }

  if (!freeink::metalio::acquireAudio(&inputOwnerToken_, true)) {
    heap_caps_free(inputBuffer_);
    inputBuffer_ = nullptr;
    return false;
  }

  if (!freeink::metalio::startAudio(&inputOwnerToken_, true)) {
    freeink::metalio::releaseAudio(&inputOwnerToken_, true);
    heap_caps_free(inputBuffer_);
    inputBuffer_ = nullptr;
    return false;
  }

  inputRunning_ = true;
  return true;
}

void MetalioAudioCodec::stopInput() {
  if (inputRunning_) {
    freeink::metalio::stopAudio(&inputOwnerToken_, true);
    freeink::metalio::releaseAudio(&inputOwnerToken_, true);
    inputRunning_ = false;
  }

  if (inputBuffer_ != nullptr) {
    heap_caps_free(inputBuffer_);
    inputBuffer_ = nullptr;
  }
}

bool MetalioAudioCodec::startOutput() {
  if (outputRunning_) {
    return true;
  }

  if (outputBuffer_ == nullptr) {
    outputBuffer_ = static_cast<int32_t*>(
        heap_caps_malloc(
            kIoChunkSamples * 2 * sizeof(int32_t),
            kAudioBufferCaps));

    if (outputBuffer_ == nullptr) {
      return false;
    }
  }

  if (!freeink::metalio::acquireAudio(&outputOwnerToken_, false)) {
    heap_caps_free(outputBuffer_);
    outputBuffer_ = nullptr;
    return false;
  }

  if (!freeink::metalio::startAudio(&outputOwnerToken_, false)) {
    freeink::metalio::releaseAudio(&outputOwnerToken_, false);
    heap_caps_free(outputBuffer_);
    outputBuffer_ = nullptr;
    return false;
  }

  if (!freeink::metalio::setAmplifier(true)) {
    freeink::metalio::stopAudio(&outputOwnerToken_, false);
    freeink::metalio::releaseAudio(&outputOwnerToken_, false);
    heap_caps_free(outputBuffer_);
    outputBuffer_ = nullptr;
    return false;
  }

  outputRunning_ = true;
  return true;
}

void MetalioAudioCodec::stopOutput() {
  if (outputRunning_) {
    freeink::metalio::setAmplifier(false);
    freeink::metalio::stopAudio(&outputOwnerToken_, false);
    freeink::metalio::releaseAudio(&outputOwnerToken_, false);
    outputRunning_ = false;
  }

  if (outputBuffer_ != nullptr) {
    heap_caps_free(outputBuffer_);
    outputBuffer_ = nullptr;
  }
}

std::size_t MetalioAudioCodec::read(int16_t* dest,
                                    std::size_t samples,
                                    uint32_t timeoutMs) {
  if (!inputRunning_ ||
      dest == nullptr ||
      samples == 0 ||
      inputBuffer_ == nullptr) {
    return 0;
  }

  const std::size_t frames =
      std::min(samples, kIoChunkSamples);

  auto& bus = freeink::metalio::audioBus();

  if (bus.rx == nullptr) {
    return 0;
  }

  std::size_t bytesRead = 0;

  const esp_err_t err = i2s_channel_read(
      bus.rx,
      inputBuffer_,
      frames * sizeof(int32_t),
      &bytesRead,
      pdMS_TO_TICKS(timeoutMs));

  if (err != ESP_OK) {
    return 0;
  }

  const std::size_t count =
      std::min(frames, bytesRead / sizeof(int32_t));

  for (std::size_t i = 0; i < count; ++i) {
    dest[i] = freeink::metalio::inputSample(inputBuffer_[i]);
  }

  return count;
}

std::size_t MetalioAudioCodec::write(const int16_t* data,
                                     std::size_t samples,
                                     uint32_t timeoutMs) {
  if (!outputRunning_ ||
      data == nullptr ||
      samples == 0 ||
      outputBuffer_ == nullptr) {
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
    return 0;
  }

  std::size_t bytesWritten = 0;

  const esp_err_t err = i2s_channel_write(
      bus.tx,
      outputBuffer_,
      frames * 2 * sizeof(int32_t),
      &bytesWritten,
      pdMS_TO_TICKS(timeoutMs));

  if (err != ESP_OK) {
    return 0;
  }

  return std::min(
      frames,
      bytesWritten / (2 * sizeof(int32_t)));
}

void MetalioAudioCodec::setOutputVolume(uint8_t volume) {
  outputVolume_ =
      volume > 100 ? 100 : volume;
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

void MetalioAudioCodec::setOutputVolume(uint8_t volume) {
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
