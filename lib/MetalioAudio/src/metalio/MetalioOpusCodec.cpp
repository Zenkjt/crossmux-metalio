#include "MetalioOpusCodec.h"

#if FREEINK_DEVICE_METALIO_EINK4

#include <Logging.h>
#include <opus.h>

#include <algorithm>
#include <array>

namespace metalio_opus {
namespace {

constexpr const char* kTag = "METALIO-OPUS";

}  // namespace

Encoder::Encoder() {
  int error = OPUS_OK;
  encoder_ = opus_encoder_create(kSampleRate, kChannels,
                                 OPUS_APPLICATION_AUDIO, &error);
  if (encoder_ == nullptr || error != OPUS_OK) {
    encoder_ = nullptr;
    LOG_ERR(kTag, "opus_encoder_create failed: %d", error);
    return;
  }

  setComplexity(0);
  setDtx(false);
  (void)opus_encoder_ctl(static_cast<OpusEncoder*>(encoder_),
                         OPUS_SET_BITRATE(24000));
  (void)opus_encoder_ctl(static_cast<OpusEncoder*>(encoder_),
                         OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
}

Encoder::~Encoder() {
  if (encoder_ != nullptr) {
    opus_encoder_destroy(static_cast<OpusEncoder*>(encoder_));
    encoder_ = nullptr;
  }
}

bool Encoder::encode(const int16_t* pcm, std::size_t samples,
                     std::vector<uint8_t>& packet) {
  packet.clear();
  if (encoder_ == nullptr || pcm == nullptr || samples != kFrameSamples) {
    return false;
  }

  std::array<uint8_t, kMaxPacketBytes> encoded{};
  const int bytes = opus_encode(static_cast<OpusEncoder*>(encoder_), pcm,
                                kFrameSamples, encoded.data(),
                                static_cast<opus_int32>(encoded.size()));
  if (bytes <= 0) {
    LOG_ERR(kTag, "opus_encode failed: %d", bytes);
    return false;
  }

  packet.assign(encoded.begin(), encoded.begin() + bytes);
  return true;
}

void Encoder::reset() {
  if (encoder_ != nullptr) {
    (void)opus_encoder_ctl(static_cast<OpusEncoder*>(encoder_),
                           OPUS_RESET_STATE);
  }
}

void Encoder::setComplexity(int complexity) {
  if (encoder_ != nullptr) {
    complexity = std::clamp(complexity, 0, 10);
    (void)opus_encoder_ctl(static_cast<OpusEncoder*>(encoder_),
                           OPUS_SET_COMPLEXITY(complexity));
  }
}

void Encoder::setDtx(bool enabled) {
  if (encoder_ != nullptr) {
    (void)opus_encoder_ctl(static_cast<OpusEncoder*>(encoder_),
                           OPUS_SET_DTX(enabled ? 1 : 0));
  }
}

Decoder::Decoder() {
  int error = OPUS_OK;
  decoder_ = opus_decoder_create(kSampleRate, kChannels, &error);
  if (decoder_ == nullptr || error != OPUS_OK) {
    decoder_ = nullptr;
    LOG_ERR(kTag, "opus_decoder_create failed: %d", error);
  }
}

Decoder::~Decoder() {
  if (decoder_ != nullptr) {
    opus_decoder_destroy(static_cast<OpusDecoder*>(decoder_));
    decoder_ = nullptr;
  }
}

bool Decoder::decode(const uint8_t* packet, std::size_t packetBytes,
                     std::vector<int16_t>& pcm) {
  pcm.clear();
  if (decoder_ == nullptr || packet == nullptr || packetBytes == 0) {
    return false;
  }

  pcm.resize(kFrameSamples);
  const int samples = opus_decode(
      static_cast<OpusDecoder*>(decoder_), packet,
      static_cast<opus_int32>(packetBytes), pcm.data(),
      kFrameSamples, 0);
  if (samples <= 0) {
    pcm.clear();
    LOG_ERR(kTag, "opus_decode failed: %d", samples);
    return false;
  }

  pcm.resize(static_cast<std::size_t>(samples));
  return true;
}

void Decoder::reset() {
  if (decoder_ != nullptr) {
    (void)opus_decoder_ctl(static_cast<OpusDecoder*>(decoder_),
                           OPUS_RESET_STATE);
  }
}

}  // namespace metalio_opus

#else

namespace metalio_opus {
Encoder::Encoder() = default;
Encoder::~Encoder() = default;
bool Encoder::encode(const int16_t*, std::size_t, std::vector<uint8_t>& packet) {
  packet.clear();
  return false;
}
void Encoder::reset() {}
void Encoder::setComplexity(int) {}
void Encoder::setDtx(bool) {}
Decoder::Decoder() = default;
Decoder::~Decoder() = default;
bool Decoder::decode(const uint8_t*, std::size_t, std::vector<int16_t>& pcm) {
  pcm.clear();
  return false;
}
void Decoder::reset() {}
}  // namespace metalio_opus

#endif
