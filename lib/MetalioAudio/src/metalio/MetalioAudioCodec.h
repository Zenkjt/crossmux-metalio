#pragma once

#include <cstddef>
#include <cstdint>

class MetalioAudioCodec {
 public:
  MetalioAudioCodec();
  ~MetalioAudioCodec();

  MetalioAudioCodec(const MetalioAudioCodec&) = delete;
  MetalioAudioCodec& operator=(const MetalioAudioCodec&) = delete;

  bool startInput();
  void stopInput();

  bool startOutput();
  void stopOutput();

  std::size_t read(int16_t* dest,
                   std::size_t samples,
                   uint32_t timeoutMs = 1000);

  std::size_t write(const int16_t* data,
                    std::size_t samples,
                    uint32_t timeoutMs = 1000);

  void setOutputVolume(uint8_t volume);
  uint8_t outputVolume() const;

  bool inputRunning() const;
  bool outputRunning() const;

 private:
  static int inputOwnerToken_;
  static int outputOwnerToken_;

  bool inputRunning_ = false;
  bool outputRunning_ = false;
  uint8_t outputVolume_ = 70;

  int32_t* inputBuffer_ = nullptr;
  int32_t* outputBuffer_ = nullptr;
};
