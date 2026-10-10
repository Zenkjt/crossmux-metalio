#include "MetalioOggOpus.h"

#if FREEINK_DEVICE_METALIO_EINK4

#include <Logging.h>
#include <freertos/FreeRTOS.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>

#include "MetalioAudioService.h"
#include "MetalioOpusCodec.h"

namespace metalio_ogg_opus {
namespace {

constexpr char kRecordingDir[] = "/sdcard/metalio/e-ink/recordings";
constexpr uint32_t kOggSerial = 0x4d45544c;  // "METL"

uint32_t crcOgg(const uint8_t* data, std::size_t size) {
  uint32_t crc = 0;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= static_cast<uint32_t>(data[i]) << 24;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04c11db7u
                                 : (crc << 1);
    }
  }
  return crc;
}

void putLe32(uint8_t* p, uint32_t value) {
  p[0] = static_cast<uint8_t>(value);
  p[1] = static_cast<uint8_t>(value >> 8);
  p[2] = static_cast<uint8_t>(value >> 16);
  p[3] = static_cast<uint8_t>(value >> 24);
}

void putLe64(uint8_t* p, uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    p[i] = static_cast<uint8_t>(value >> (i * 8));
  }
}

bool writeOggPage(FILE* file, uint8_t headerType, uint64_t granule,
                  uint32_t serial, uint32_t sequence,
                  const uint8_t* packet, std::size_t packetBytes) {
  if (packet == nullptr || packetBytes == 0 || packetBytes > 65025) {
    return false;
  }

  // A packet whose size is an exact multiple of 255 needs a zero-length
  // lacing segment to mark the end of the packet. Without that terminator,
  // Ogg readers treat the next page as a continuation of this packet.
  const std::size_t dataSegmentCount = (packetBytes + 254) / 255;
  const bool needsTerminator = (packetBytes % 255) == 0;
  const std::size_t segmentCount = dataSegmentCount + (needsTerminator ? 1 : 0);
  if (segmentCount == 0 || segmentCount > 255) return false;

  std::vector<uint8_t> page(27 + segmentCount + packetBytes, 0);
  std::memcpy(page.data(), "OggS", 4);
  page[4] = 0;
  page[5] = headerType;
  putLe64(page.data() + 6, granule);
  putLe32(page.data() + 14, serial);
  putLe32(page.data() + 18, sequence);
  page[26] = static_cast<uint8_t>(segmentCount);

  std::size_t remaining = packetBytes;
  for (std::size_t i = 0; i < segmentCount; ++i) {
    const std::size_t n = std::min<std::size_t>(remaining, 255);
    page[27 + i] = static_cast<uint8_t>(n);
    remaining -= n;
  }

  std::memcpy(page.data() + 27 + segmentCount, packet, packetBytes);

  const uint32_t crc = crcOgg(page.data(), page.size());
  putLe32(page.data() + 22, crc);

  return std::fwrite(page.data(), 1, page.size(), file) == page.size();
}

std::vector<uint8_t> makeOpusHead() {
  std::vector<uint8_t> head(19, 0);
  std::memcpy(head.data(), "OpusHead", 8);
  head[8] = 1;
  head[9] = 1;
  // pre-skip = 0
  head[12] = static_cast<uint8_t>(metalio_opus::kSampleRate);
  head[13] = static_cast<uint8_t>(metalio_opus::kSampleRate >> 8);
  head[14] = static_cast<uint8_t>(metalio_opus::kSampleRate >> 16);
  head[15] = static_cast<uint8_t>(metalio_opus::kSampleRate >> 24);
  // output gain = 0, mapping family = 0
  return head;
}

std::vector<uint8_t> makeOpusTags() {
  static constexpr char kVendor[] = "CrossMux Metalio";
  std::vector<uint8_t> tags(8 + 4 + sizeof(kVendor) - 1 + 4, 0);
  std::memcpy(tags.data(), "OpusTags", 8);
  putLe32(tags.data() + 8, sizeof(kVendor) - 1);
  std::memcpy(tags.data() + 12, kVendor, sizeof(kVendor) - 1);
  putLe32(tags.data() + 12 + sizeof(kVendor) - 1, 0);
  return tags;
}

bool ensureDir(const char* path) {
  struct stat st {};
  if (stat(path, &st) == 0) return S_ISDIR(st.st_mode);
  if (mkdir(path, 0755) == 0) return true;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool ensureRecordingDir() {
  if (!ensureDir("/sdcard/metalio")) return false;
  if (!ensureDir("/sdcard/metalio/e-ink")) return false;
  return ensureDir(kRecordingDir);
}

bool readExact(FILE* file, void* dst, std::size_t size) {
  return std::fread(dst, 1, size, file) == size;
}


}  // namespace

std::string nextRecordingPath() {
  ensureRecordingDir();
  for (unsigned i = 1; i < 10000; ++i) {
    char path[96];
    std::snprintf(path, sizeof(path), "%s/recording_%04u.ogg",
                  kRecordingDir, i);
    FILE* f = std::fopen(path, "rb");
    if (!f) return path;
    std::fclose(f);
  }
  return {};
}

bool recordFile(const char* path, uint32_t durationMs) {
  if (!path || !*path || durationMs == 0) return false;
  if (!ensureRecordingDir()) return false;

  FILE* file = std::fopen(path, "wb");
  if (!file) {
    LOG_ERR("METALIO-REC", "open %s failed errno=%d", path, errno);
    return false;
  }

  const auto head = makeOpusHead();
  const auto tags = makeOpusTags();
  uint32_t sequence = 0;
  uint64_t granule = 0;

  bool ok = writeOggPage(file, 0x02, 0, kOggSerial, sequence++,
                         head.data(), head.size());
  if (ok) {
    ok = writeOggPage(file, 0x00, 0, kOggSerial, sequence++,
                      tags.data(), tags.size());
  }

  if (ok) {
    metalio_audio_service::startCapture(false);
    const uint32_t frameMs = metalio_opus::kFrameDurationMs;
    const uint32_t frames = (durationMs + frameMs - 1) / frameMs;

    for (uint32_t i = 0; i < frames; ++i) {
      std::vector<int16_t> pcm;
      if (!metalio_audio_service::readCapturedPcm(
              pcm, metalio_opus::kFrameSamples, 2000)) {
        ok = false;
        break;
      }

      if (pcm.size() < metalio_opus::kFrameSamples) {
        pcm.resize(metalio_opus::kFrameSamples, 0);
      }

      std::vector<uint8_t> packet;
      if (!metalio_audio_service::encodePcmFrame(
              pcm.data(), metalio_opus::kFrameSamples, packet)) {
        ok = false;
        break;
      }

      // Ogg Opus granule positions are expressed in the Opus 48 kHz
      // reference clock, regardless of the encoder's 16 kHz output rate.
      constexpr uint64_t kOggOpusGranulePerFrame =
          48000ULL * metalio_opus::kFrameDurationMs / 1000ULL;
      granule += kOggOpusGranulePerFrame;
      if (!writeOggPage(file, 0x00, granule, kOggSerial, sequence++,
                        packet.data(), packet.size())) {
        ok = false;
        break;
      }
    }
    metalio_audio_service::stopCapture();
  }

  std::fclose(file);
  if (!ok) {
    std::remove(path);
  }

  LOG_INF("METALIO-REC", "%s recording %s",
          ok ? "saved" : "failed", path);
  return ok;
}

bool playFile(const char* path) {
  if (!path || !*path) return false;

  FILE* file = std::fopen(path, "rb");
  if (!file) {
    LOG_ERR("METALIO-PLAY", "open %s failed errno=%d", path, errno);
    return false;
  }

  bool seenHead = false;
  bool seenTags = false;
  bool ok = true;
  std::vector<uint8_t> partialPacket;

  for (;;) {
    std::array<uint8_t, 27> header{};
    if (!readExact(file, header.data(), header.size())) break;
    if (std::memcmp(header.data(), "OggS", 4) != 0 || header[4] != 0) {
      ok = false;
      break;
    }

    const uint8_t headerType = header[5];
    const std::size_t segmentCount = header[26];
    std::vector<uint8_t> lacing(segmentCount);
    if (segmentCount && !readExact(file, lacing.data(), segmentCount)) {
      ok = false;
      break;
    }

    std::size_t bodyBytes = 0;
    for (uint8_t n : lacing) bodyBytes += n;
    std::vector<uint8_t> body(bodyBytes);
    if (bodyBytes && !readExact(file, body.data(), bodyBytes)) {
      ok = false;
      break;
    }

    std::size_t offset = 0;
    for (std::size_t i = 0; i < lacing.size(); ++i) {
      const std::size_t len = lacing[i];
      if (offset + len > body.size()) {
        ok = false;
        break;
      }

      partialPacket.insert(partialPacket.end(),
                           body.begin() + offset,
                           body.begin() + offset + len);
      offset += len;

      if (len == 255) continue;

      const std::vector<uint8_t> packet = std::move(partialPacket);
      partialPacket.clear();

      if (!seenHead) {
        if (packet.size() >= 19 &&
            std::memcmp(packet.data(), "OpusHead", 8) == 0) {
          seenHead = true;
          continue;
        }
        ok = false;
        break;
      }

      if (!seenTags) {
        if (packet.size() >= 8 &&
            std::memcmp(packet.data(), "OpusTags", 8) == 0) {
          seenTags = true;
          continue;
        }
        // Be liberal with files that omit OpusTags.
        seenTags = true;
      }

      std::vector<int16_t> pcm;
      if (!metalio_audio_service::decodeOpusPacket(
              packet.data(), packet.size(), pcm)) {
        ok = false;
        break;
      }

      while (!metalio_audio_service::queuePcm(pcm.data(), pcm.size())) {
        if (!metalio_audio_service::waitPlaybackDrained(1000)) {
          ok = false;
          break;
        }
      }
      if (!ok) break;
    }

    if (!ok) break;
    if (headerType & 0x04) break;  // EOS
  }

  std::fclose(file);
  if (ok) {
    ok = metalio_audio_service::waitPlaybackDrained(10000);
  }
  if (!ok) {
    metalio_audio_service::flushPlayback();
  }

  LOG_INF("METALIO-PLAY", "%s %s", ok ? "played" : "failed", path);
  return ok;
}

}  // namespace metalio_ogg_opus

#else

namespace metalio_ogg_opus {
bool playFile(const char*) { return false; }
bool recordFile(const char*, uint32_t) { return false; }
std::string nextRecordingPath() { return {}; }
}  // namespace metalio_ogg_opus

#endif
