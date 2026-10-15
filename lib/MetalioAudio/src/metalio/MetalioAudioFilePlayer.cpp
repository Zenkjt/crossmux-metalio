#include "MetalioAudioFilePlayer.h"

#if FREEINK_DEVICE_METALIO_EINK4

#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#include <HalStorage.h>
#include "MetalioAudioService.h"
#include <Logging.h>

namespace metalio_audio_file_player {
namespace {

constexpr char kTag[] = "METALIO-FILE";

// 32 KiB = 16384 mono 16-bit samples = 1.024 s at 16 kHz.
constexpr std::size_t kFileBufferBytes = 32 * 1024;

constexpr uint32_t kTaskStackBytes = 16 * 1024;
constexpr UBaseType_t kTaskPriority = 3;
constexpr UBaseType_t kTaskCaps =
    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

uint16_t readLe16(const uint8_t* p) {
  return static_cast<uint16_t>(
      static_cast<uint16_t>(p[0]) |
      (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t readLe32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

bool readExact(
    HalFile& file,
    void* dst,
    std::size_t bytes) {
  uint8_t* out = static_cast<uint8_t*>(dst);
  std::size_t done = 0;

  while (done < bytes) {
    const int n = file.read(out + done, bytes - done);

    if (n <= 0) {
      return false;
    }

    done += static_cast<std::size_t>(n);
  }

  return true;
}

bool skipBytes(HalFile& file, std::size_t bytes) {
  return file.seek(file.position() + bytes);
}

struct WavInfo {
  uint16_t format = 0;
  uint16_t channels = 0;
  uint32_t sampleRate = 0;
  uint16_t bitsPerSample = 0;
  std::size_t dataOffset = 0;
  std::size_t dataBytes = 0;
};

bool parseWav(HalFile& file, WavInfo& info) {
  uint8_t header[12];

  if (!file.seek(0) ||
      !readExact(file, header, sizeof(header))) {
    return false;
  }

  if (std::memcmp(header, "RIFF", 4) != 0 ||
      std::memcmp(header + 8, "WAVE", 4) != 0) {
    return false;
  }

  bool haveFmt = false;
  bool haveData = false;
  const std::size_t fileSize = file.fileSize();

  while (file.position() + 8 <= fileSize) {
    uint8_t chunkHeader[8];

    if (!readExact(file, chunkHeader, sizeof(chunkHeader))) {
      return false;
    }

    const uint32_t chunkSize =
        readLe32(chunkHeader + 4);

    const std::size_t chunkData =
        file.position();

    if (std::memcmp(chunkHeader, "fmt ", 4) == 0) {
      if (chunkSize < 16 ||
          chunkData + 16 > fileSize) {
        return false;
      }

      uint8_t fmt[16];

      if (!readExact(file, fmt, sizeof(fmt))) {
        return false;
      }

      info.format = readLe16(fmt);
      info.channels = readLe16(fmt + 2);
      info.sampleRate = readLe32(fmt + 4);
      info.bitsPerSample = readLe16(fmt + 14);

      haveFmt = true;

      if (chunkSize > 16 &&
          !skipBytes(
              file,
              static_cast<std::size_t>(chunkSize - 16))) {
        return false;
      }

    } else if (std::memcmp(chunkHeader, "data", 4) == 0) {
      if (chunkData + chunkSize > fileSize) {
        return false;
      }

      info.dataOffset = chunkData;
      info.dataBytes = chunkSize;
      haveData = true;

      if (haveFmt) {
        break;
      }

      if (!skipBytes(file, chunkSize)) {
        return false;
      }

    } else {
      if (chunkData + chunkSize > fileSize ||
          !skipBytes(file, chunkSize)) {
        return false;
      }
    }

    if (chunkSize & 1u) {
      if (!skipBytes(file, 1)) {
        return false;
      }
    }
  }

  if (!haveFmt || !haveData) {
    return false;
  }

  return info.format == 1 &&
         info.channels == 1 &&
         info.sampleRate == 16000 &&
         info.bitsPerSample == 16 &&
         (info.dataBytes % sizeof(int16_t)) == 0;
}

}  // namespace

struct Player::Impl {
  std::mutex mutex;
  std::condition_variable cv;

  TaskHandle_t task = nullptr;

  bool stopRequested = false;
  bool running = false;
  bool success = false;

  char error[128] = "";

  uint8_t* fileBuffer = nullptr;
  std::string taskPath;

  void setError(const char* text) {
    std::strncpy(
        error,
        text != nullptr ? text : "Unknown error",
        sizeof(error) - 1);

    error[sizeof(error) - 1] = '\0';
  }

  static void taskEntry(void* arg) {
    static_cast<Impl*>(arg)->run();
    vTaskDeleteWithCaps(nullptr);
  }

  bool shouldStop() {
    std::lock_guard<std::mutex> lock(mutex);
    return stopRequested;
  }

  void run() {
    HalFile file;
    std::string pathCopy;

    {
      std::lock_guard<std::mutex> lock(mutex);
      pathCopy = taskPath;
    }

    file = Storage.open(
        pathCopy.c_str(),
        O_RDONLY);

    if (!file.isOpen()) {
      LOG_ERR(
          kTag,
          "open failed: %s",
          pathCopy.c_str());

      setError("Cannot open audio file");
      finish(false);
      return;
    }

    WavInfo wav;

    if (!parseWav(file, wav)) {
      LOG_ERR(
          kTag,
          "unsupported WAV: %s",
          pathCopy.c_str());

      setError(
          "WAV must be PCM/mono/16k/16-bit");

      finish(false);
      return;
    }

    if (fileBuffer == nullptr) {
      setError(
          "PSRAM buffer allocation failed");

      finish(false);
      return;
    }

    LOG_INF(
        kTag,
        "WAV stream: path=%s size=%u data=%u "
        "rate=%u ch=%u bits=%u",
        pathCopy.c_str(),
        static_cast<unsigned>(file.fileSize()),
        static_cast<unsigned>(wav.dataBytes),
        static_cast<unsigned>(wav.sampleRate),
        static_cast<unsigned>(wav.channels),
        static_cast<unsigned>(wav.bitsPerSample));

    LOG_INF(
        kTag,
        "PSRAM WAV buffer=%p bytes=%u external=%d",
        fileBuffer,
        static_cast<unsigned>(kFileBufferBytes),
        esp_ptr_external_ram(fileBuffer) ? 1 : 0);

    if (!file.seek(wav.dataOffset)) {
      setError("WAV data seek failed");
      finish(false);
      return;
    }

    std::size_t remaining = wav.dataBytes;
    std::size_t totalSamples = 0;

    while (remaining > 0) {
      if (shouldStop()) {
        setError("Playback stopped");
        finish(false);
        return;
      }

      const std::size_t bytesToRead =
          std::min(
              remaining,
              kFileBufferBytes);

      const int n =
          file.read(
              fileBuffer,
              bytesToRead);

      if (n <= 0) {
        LOG_ERR(
            kTag,
            "WAV stream: SD read failed");

        setError("SD read failed");
        finish(false);
        return;
      }

      if ((static_cast<std::size_t>(n) &
           (sizeof(int16_t) - 1)) != 0) {
        setError("Odd WAV data length");
        finish(false);
        return;
      }

      const std::size_t samples =
          static_cast<std::size_t>(n) /
          sizeof(int16_t);

      // Use the same synchronous playback path
      // that was proven by the successful OGG
      // in-RAM speaker test.
      //
      // Only this 32 KiB window is resident in
      // PSRAM; the WAV remains on SD.
      if (!metalio_audio_service::playPcm(
              reinterpret_cast<const int16_t*>(
                  fileBuffer),
              samples,
              3000)) {
        LOG_ERR(
            kTag,
            "WAV stream aborted: audio output failed "
            "at sample=%u",
            static_cast<unsigned>(totalSamples));

        metalio_audio_service::flushPlayback();
        metalio_audio_service::stop();

        setError("Audio output failed");
        finish(false);
        return;
      }

      totalSamples += samples;
      remaining -= static_cast<std::size_t>(n);
    }

    LOG_INF(
        kTag,
        "WAV stream finished: samples=%u "
        "duration_ms=%u",
        static_cast<unsigned>(totalSamples),
        static_cast<unsigned>(
            (static_cast<uint64_t>(totalSamples) *
             1000ULL) /
            16000ULL));

    finish(true);
  }

  void finish(bool result) {
    {
      std::lock_guard<std::mutex> lock(mutex);

      success = result;
      running = false;
      task = nullptr;
    }

    cv.notify_all();
  }

  bool start(const char* path) {
    if (path == nullptr || path[0] == '\0') {
      setError("Invalid audio path");
      return false;
    }

    std::lock_guard<std::mutex> lifecycle(mutex);

    if (running || task != nullptr) {
      setError(
          "Audio playback already running");
      return false;
    }

    if (!metalio_audio_service::start()) {
      setError("Audio service start failed");
      return false;
    }

    if (fileBuffer == nullptr) {
      fileBuffer = static_cast<uint8_t*>(
          heap_caps_malloc(
              kFileBufferBytes,
              MALLOC_CAP_SPIRAM |
                  MALLOC_CAP_8BIT));

      if (fileBuffer == nullptr) {
        setError(
            "PSRAM file buffer allocation failed");
        return false;
      }
    }

    taskPath = path;
    stopRequested = false;
    success = false;
    running = true;
    error[0] = '\0';

    if (xTaskCreateWithCaps(
            taskEntry,
            "metalio_file",
            kTaskStackBytes,
            this,
            kTaskPriority,
            &task,
            kTaskCaps) != pdPASS) {
      running = false;
      task = nullptr;
      setError(
          "File playback task creation failed");
      return false;
    }

    LOG_INF(
        kTag,
        "file playback task started");

    return true;
  }

  void requestStop() {
    std::lock_guard<std::mutex> lock(mutex);
    stopRequested = true;
  }
};

Player::Player()
    : impl_(new Impl()) {}

Player::~Player() {
  stop();
  delete impl_;
}

bool Player::playWav(const char* path) {
  return impl_ != nullptr &&
         impl_->start(path);
}

bool Player::wait(uint32_t timeoutMs) {
  if (impl_ == nullptr) {
    return false;
  }

  std::unique_lock<std::mutex> lock(
      impl_->mutex);

  const bool done =
      impl_->cv.wait_for(
          lock,
          std::chrono::milliseconds(timeoutMs),
          [this] {
            return !impl_->running;
          });

  return done && impl_->success;
}

void Player::stop() {
  if (impl_ == nullptr) {
    return;
  }

  impl_->requestStop();

  std::unique_lock<std::mutex> lock(
      impl_->mutex);

  impl_->cv.wait_for(
      lock,
      std::chrono::milliseconds(3000),
      [this] {
        return !impl_->running;
      });

  if (impl_->fileBuffer != nullptr) {
    heap_caps_free(
        impl_->fileBuffer);

    impl_->fileBuffer = nullptr;
  }
}

bool Player::isPlaying() const {
  if (impl_ == nullptr) {
    return false;
  }

  std::lock_guard<std::mutex> lock(
      impl_->mutex);

  return impl_->running;
}

bool Player::lastSuccess() const {
  if (impl_ == nullptr) {
    return false;
  }

  std::lock_guard<std::mutex> lock(
      impl_->mutex);

  return impl_->success;
}

const char* Player::lastError() const {
  if (impl_ == nullptr) {
    return "Player unavailable";
  }

  std::lock_guard<std::mutex> lock(
      impl_->mutex);

  return impl_->error;
}

}  // namespace metalio_audio_file_player

#else

namespace metalio_audio_file_player {

Player::Player()
    : impl_(nullptr) {}

Player::~Player() = default;

bool Player::playWav(const char*) {
  return false;
}

bool Player::wait(uint32_t) {
  return false;
}

void Player::stop() {}

bool Player::isPlaying() const {
  return false;
}

bool Player::lastSuccess() const {
  return false;
}

const char* Player::lastError() const {
  return "Metalio E-INK4 only";
}

}  // namespace metalio_audio_file_player

#endif
