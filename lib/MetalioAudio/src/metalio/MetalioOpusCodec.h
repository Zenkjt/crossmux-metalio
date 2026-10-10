#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace metalio_opus {

constexpr int kSampleRate = 16000;
constexpr int kChannels = 1;
constexpr int kFrameDurationMs = 60;
constexpr int kFrameSamples = kSampleRate * kFrameDurationMs / 1000;
constexpr std::size_t kMaxPacketBytes = 1275;
constexpr int kMaxDecoderSamples = 5760;

class Encoder {
 public:
  Encoder();
  ~Encoder();

  Encoder(const Encoder&) = delete;
  Encoder& operator=(const Encoder&) = delete;

  bool encode(const int16_t* pcm, std::size_t samples,
              std::vector<uint8_t>& packet);
  void reset();
  void setComplexity(int complexity);
  void setDtx(bool enabled);

 private:
  void* encoder_ = nullptr;
};

class Decoder {
 public:
  Decoder();
  ~Decoder();

  Decoder(const Decoder&) = delete;
  Decoder& operator=(const Decoder&) = delete;

  bool decode(const uint8_t* packet, std::size_t packetBytes,
              std::vector<int16_t>& pcm);
  void reset();

 private:
  void* decoder_ = nullptr;
};

}  // namespace metalio_opus
