#include "MetalioAudioService.h"

#if FREEINK_DEVICE_METALIO_EINK4

#include <Logging.h>
#include <MetalioEInk4Board.h>
#include <MetalioAudio.h>
#include <driver/i2s_std.h>
#include <freertos/FreeRTOS.h>

namespace metalio_audio_service {
namespace {
constexpr uint32_t kSampleRate = 16000;
constexpr uint8_t kVolume = 30;
constexpr int16_t kAmplitude = 8000;
constexpr size_t kFramesPerBuffer = 160;
constexpr size_t kStereoSamplesPerBuffer = kFramesPerBuffer * 2;
constexpr int kToneBuffers = 50;
constexpr int kHalfPeriodSamples = 8;
static int32_t toneBuffer[kStereoSamplesPerBuffer];

void fillToneBuffer(size_t firstSample) {
  for (size_t frame = 0; frame < kFramesPerBuffer; ++frame) {
    const size_t sampleIndex = firstSample + frame;
    const int16_t sample = ((sampleIndex / kHalfPeriodSamples) & 1U) ? -kAmplitude : kAmplitude;
    const int32_t pcm = freeink::metalio::outputSample(sample, kVolume);
    toneBuffer[frame * 2] = pcm;
    toneBuffer[frame * 2 + 1] = pcm;
  }
}
}  // namespace

void playTestTone() {
  LOG_INF("METALIO-AUDIO", "Starting local speaker hardware test");
  static int owner;
  if (!freeink::metalio::acquireAudio(&owner, false)) {
    LOG_ERR("METALIO-AUDIO", "Failed to acquire Metalio speaker audio bus");
    return;
  }
  auto& bus = freeink::metalio::audioBus();
  if (!freeink::metalio::startAudio(&owner, false)) {
    LOG_ERR("METALIO-AUDIO", "Failed to start Metalio I2S TX");
    freeink::metalio::releaseAudio(&owner, false);
    return;
  }
  if (!freeink::metalio::setAmplifier(true)) {
    LOG_ERR("METALIO-AUDIO", "Failed to enable local speaker amplifier");
    freeink::metalio::stopAudio(&owner, false);
    freeink::metalio::releaseAudio(&owner, false);
    return;
  }
  bool ok = true;
  for (int bufferIndex = 0; bufferIndex < kToneBuffers; ++bufferIndex) {
    fillToneBuffer(static_cast<size_t>(bufferIndex) * kFramesPerBuffer);
    size_t bytesWritten = 0;
    const esp_err_t err = i2s_channel_write(bus.tx, toneBuffer, sizeof(toneBuffer), &bytesWritten, pdMS_TO_TICKS(100));
    if (err != ESP_OK || bytesWritten != sizeof(toneBuffer)) {
      LOG_ERR("METALIO-AUDIO", "I2S write failed: err=%d bytes=%u/%u", static_cast<int>(err), static_cast<unsigned>(bytesWritten), static_cast<unsigned>(sizeof(toneBuffer)));
      ok = false;
      break;
    }
  }
  freeink::metalio::setAmplifier(false);
  freeink::metalio::stopAudio(&owner, false);
  freeink::metalio::releaseAudio(&owner, false);
  LOG_INF("METALIO-AUDIO", "Local speaker test %s", ok ? "completed" : "failed");
}
}  // namespace metalio_audio_service
#else
namespace metalio_audio_service { void playTestTone() {} }
#endif
