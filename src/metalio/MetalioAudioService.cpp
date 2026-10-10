#include "MetalioAudioService.h"

#if FREEINK_DEVICE_METALIO_EINK4

#include <Logging.h>

#include <cmath>
#include <vector>

#include "MetalioAudioCodec.h"

namespace metalio_audio_service {
namespace {

MetalioAudioCodec codec;

}  // namespace

bool start() {
  return codec.startOutput();
}

void stop() {
  codec.stopOutput();
}

bool playPcm(const int16_t* samples, std::size_t count, uint32_t timeoutMs) {
  if (samples == nullptr || count == 0) return false;

  if (!codec.startOutput()) {
    LOG_ERR("METALIO-AUDIO", "Failed to start Metalio speaker codec");
    return false;
  }

  std::size_t offset = 0;
  while (offset < count) {
    const std::size_t written = codec.write(samples + offset, count - offset, timeoutMs);
    if (written == 0) {
      LOG_ERR("METALIO-AUDIO", "Metalio I2S output stalled at %u/%u samples",
              static_cast<unsigned>(offset), static_cast<unsigned>(count));
      codec.stopOutput();
      return false;
    }
    offset += written;
  }

  codec.stopOutput();
  return true;
}

bool recordPcm(std::vector<int16_t>& samples, std::size_t count, uint32_t timeoutMs) {
  samples.resize(count);

  if (!codec.startInput()) {
    LOG_ERR("METALIO-AUDIO", "Failed to start Metalio microphone codec");
    samples.clear();
    return false;
  }

  std::size_t offset = 0;
  while (offset < count) {
    const std::size_t read = codec.read(samples.data() + offset, count - offset, timeoutMs);
    if (read == 0) {
      LOG_ERR("METALIO-AUDIO", "Metalio I2S input stalled at %u/%u samples",
              static_cast<unsigned>(offset), static_cast<unsigned>(count));
      codec.stopInput();
      samples.clear();
      return false;
    }
    offset += read;
  }

  codec.stopInput();
  return true;
}

void setOutputVolume(uint8_t volume) {
  codec.setOutputVolume(volume);
}

uint8_t outputVolume() {
  return codec.outputVolume();
}

void playTestTone() {
  constexpr uint32_t kSampleRate = 16000;
  constexpr std::size_t kFrames = kSampleRate / 2;
  constexpr double kFrequency = 1000.0;
  constexpr double kPi = 3.14159265358979323846;
  constexpr double kAmplitude = 8000.0;

  static std::vector<int16_t> tone(kFrames);
  for (std::size_t i = 0; i < tone.size(); ++i) {
    tone[i] = static_cast<int16_t>(
        std::sin(2.0 * kPi * kFrequency * static_cast<double>(i) / kSampleRate) *
        kAmplitude);
  }

  LOG_INF("METALIO-AUDIO", "Playing local 1 kHz speaker test");
  playPcm(tone.data(), tone.size(), 1000);
}

}  // namespace metalio_audio_service

#else

namespace metalio_audio_service {
bool start() { return false; }
void stop() {}
bool playPcm(const int16_t*, std::size_t, uint32_t) { return false; }
bool recordPcm(std::vector<int16_t>& samples, std::size_t, uint32_t) {
  samples.clear();
  return false;
}
void setOutputVolume(uint8_t) {}
uint8_t outputVolume() { return 0; }
void playTestTone() {}
}  // namespace metalio_audio_service

#endif
