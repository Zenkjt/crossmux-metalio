#include "MetalioAudioCodec.h"

#if FREEINK_DEVICE_METALIO_EINK4

#include <MetalioAudio.h>
#include <MetalioEInk4Board.h>
#include <driver/i2s_std.h>

#include <algorithm>
#include <array>

namespace metalio_audio {
namespace {

constexpr std::size_t kIoChunkSamples = 960;  // 60 ms @ 16 kHz

}  // namespace

int MetalioAudioCodec::outputOwnerToken_;
int MetalioAudioCodec::inputOwnerToken_;

MetalioAudioCodec::~MetalioAudioCodec() {
  stopInput();
  stopOutput();
}

bool MetalioAudioCodec::startOutput() {
  if (outputStarted_) return true;

  if (!freeink::metalio::acquireAudio(&outputOwnerToken_, false)) return false;
  if (!freeink::metalio::startAudio(&outputOwnerToken_, false)) {
    freeink::metalio::releaseAudio(&outputOwnerToken_, false);
    return false;
  }
  if (!freeink::metalio::setAmplifier(true)) {
    freeink::metalio::stopAudio(&outputOwnerToken_, false);
    freeink::metalio::releaseAudio(&outputOwnerToken_, false);
    return false;
  }

  outputStarted_ = true;
  return true;
}

void MetalioAudioCodec::stopOutput() {
  if (!outputStarted_) return;

  freeink::metalio::setAmplifier(false);
  freeink::metalio::stopAudio(&outputOwnerToken_, false);
  freeink::metalio::releaseAudio(&outputOwnerToken_, false);
  outputStarted_ = false;
}

bool MetalioAudioCodec::startInput() {
  if (inputStarted_) return true;

  if (!freeink::metalio::acquireAudio(&inputOwnerToken_, true)) return false;
  if (!freeink::metalio::startAudio(&inputOwnerToken_, true)) {
    freeink::metalio::releaseAudio(&inputOwnerToken_, true);
    return false;
  }

  inputStarted_ = true;
  return true;
}

void MetalioAudioCodec::stopInput() {
  if (!inputStarted_) return;

  freeink::metalio::stopAudio(&inputOwnerToken_, true);
  freeink::metalio::releaseAudio(&inputOwnerToken_, true);
  inputStarted_ = false;
}

std::size_t MetalioAudioCodec::write(const int16_t* samples, std::size_t count,
                                     uint32_t timeoutMs) {
  if (!outputStarted_ || samples == nullptr || count == 0) return 0;

  const std::size_t frames = std::min(count, kIoChunkSamples);
  std::array<int32_t, kIoChunkSamples * 2> stereo{};

  for (std::size_t i = 0; i < frames; ++i) {
    const int32_t pcm =
        freeink::metalio::outputSample(samples[i], outputVolume_);
    stereo[i * 2] = pcm;
    stereo[i * 2 + 1] = pcm;
  }

  auto& bus = freeink::metalio::audioBus();
  std::size_t bytesWritten = 0;
  const esp_err_t err = i2s_channel_write(
      bus.tx, stereo.data(), frames * 2 * sizeof(int32_t), &bytesWritten,
      pdMS_TO_TICKS(timeoutMs));
  if (err != ESP_OK) return 0;

  return std::min(frames, bytesWritten / (2 * sizeof(int32_t)));
}

std::size_t MetalioAudioCodec::read(int16_t* samples, std::size_t count,
                                    uint32_t timeoutMs) {
  if (!inputStarted_ || samples == nullptr || count == 0) return 0;

  const std::size_t frames = std::min(count, kIoChunkSamples);
  std::array<int32_t, kIoChunkSamples> pcm{};

  auto& bus = freeink::metalio::audioBus();
  std::size_t bytesRead = 0;
  const esp_err_t err = i2s_channel_read(
      bus.rx, pcm.data(), frames * sizeof(int32_t), &bytesRead,
      pdMS_TO_TICKS(timeoutMs));
  if (err != ESP_OK) return 0;

  const std::size_t readFrames =
      std::min(frames, bytesRead / sizeof(int32_t));
  for (std::size_t i = 0; i < readFrames; ++i) {
    samples[i] = freeink::metalio::inputSample(pcm[i]);
  }
  return readFrames;
}

void MetalioAudioCodec::setOutputVolume(uint8_t volume) {
  outputVolume_ = volume > 100 ? 100 : volume;
}

}  // namespace metalio_audio

#else

namespace metalio_audio {
int MetalioAudioCodec::outputOwnerToken_;
int MetalioAudioCodec::inputOwnerToken_;
MetalioAudioCodec::~MetalioAudioCodec() = default;
bool MetalioAudioCodec::startOutput() { return false; }
void MetalioAudioCodec::stopOutput() {}
bool MetalioAudioCodec::startInput() { return false; }
void MetalioAudioCodec::stopInput() {}
std::size_t MetalioAudioCodec::write(const int16_t*, std::size_t, uint32_t) {
  return 0;
}
std::size_t MetalioAudioCodec::read(int16_t*, std::size_t, uint32_t) {
  return 0;
}
void MetalioAudioCodec::setOutputVolume(uint8_t volume) {
  outputVolume_ = volume > 100 ? 100 : volume;
}
}  // namespace metalio_audio

#endif
