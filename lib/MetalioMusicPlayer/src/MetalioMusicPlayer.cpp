
#include "MetalioMusicPlayer.h"

#if FREEINK_DEVICE_METALIO_EINK4

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>

#include <PCMFlow.h>

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>

#include <HalStorage.h>

#include "MetalioAudioService.h"

namespace metalio_music_player {
namespace {

constexpr char kMusicDir[] = "/music";
constexpr std::size_t kMaxTracks = 200;
constexpr std::size_t kDecodeFrames = 960;
constexpr std::size_t kQueueRetryMs = 8;
constexpr uint32_t kUiRefreshMs = 1000;
constexpr uint32_t kPlaybackSampleRate = 16000;
constexpr uint8_t kDefaultVolume = 70;
constexpr uint8_t kMinVolume = 0;
constexpr uint8_t kMaxVolume = 100;
constexpr uint32_t kTaskStackBytes = 16 * 1024;
constexpr UBaseType_t kTaskPriority = 3;
constexpr UBaseType_t kTaskCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

bool hasExtension(const std::string& path, const char* ext) {
  if (path.size() < std::strlen(ext)) return false;
  const std::size_t pos = path.size() - std::strlen(ext);
  for (std::size_t i = 0; i < std::strlen(ext); ++i) {
    if (std::tolower(static_cast<unsigned char>(path[pos + i])) !=
        std::tolower(static_cast<unsigned char>(ext[i]))) {
      return false;
    }
  }
  return true;
}

bool isSupportedPath(const std::string& path) {
  return hasExtension(path, ".mp3") || hasExtension(path, ".ogg") ||
         hasExtension(path, ".wav");
}

std::string baseName(const std::string& path) {
  const std::size_t slash = path.find_last_of('/');
  const std::size_t dot = path.find_last_of('.');
  const std::size_t start = slash == std::string::npos ? 0 : slash + 1;
  const std::size_t end = (dot == std::string::npos || dot < start) ? path.size() : dot;
  return path.substr(start, end - start);
}

void splitTitleArtist(const std::string& path, std::string& title, std::string& artist) {
  const std::string base = baseName(path);
  const std::size_t sep = base.find(" - ");
  if (sep != std::string::npos) {
    artist = base.substr(0, sep);
    title = base.substr(sep + 3);
  } else {
    title = base;
    artist.clear();
  }
}

uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t le64(const uint8_t* p) {
  uint64_t value = 0;
  for (int i = 0; i < 8; ++i) value |= static_cast<uint64_t>(p[i]) << (8 * i);
  return value;
}

class FsByteStream final : public ByteStream {
 public:
  explicit FsByteStream(HalFile& file) : file_(file) {}

  std::size_t read(void* dst, std::size_t count) override {
    if (!file_.isOpen()) return 0;
    const int n = file_.read(dst, count);
    if (n > 0) position_ += static_cast<std::size_t>(n);
    return n > 0 ? static_cast<std::size_t>(n) : 0;
  }

  bool isEof() const override {
    return !file_.isOpen() || position_ >= file_.fileSize();
  }

  bool isSeekable() const override { return file_.isOpen(); }

  bool seek(std::size_t offset) override {
    if (!file_.isOpen() || !file_.seek(offset)) return false;
    position_ = offset;
    return true;
  }

  std::size_t size() const override {
    return file_.isOpen() ? file_.fileSize() : 0;
  }

  std::size_t position() const override { return position_; }

 private:
  HalFile& file_;
  std::size_t position_ = 0;
};

uint32_t estimateMp3Duration(HalFile& file) {
  const std::size_t fileSize = file.fileSize();
  if (!file.seek(0)) return 0;

  std::array<uint8_t, 4096> buf{};
  const int n = file.read(buf.data(), buf.size());
  if (n < 16) {
    file.seek(0);
    return 0;
  }

  std::size_t offset = 0;
  std::size_t audioOffset = 0;

  if (n >= 10 && std::memcmp(buf.data(), "ID3", 3) == 0) {
    const uint32_t tagSize =
        ((buf[6] & 0x7f) << 21) |
        ((buf[7] & 0x7f) << 14) |
        ((buf[8] & 0x7f) << 7) |
        (buf[9] & 0x7f);
    audioOffset = 10 + tagSize + ((buf[5] & 0x10) ? 10 : 0);
    offset = audioOffset;
  }

  std::size_t frameOffset = offset;
  uint32_t header = 0;
  for (; frameOffset + 4 <= static_cast<std::size_t>(n); ++frameOffset) {
    const uint32_t h = (static_cast<uint32_t>(buf[frameOffset]) << 24) |
                       (static_cast<uint32_t>(buf[frameOffset + 1]) << 16) |
                       (static_cast<uint32_t>(buf[frameOffset + 2]) << 8) |
                       buf[frameOffset + 3];
    if ((h & 0xffe00000u) != 0xffe00000u) continue;
    const uint8_t version = static_cast<uint8_t>((h >> 19) & 0x3);
    const uint8_t layer = static_cast<uint8_t>((h >> 17) & 0x3);
    const uint8_t bitrateIndex = static_cast<uint8_t>((h >> 12) & 0xf);
    const uint8_t sampleIndex = static_cast<uint8_t>((h >> 10) & 0x3);
    if (version == 1 || layer != 1 || bitrateIndex == 0 || bitrateIndex == 15 ||
        sampleIndex == 3) {
      continue;
    }
    header = h;
    break;
  }

  if (header == 0) {
    file.seek(0);
    return 0;
  }

  const uint8_t version = static_cast<uint8_t>((header >> 19) & 0x3);
  const uint8_t bitrateIndex = static_cast<uint8_t>((header >> 12) & 0xf);
  const uint8_t sampleIndex = static_cast<uint8_t>((header >> 10) & 0x3);
  const uint8_t channelMode = static_cast<uint8_t>((header >> 6) & 0x3);

  static constexpr uint16_t kBitratesV1[] = {
      0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
  static constexpr uint16_t kBitratesV2[] = {
      0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
  static constexpr uint32_t kRatesV1[] = {44100, 48000, 32000, 0};
  static constexpr uint32_t kRatesV2[] = {22050, 24000, 16000, 0};

  const uint16_t bitrate =
      (version == 3 ? kBitratesV1[bitrateIndex] : kBitratesV2[bitrateIndex]);
  uint32_t sampleRate =
      (version == 3 ? kRatesV1[sampleIndex] : kRatesV2[sampleIndex]);
  if (version == 0) sampleRate /= 2;
  if (bitrate == 0 || sampleRate == 0) {
    file.seek(0);
    return 0;
  }

  const std::size_t sideInfo =
      version == 3 ? (channelMode == 3 ? 17 : 32) : (channelMode == 3 ? 9 : 17);
  const std::size_t xing = frameOffset + 4 + sideInfo;

  if (xing + 8 <= static_cast<std::size_t>(n) &&
      (std::memcmp(buf.data() + xing, "Xing", 4) == 0 ||
       std::memcmp(buf.data() + xing, "Info", 4) == 0)) {
    const uint32_t flags = le32(buf.data() + xing + 4);
    if ((flags & 0x1u) != 0 && xing + 12 <= static_cast<std::size_t>(n)) {
      const uint32_t frames = le32(buf.data() + xing + 8);
      const uint32_t samplesPerFrame = version == 3 ? 1152 : 576;
      file.seek(0);
      return static_cast<uint32_t>(
          (static_cast<uint64_t>(frames) * samplesPerFrame * 1000ULL) / sampleRate);
    }
  }

  const std::size_t audioBytes = fileSize > audioOffset ? fileSize - audioOffset : fileSize;
  file.seek(0);
  return static_cast<uint32_t>(
      (static_cast<uint64_t>(audioBytes) * 8ULL) / (static_cast<uint64_t>(bitrate) * 1000ULL));
}

uint32_t estimateWavDuration(HalFile& file) {
  std::array<uint8_t, 64> header{};
  if (!file.seek(0) || file.read(header.data(), header.size()) < 44 ||
      std::memcmp(header.data(), "RIFF", 4) != 0 ||
      std::memcmp(header.data() + 8, "WAVE", 4) != 0) {
    file.seek(0);
    return 0;
  }

  const uint16_t channels = static_cast<uint16_t>(header[22] | (header[23] << 8));
  const uint32_t sampleRate = le32(header.data() + 24);
  const uint16_t bits = static_cast<uint16_t>(header[34] | (header[35] << 8));
  const uint32_t dataBytes = le32(header.data() + 40);
  file.seek(0);
  if (channels == 0 || sampleRate == 0 || bits == 0) return 0;
  return static_cast<uint32_t>(
      (static_cast<uint64_t>(dataBytes) * 1000ULL) /
      (static_cast<uint64_t>(sampleRate) * channels * (bits / 8)));
}

class OggVorbisSource {
 public:
  ~OggVorbisSource() { close(); }

  bool open(HalFile&& file) {
    close();
    file_ = std::move(file);
    if (!file_.isOpen()) return false;

    const std::size_t size = file_.fileSize();
    if (size == 0 || size > kMaxFileBytes) return false;

    data_ = static_cast<unsigned char*>(
        heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    dataInPsram_ = data_ != nullptr;
    if (data_ == nullptr) {
      data_ = static_cast<unsigned char*>(malloc(size));
      dataInPsram_ = false;
    }
    if (data_ == nullptr) return false;

    if (!file_.seek(0) ||
        file_.read(data_, size) != static_cast<int>(size)) {
      close();
      return false;
    }

    int error = 0;
    decoder_ = stb_vorbis_open_memory(
        data_, static_cast<int>(size), &error, nullptr);
    if (decoder_ == nullptr) {
      Serial.printf("[METALIO-MUSIC] stb_vorbis_open_memory failed: %d\n", error);
      close();
      return false;
    }

    const stb_vorbis_info info = stb_vorbis_get_info(decoder_);
    sourceRate_ = static_cast<uint32_t>(info.sample_rate);
    sourceChannels_ = info.channels;
    if (sourceRate_ == 0 || sourceChannels_ <= 0) {
      close();
      return false;
    }

    const unsigned totalFrames = stb_vorbis_stream_length_in_samples(decoder_);
    durationMs_ = static_cast<uint32_t>(
        (static_cast<uint64_t>(totalFrames) * 1000ULL) / sourceRate_);

    eof_ = false;
    return true;
  }

  void close() {
    if (decoder_ != nullptr) {
      stb_vorbis_close(decoder_);
      decoder_ = nullptr;
    }
    if (data_ != nullptr) {
      if (dataInPsram_) {
        heap_caps_free(data_);
      } else {
        free(data_);
      }
      data_ = nullptr;
      dataInPsram_ = false;
    }
    file_.close();
    sourceRate_ = 0;
    sourceChannels_ = 0;
    durationMs_ = 0;
    pending_.clear();
    pendingOffset_ = 0;
    eof_ = true;
  }

  bool readFrames(std::vector<int16_t>& out, std::size_t maxFrames) {
    out.clear();
    if (decoder_ == nullptr || maxFrames == 0 || eof_) return false;

    constexpr int kDecodeFrames = 4096;
    std::array<int16_t, kDecodeFrames * 2> decoded{};

    while (out.size() < maxFrames && !eof_) {
      if (pendingOffset_ < pending_.size()) {
        const std::size_t count = std::min(
            maxFrames - out.size(),
            pending_.size() - pendingOffset_);
        out.insert(out.end(),
                   pending_.begin() + pendingOffset_,
                   pending_.begin() + pendingOffset_ + count);
        pendingOffset_ += count;
        continue;
      }

      pending_.clear();
      pendingOffset_ = 0;

      const int frames = stb_vorbis_get_samples_short_interleaved(
          decoder_,
          2,
          decoded.data(),
          static_cast<int>(decoded.size()));

      if (frames <= 0) {
        eof_ = true;
        break;
      }

      const int channels = std::max(1, sourceChannels_);
      const double step =
          static_cast<double>(sourceRate_) /
          static_cast<double>(kPlaybackSampleRate);

      for (double sourcePosition = 0.0;
           sourcePosition < frames;
           sourcePosition += step) {
        const int index0 = static_cast<int>(sourcePosition);
        const double frac = sourcePosition - index0;
        const int index1 = std::min(index0 + 1, frames - 1);

        const int16_t left0 = decoded[index0 * 2];
        const int16_t right0 = channels > 1 ? decoded[index0 * 2 + 1] : left0;
        const int16_t left1 = decoded[index1 * 2];
        const int16_t right1 = channels > 1 ? decoded[index1 * 2 + 1] : left1;

        const int32_t mono0 =
            (static_cast<int32_t>(left0) + static_cast<int32_t>(right0)) / 2;
        const int32_t mono1 =
            (static_cast<int32_t>(left1) + static_cast<int32_t>(right1)) / 2;
        const int32_t sample =
            mono0 + static_cast<int32_t>((mono1 - mono0) * frac);

        pending_.push_back(static_cast<int16_t>(
            std::clamp(sample, static_cast<int32_t>(-32768), static_cast<int32_t>(32767))));
      }
    }

    return !out.empty();
  }

  bool eof() const { return eof_; }
  uint32_t durationMs() const { return durationMs_; }

 private:
  static constexpr std::size_t kMaxFileBytes = 6 * 1024 * 1024;

  HalFile file_;
  unsigned char* data_ = nullptr;
  bool dataInPsram_ = false;
  stb_vorbis* decoder_ = nullptr;
  std::vector<int16_t> pending_;
  std::size_t pendingOffset_ = 0;
  uint32_t sourceRate_ = 0;
  int sourceChannels_ = 0;
  uint32_t durationMs_ = 0;
  bool eof_ = true;
};
}  // namespace

struct MusicPlayer::Impl {
  mutable std::mutex mutex;
  std::condition_variable cv;

  std::vector<TrackInfo> tracks;
  std::size_t selected = 0;
  std::size_t current = 0;

  PlayerState state = PlayerState::Stopped;
  std::string error;
  uint32_t elapsed = 0;
  uint32_t duration = 0;
  uint8_t volume = kDefaultVolume;

  bool taskStop = false;
  bool commandPending = false;
  bool paused = false;
  uint32_t generation = 0;
  TaskHandle_t task = nullptr;

  bool started = false;
};

void playbackTask(void* arg) {
  auto* impl = static_cast<MusicPlayer::Impl*>(arg);

  for (;;) {
    std::size_t index = 0;
    uint32_t generation = 0;

    {
      std::unique_lock<std::mutex> lock(impl->mutex);
      impl->cv.wait(lock, [&] {
        return impl->taskStop || impl->commandPending;
      });

      if (impl->taskStop) break;

      impl->commandPending = false;
      index = impl->current;
      generation = impl->generation;
      impl->state = PlayerState::Loading;
      impl->error.clear();
      impl->elapsed = 0;
      impl->duration = 0;
    }

    if (!metalio_audio_service::start()) {
      std::lock_guard<std::mutex> lock(impl->mutex);
      impl->state = PlayerState::Error;
      impl->error = "Audio service start failed";
      continue;
    }

    metalio_audio_service::setOutputVolume(impl->volume);

    const TrackInfo track = [&] {
      std::lock_guard<std::mutex> lock(impl->mutex);
      return index < impl->tracks.size() ? impl->tracks[index] : TrackInfo{};
    }();

    HalFile file = Storage.open(track.path.c_str(), O_RDONLY);
    if (!file.isOpen()) {
      std::lock_guard<std::mutex> lock(impl->mutex);
      impl->state = PlayerState::Error;
      impl->error = "Cannot open " + track.path;
      continue;
    }

    const bool isOggVorbis = hasExtension(track.path, ".ogg");
    const bool isMp3 = hasExtension(track.path, ".mp3");
    const bool isWav = hasExtension(track.path, ".wav");

    if (isMp3 || isWav) {
      const uint32_t estimatedDuration = isMp3 ? estimateMp3Duration(file) : estimateWavDuration(file);
      {
        std::lock_guard<std::mutex> lock(impl->mutex);
        impl->duration = estimatedDuration;
        if (index < impl->tracks.size()) impl->tracks[index].durationMs = estimatedDuration;
      }

      FsByteStream stream(file);
      PCMFlow flow;
      PCMFormat outputFormat;
      outputFormat.sampleRate = kPlaybackSampleRate;
      outputFormat.channels = 1;
      outputFormat.bitsPerSample = 16;
      flow.setOutputFormat(outputFormat);
      flow.setBufferFrames(8192);
      flow.setInput(stream, PCMFlow::CodecKind::Auto);

      std::array<int16_t, kDecodeFrames> pcm{};

      for (;;) {
        {
          std::lock_guard<std::mutex> lock(impl->mutex);
          if (impl->taskStop || impl->generation != generation) break;
          if (impl->paused) {
            impl->state = PlayerState::Paused;
          }
        }

        bool paused = false;
        {
          std::lock_guard<std::mutex> lock(impl->mutex);
          paused = impl->paused;
        }
        if (paused) {
          metalio_audio_service::flushPlayback();
          std::unique_lock<std::mutex> lock(impl->mutex);
          impl->cv.wait_for(lock, std::chrono::milliseconds(50), [&] {
            return impl->taskStop || impl->generation != generation || !impl->paused;
          });
          continue;
        }

        flow.pump();
        const std::size_t available = flow.availableFrames();
        if (available == 0) {
          if (flow.isEof()) break;
          vTaskDelay(pdMS_TO_TICKS(2));
          continue;
        }

        const std::size_t want = std::min<std::size_t>(available, pcm.size());
        const std::size_t got = flow.readFrames(pcm.data(), want);
        if (got == 0) continue;

        bool queued = false;
        while (!queued) {
          {
            std::lock_guard<std::mutex> lock(impl->mutex);
            if (impl->taskStop || impl->generation != generation || impl->paused) break;
          }
          queued = metalio_audio_service::queuePcm(pcm.data(), got);
          if (!queued) vTaskDelay(pdMS_TO_TICKS(kQueueRetryMs));
        }

        if (queued) {
          std::lock_guard<std::mutex> lock(impl->mutex);
          impl->elapsed = static_cast<uint32_t>(
              std::min<uint64_t>(
                  static_cast<uint64_t>(impl->elapsed) +
                      (static_cast<uint64_t>(got) * 1000ULL) / kPlaybackSampleRate,
                  0xffffffffULL));
          impl->state = PlayerState::Playing;
        }
      }

      flow.close();
    } else if (isOggVorbis) {
      OggVorbisSource source;
      if (!source.open(std::move(file))) {
        std::lock_guard<std::mutex> lock(impl->mutex);
        impl->state = PlayerState::Error;
        impl->error = "Invalid Ogg Vorbis";
        continue;
      }

      {
        std::lock_guard<std::mutex> lock(impl->mutex);
        impl->duration = source.durationMs();
        if (index < impl->tracks.size()) impl->tracks[index].durationMs = source.durationMs();
      }

      std::vector<int16_t> pcm;
      pcm.reserve(kDecodeFrames);

      while (!source.eof()) {
        {
          std::lock_guard<std::mutex> lock(impl->mutex);
          if (impl->taskStop || impl->generation != generation) break;
        }

        bool paused = false;
        {
          std::lock_guard<std::mutex> lock(impl->mutex);
          paused = impl->paused;
        }
        if (paused) {
          metalio_audio_service::flushPlayback();
          std::unique_lock<std::mutex> lock(impl->mutex);
          lock.unlock();
          vTaskDelay(pdMS_TO_TICKS(50));
          continue;
        }

        pcm.clear();
        source.readFrames(pcm, kDecodeFrames);
        if (pcm.empty()) break;

        bool queued = false;
        while (!queued) {
          {
            std::lock_guard<std::mutex> lock(impl->mutex);
            if (impl->taskStop || impl->generation != generation || impl->paused) break;
          }
          queued = metalio_audio_service::queuePcm(pcm.data(), pcm.size());
          if (!queued) vTaskDelay(pdMS_TO_TICKS(kQueueRetryMs));
        }

        if (queued) {
          std::lock_guard<std::mutex> lock(impl->mutex);
          impl->elapsed = static_cast<uint32_t>(
              std::min<uint64_t>(
                  static_cast<uint64_t>(impl->elapsed) +
                      (static_cast<uint64_t>(pcm.size()) * 1000ULL) / kPlaybackSampleRate,
                  0xffffffffULL));
          impl->state = PlayerState::Playing;
        }
      }
    } else {
      std::lock_guard<std::mutex> lock(impl->mutex);
      impl->state = PlayerState::Error;
      impl->error = "Unsupported audio format";
      continue;
    }

    metalio_audio_service::waitPlaybackDrained(10000);

    bool advance = false;
    {
      std::lock_guard<std::mutex> lock(impl->mutex);
      if (!impl->taskStop && impl->generation == generation && !impl->paused &&
          index < impl->tracks.size()) {
        advance = true;
        impl->current = (index + 1) % impl->tracks.size();
        impl->selected = impl->current;
        impl->generation++;
        impl->commandPending = true;
      }
      if (!advance && !impl->taskStop && impl->generation == generation) {
        if (impl->paused) {
          impl->state = PlayerState::Paused;
        } else if (impl->state != PlayerState::Error) {
          impl->state = PlayerState::Stopped;
        }
      }
    }
    if (advance) {
      impl->cv.notify_all();
    }
  }

  metalio_audio_service::flushPlayback();
  metalio_audio_service::stop();

  {
    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->task = nullptr;
  }

  vTaskDeleteWithCaps(nullptr);
}

MusicPlayer::MusicPlayer() : impl_(new Impl()) {}

MusicPlayer& MusicPlayer::instance() {
  static MusicPlayer instance;
  return instance;
}

bool MusicPlayer::begin() {
  auto& impl = *impl_;
  refreshLibrary();

  std::lock_guard<std::mutex> lock(impl.mutex);
  if (impl.started) return true;

  impl.taskStop = false;
  impl.started = true;
  TaskHandle_t task = nullptr;
  if (xTaskCreateWithCaps(
          playbackTask,
          "metalio_music",
          kTaskStackBytes,
          &impl,
          kTaskPriority,
          &task,
          kTaskCaps) != pdPASS) {
    impl.started = false;
    impl.error = "Music task allocation failed";
    impl.state = PlayerState::Error;
    return false;
  }
  impl.task = task;
  return true;
}

void MusicPlayer::shutdown() {
  auto& impl = *impl_;
  TaskHandle_t task = nullptr;
  {
    std::lock_guard<std::mutex> lock(impl.mutex);
    if (!impl.started) return;
    impl.taskStop = true;
    impl.commandPending = true;
    task = impl.task;
  }
  impl.cv.notify_all();

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(impl.mutex);
      if (impl.task == nullptr) break;
    }
    if (std::chrono::steady_clock::now() >= deadline) break;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  metalio_audio_service::flushPlayback();
  metalio_audio_service::stop();

  std::lock_guard<std::mutex> lock(impl.mutex);
  impl.started = false;
}

bool MusicPlayer::refreshLibrary() {
  auto& impl = *impl_;
  std::vector<String> names = Storage.listFiles(kMusicDir, static_cast<int>(kMaxTracks));

  std::vector<TrackInfo> tracks;
  tracks.reserve(names.size());
  for (const String& name : names) {
    std::string path = kMusicDir;
    path += "/";
    path += name.c_str();
    if (!isSupportedPath(path)) continue;

    TrackInfo info;
    info.path = std::move(path);
    splitTitleArtist(info.path, info.title, info.artist);
    tracks.push_back(std::move(info));
  }

  std::sort(tracks.begin(), tracks.end(), [](const TrackInfo& a, const TrackInfo& b) {
    return a.title < b.title;
  });

  {
    std::lock_guard<std::mutex> lock(impl.mutex);
    impl.tracks = std::move(tracks);
    if (impl.tracks.empty()) {
      impl.selected = 0;
      impl.current = 0;
    } else {
      impl.selected = std::min(impl.selected, impl.tracks.size() - 1);
      impl.current = std::min(impl.current, impl.tracks.size() - 1);
    }
  }

  return !impl.tracks.empty();
}

std::size_t MusicPlayer::trackCount() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->tracks.size();
}

TrackInfo MusicPlayer::track(std::size_t index) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return index < impl_->tracks.size() ? impl_->tracks[index] : TrackInfo{};
}

std::size_t MusicPlayer::selectedIndex() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->selected;
}

void MusicPlayer::selectNext() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->tracks.empty()) return;
  impl_->selected = (impl_->selected + 1) % impl_->tracks.size();
}

void MusicPlayer::selectPrevious() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->tracks.empty()) return;
  impl_->selected = impl_->selected == 0 ? impl_->tracks.size() - 1 : impl_->selected - 1;
}

bool MusicPlayer::playSelected() {
  return playIndex(selectedIndex());
}

bool MusicPlayer::playIndex(std::size_t index) {
  auto& impl = *impl_;
  {
    std::lock_guard<std::mutex> lock(impl.mutex);
    if (index >= impl.tracks.size()) return false;
    impl.current = index;
    impl.selected = index;
    impl.elapsed = 0;
    impl.duration = impl.tracks[index].durationMs;
    impl.paused = false;
    impl.state = PlayerState::Loading;
    impl.generation++;
    impl.commandPending = true;
  }
  if (!begin()) return false;
  metalio_audio_service::flushPlayback();
  impl.cv.notify_all();
  return true;
}

void MusicPlayer::togglePause() {
  auto& impl = *impl_;
  {
    std::lock_guard<std::mutex> lock(impl.mutex);
    if (impl.state == PlayerState::Stopped || impl.state == PlayerState::Error) {
      impl.current = impl.selected;
      impl.elapsed = 0;
      impl.duration = impl.tracks.empty() ? 0 : impl.tracks[impl.current].durationMs;
      impl.paused = false;
      impl.generation++;
      impl.commandPending = true;
      impl.state = PlayerState::Loading;
    } else {
      impl.paused = !impl.paused;
      if (impl.paused) {
        impl.state = PlayerState::Paused;
      }
    }
  }
  bool pausedNow = false;
  {
    std::lock_guard<std::mutex> lock(impl.mutex);
    pausedNow = impl.paused;
  }
  if (pausedNow) metalio_audio_service::flushPlayback();
  impl.cv.notify_all();
}

void MusicPlayer::nextTrack() {
  auto& impl = *impl_;
  {
    std::lock_guard<std::mutex> lock(impl.mutex);
    if (impl.tracks.empty()) return;
    impl.current = (impl.current + 1) % impl.tracks.size();
    impl.selected = impl.current;
    impl.elapsed = 0;
    impl.duration = impl.tracks[impl.current].durationMs;
    impl.paused = false;
    impl.generation++;
    impl.commandPending = true;
  }
  metalio_audio_service::flushPlayback();
  impl.cv.notify_all();
}

void MusicPlayer::previousTrack() {
  auto& impl = *impl_;
  {
    std::lock_guard<std::mutex> lock(impl.mutex);
    if (impl.tracks.empty()) return;
    impl.current = impl.current == 0 ? impl.tracks.size() - 1 : impl.current - 1;
    impl.selected = impl.current;
    impl.elapsed = 0;
    impl.duration = impl.tracks[impl.current].durationMs;
    impl.paused = false;
    impl.generation++;
    impl.commandPending = true;
  }
  metalio_audio_service::flushPlayback();
  impl.cv.notify_all();
}

void MusicPlayer::volumeUp(uint8_t step) {
  auto& impl = *impl_;
  uint8_t volume = 0;
  {
    std::lock_guard<std::mutex> lock(impl.mutex);
    volume = static_cast<uint8_t>(std::min<int>(kMaxVolume, impl.volume + step));
    impl.volume = volume;
  }
  metalio_audio_service::setOutputVolume(volume);
}

void MusicPlayer::volumeDown(uint8_t step) {
  auto& impl = *impl_;
  uint8_t volume = 0;
  {
    std::lock_guard<std::mutex> lock(impl.mutex);
    volume = static_cast<uint8_t>(impl.volume > step ? impl.volume - step : kMinVolume);
    impl.volume = volume;
  }
  metalio_audio_service::setOutputVolume(volume);
}

PlayerState MusicPlayer::state() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->state;
}

TrackInfo MusicPlayer::currentTrack() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->current < impl_->tracks.size() ? impl_->tracks[impl_->current] : TrackInfo{};
}

uint32_t MusicPlayer::elapsedMs() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->elapsed;
}

uint32_t MusicPlayer::durationMs() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->duration;
}

uint8_t MusicPlayer::volume() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->volume;
}

std::string MusicPlayer::errorText() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->error;
}

}  // namespace metalio_music_player

#else

namespace metalio_music_player {
MusicPlayer& MusicPlayer::instance() {
  static MusicPlayer instance;
  return instance;
}
MusicPlayer::MusicPlayer() : impl_(nullptr) {}
bool MusicPlayer::begin() { return false; }
void MusicPlayer::shutdown() {}
bool MusicPlayer::refreshLibrary() { return false; }
std::size_t MusicPlayer::trackCount() const { return 0; }
TrackInfo MusicPlayer::track(std::size_t) const { return {}; }
std::size_t MusicPlayer::selectedIndex() const { return 0; }
void MusicPlayer::selectNext() {}
void MusicPlayer::selectPrevious() {}
bool MusicPlayer::playSelected() { return false; }
bool MusicPlayer::playIndex(std::size_t) { return false; }
void MusicPlayer::togglePause() {}
void MusicPlayer::nextTrack() {}
void MusicPlayer::previousTrack() {}
void MusicPlayer::volumeUp(uint8_t) {}
void MusicPlayer::volumeDown(uint8_t) {}
PlayerState MusicPlayer::state() const { return PlayerState::Stopped; }
TrackInfo MusicPlayer::currentTrack() const { return {}; }
uint32_t MusicPlayer::elapsedMs() const { return 0; }
uint32_t MusicPlayer::durationMs() const { return 0; }
uint8_t MusicPlayer::volume() const { return 70; }
std::string MusicPlayer::errorText() const { return {}; }
}  // namespace metalio_music_player

#endif
