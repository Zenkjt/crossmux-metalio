#pragma once

#include <cstddef>
#include <cstdint>

namespace metalio_audio {

class MetalioAudioCodec {
 public:
  MetalioAudioCodec() = default;
  ~MetalioAudioCodec();

  MetalioAudioCodec(const MetalioAudioCodec&) = delete;
  MetalioAudioCodec& operator=(const MetalioAudioCodec&) = delete;

  bool startOutput();
  void stopOutput();
  bool startInput();
  void stopInput();

  std::size_t write(const int16_t* samples, std::size_t count,
                    uint32_t timeoutMs = 1000);
  std::size_t read(int16_t* samples, std::size_t count,
                   uint32_t timeoutMs = 1000);

  void setOutputVolume(uint8_t volume);
  uint8_t outputVolume() const { return outputVolume_; }

 private:
  static int outputOwnerToken_;
  static int inputOwnerToken_;
  uint8_t outputVolume_ = 70;
  bool outputStarted_ = false;
  bool inputStarted_ = false;
};

}  // namespace metalio_audio
