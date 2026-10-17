#include "MetalioOggFilePlayer.h"

#if FREEINK_DEVICE_METALIO_EINK4

#include <esp_heap_caps.h>

#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/queue.h>
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
#include <Logging.h>

#include "MetalioAudioService.h"

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

namespace metalio_ogg_file_player {

namespace {

constexpr char kTag[] = "METALIO-OGG";

constexpr std::size_t kCompressedBufferBytes = 32 * 1024;

constexpr std::size_t kPcmBufferSamples = 4096;
constexpr std::size_t kPcmBufferCount = 2;

constexpr uint32_t kTaskStackBytes = 12 * 1024;

constexpr UBaseType_t kReaderPriority = 3;
constexpr UBaseType_t kOutputPriority = 4;

constexpr UBaseType_t kTaskCaps =
    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

constexpr uint32_t kOutputSampleRate = 16000;

/*
 * First hardware test:
 *
 * Ogg Vorbis
 * mono
 * 16 kHz
 *
 * This matches test1.ogg.
 */

struct PcmBlock {
  int16_t* data = nullptr;
  std::size_t samples = 0;
  bool end = false;
};

}  // namespace


struct Player::Impl {

  std::mutex mutex;
  std::condition_variable cv;

  TaskHandle_t readerTask = nullptr;
  TaskHandle_t outputTask = nullptr;

  QueueHandle_t freeQ = nullptr;
  QueueHandle_t readyQ = nullptr;

  int16_t* pcm[kPcmBufferCount] = {
      nullptr,
      nullptr,
  };

  bool stopRequested = false;
  bool running = false;
  bool success = false;
  bool failed = false;

  char error[160] = "";

  std::string path;


  void setError(const char* text) {

    std::lock_guard<std::mutex> lock(mutex);

    std::strncpy(
        error,
        text ? text : "Unknown error",
        sizeof(error) - 1);

    error[sizeof(error) - 1] = '\0';
  }


  bool isStopped() {

    std::lock_guard<std::mutex> lock(mutex);

    return stopRequested;
  }


  void finish(bool ok) {

    {
      std::lock_guard<std::mutex> lock(mutex);

      success = ok;
      running = false;
    }

    cv.notify_all();
  }


  void fail(const char* text) {

    setError(text);

    {
      std::lock_guard<std::mutex> lock(mutex);

      failed = true;
      stopRequested = true;
    }

    if (readyQ != nullptr) {

      PcmBlock end;
      end.end = true;

      xQueueSend(
          readyQ,
          &end,
          0);
    }
  }


  static void readerEntry(void* arg) {

    auto* self =
        static_cast<Impl*>(arg);

    self->reader();

    {
      std::lock_guard<std::mutex> lock(
          self->mutex);

      self->readerTask = nullptr;
    }

    self->cv.notify_all();

    vTaskDeleteWithCaps(nullptr);
  }


  static void outputEntry(void* arg) {

    auto* self =
        static_cast<Impl*>(arg);

    self->output();

    {
      std::lock_guard<std::mutex> lock(
          self->mutex);

      self->outputTask = nullptr;
    }

    self->cv.notify_all();

    vTaskDeleteWithCaps(nullptr);
  }


  bool allocatePcmBuffers() {

    freeQ =
        xQueueCreate(
            kPcmBufferCount,
            sizeof(PcmBlock));

    readyQ =
        xQueueCreate(
            kPcmBufferCount,
            sizeof(PcmBlock));

    if (!freeQ || !readyQ) {

      setError(
          "PCM queue allocation failed");

      return false;
    }


    for (std::size_t i = 0;
         i < kPcmBufferCount;
         ++i) {

      pcm[i] =
          static_cast<int16_t*>(
              heap_caps_malloc(
                  kPcmBufferSamples *
                      sizeof(int16_t),
                  kTaskCaps));

      if (!pcm[i]) {

        setError(
            "PSRAM PCM buffer allocation failed");

        return false;
      }


      PcmBlock block;

      block.data = pcm[i];
      block.samples = 0;
      block.end = false;

      if (xQueueSend(
              freeQ,
              &block,
              0) != pdTRUE) {

        setError(
            "PCM free queue failed");

        return false;
      }
    }

    return true;
  }


  void destroyPcmBuffers() {

    if (readyQ != nullptr) {

      vQueueDelete(readyQ);
      readyQ = nullptr;
    }


    if (freeQ != nullptr) {

      vQueueDelete(freeQ);
      freeQ = nullptr;
    }


    for (auto& buffer : pcm) {

      if (buffer != nullptr) {

        heap_caps_free(buffer);
        buffer = nullptr;
      }
    }
  }


  bool startTasks() {

    if (xTaskCreateWithCaps(
            readerEntry,
            "metalio_ogg_rd",
            kTaskStackBytes,
            this,
            kReaderPriority,
            &readerTask,
            kTaskCaps) != pdPASS) {

      readerTask = nullptr;

      setError(
          "OGG reader task creation failed");

      return false;
    }


    if (xTaskCreateWithCaps(
            outputEntry,
            "metalio_ogg_out",
            kTaskStackBytes,
            this,
            kOutputPriority,
            &outputTask,
            kTaskCaps) != pdPASS) {

      outputTask = nullptr;

      setError(
          "OGG output task creation failed");

      return false;
    }

    return true;
  }


  /*
   * SD -> PSRAM -> Vorbis decoder -> PCM queue
   *
   * The SD file remains OPEN during the
   * complete streaming decode.
   */
  void reader() {

    HalFile file =
        Storage.open(
            path.c_str(),
            O_RDONLY);


    if (!file.isOpen()) {

      fail(
          "Cannot open OGG file");

      return;
    }


    const std::size_t fileSize =
        file.fileSize();


    if (fileSize == 0) {

      fail(
          "OGG file is empty");

      return;
    }


    /*
     * Compressed OGG streaming buffer.
     */
    auto* compressed =
        static_cast<uint8_t*>(
            heap_caps_malloc(
                kCompressedBufferBytes,
                kTaskCaps));


    if (compressed == nullptr) {

      fail(
          "PSRAM OGG buffer allocation failed");

      return;
    }


    LOG_INF(
        kTag,
        "SD OPEN: %s size=%u",
        path.c_str(),
        static_cast<unsigned>(
            fileSize));


    LOG_INF(
        kTag,
        "Compressed PSRAM buffer=%u bytes",
        static_cast<unsigned>(
            kCompressedBufferBytes));


    std::size_t used = 0;
    bool eof = false;

    uint32_t readCount = 0;
    uint32_t decodedSamples = 0;


    stb_vorbis* decoder = nullptr;


    /*
     * Read more compressed data from SD.
     */
    auto readMore =
        [&]() -> bool {

      if (eof ||
          used >= kCompressedBufferBytes) {

        return false;
      }


      const std::size_t available =
          kCompressedBufferBytes - used;


      const int bytesRead =
          file.read(
              compressed + used,
              available);


      ++readCount;


      if (bytesRead < 0) {

        fail(
            "SD read failed");

        return false;
      }


      if (bytesRead == 0) {

        eof = true;

        return false;
      }


      used +=
          static_cast<std::size_t>(
              bytesRead);

      return true;
    };


    /*
     * --------------------------------------------------
     * Decoder initialization
     * --------------------------------------------------
     */
    while (
        decoder == nullptr &&
        !isStopped()) {

      if (used == 0 &&
          !readMore()) {

        if (eof) {

          fail(
              "OGG header read failed");
        }

        break;
      }


      int dataUsed = 0;
      int error = 0;


      decoder =
          stb_vorbis_open_pushdata(
              compressed,
              static_cast<int>(
                  used),
              &dataUsed,
              &error,
              nullptr);


      if (decoder != nullptr) {

        /*
         * dataUsed is compressed bytes
         * consumed during decoder setup.
         */
        if (dataUsed > 0) {

          if (static_cast<std::size_t>(
                  dataUsed) > used) {

            stb_vorbis_close(
                decoder);

            decoder = nullptr;

            fail(
                "Vorbis consumed invalid bytes");

            break;
          }


          const std::size_t remaining =
              used -
              static_cast<std::size_t>(
                  dataUsed);


          std::memmove(
              compressed,
              compressed + dataUsed,
              remaining);


          used = remaining;
        }


        const stb_vorbis_info info =
            stb_vorbis_get_info(
                decoder);


        LOG_INF(
            kTag,
            "Decoder PASS: rate=%u channels=%d",
            static_cast<unsigned>(
                info.sample_rate),
            info.channels);


        if (info.sample_rate !=
                kOutputSampleRate ||
            info.channels != 1) {

          stb_vorbis_close(
              decoder);

          decoder = nullptr;

          fail(
              "Test OGG must be mono/16kHz");

          break;
        }


        LOG_INF(
            kTag,
            "SD STILL OPEN: streaming decode starts");

        break;
      }


      /*
       * Decoder needs more data.
       */
      if (eof) {

        fail(
            "OGG decoder initialization failed");

        break;
      }


      if (used >=
          kCompressedBufferBytes) {

        fail(
            "OGG decoder buffer full");

        break;
      }


      readMore();
    }


    /*
     * --------------------------------------------------
     * Streaming decode
     * --------------------------------------------------
     */
    if (decoder != nullptr) {

      while (!isStopped()) {

        if (used == 0 &&
            !eof) {

          readMore();
        }


        /*
         * IMPORTANT:
         *
         * dataUsed          = compressed bytes consumed
         * channels           = decoded channel count
         * samplesPerChannel  = decoded PCM frames
         */
        int dataUsed = 0;
        int channels = 0;
        int samplesPerChannel = 0;

        float** output = nullptr;


        if (used > 0) {

          dataUsed =
              stb_vorbis_decode_frame_pushdata(
                  decoder,
                  compressed,
                  static_cast<int>(
                      used),
                  &channels,
                  &output,
                  &samplesPerChannel);
        }


        /*
         * Remove compressed bytes consumed
         * by stb_vorbis.
         */
        if (dataUsed > 0) {

          if (static_cast<std::size_t>(
                  dataUsed) > used) {

            fail(
                "Vorbis consumed invalid bytes");

            break;
          }


          const std::size_t remaining =
              used -
              static_cast<std::size_t>(
                  dataUsed);


          std::memmove(
              compressed,
              compressed + dataUsed,
              remaining);


          used = remaining;
        }


        /*
         * First hardware test is mono.
         */
        if (channels != 0 &&
            channels != 1) {

          fail(
              "Unexpected Vorbis channel count");

          break;
        }


        /*
         * Convert stb_vorbis float PCM
         * to int16 PCM blocks.
         */
        if (samplesPerChannel > 0 &&
            output != nullptr &&
            output[0] != nullptr) {

          std::size_t offset = 0;


          while (
              offset <
              static_cast<std::size_t>(
                  samplesPerChannel)) {

            if (isStopped()) {
              break;
            }


            PcmBlock block;


            if (xQueueReceive(
                    freeQ,
                    &block,
                    pdMS_TO_TICKS(1000))
                != pdTRUE) {

              fail(
                  "PCM buffer starvation");

              break;
            }


            const std::size_t count =
                std::min(
                    kPcmBufferSamples,
                    static_cast<std::size_t>(
                        samplesPerChannel) -
                        offset);


            for (std::size_t i = 0;
                 i < count;
                 ++i) {

              float sample =
                  output[0][offset + i];


              if (sample > 1.0f) {

                sample = 1.0f;

              } else if (sample < -1.0f) {

                sample = -1.0f;
              }


              block.data[i] =
                  static_cast<int16_t>(
                      sample * 32767.0f);
            }


            block.samples = count;
            block.end = false;


            if (xQueueSend(
                    readyQ,
                    &block,
                    pdMS_TO_TICKS(1000))
                != pdTRUE) {

              xQueueSend(
                  freeQ,
                  &block,
                  portMAX_DELAY);

              fail(
                  "PCM ready queue failed");

              break;
            }


            offset += count;


            decodedSamples +=
                static_cast<uint32_t>(
                    count);
          }


          if (failed) {
            break;
          }
        }


        /*
         * No progress.
         *
         * stb_vorbis needs more compressed
         * data in this case.
         */
        if (samplesPerChannel == 0 &&
            dataUsed == 0) {

          if (eof) {

            LOG_INF(
                kTag,
                "OGG EOF: reads=%u decoded=%u samples",
                static_cast<unsigned>(
                    readCount),
                static_cast<unsigned>(
                    decodedSamples));

            break;
          }


          if (used >=
              kCompressedBufferBytes) {

            fail(
                "Vorbis buffer full without progress");

            break;
          }


          readMore();

        } else if (
            !eof &&
            used <
                kCompressedBufferBytes / 2) {

          /*
           * Keep the streaming buffer
           * reasonably full.
           */
          readMore();
        }
      }


      stb_vorbis_close(
          decoder);

      decoder = nullptr;
    }


    /*
     * SD closes ONLY after the decoder
     * has finished streaming.
     */
    file.close();


    LOG_INF(
        kTag,
        "SD CLOSED: OGG streaming finished");


    heap_caps_free(
        compressed);


    /*
     * Tell output task that the stream ended.
     */
    PcmBlock end;
    end.end = true;


    if (readyQ != nullptr) {

      xQueueSend(
          readyQ,
          &end,
          portMAX_DELAY);
    }
  }


  /*
   * PCM queue -> MetalioAudioService -> speaker.
   */
  void output() {

    if (!metalio_audio_service::start()) {

      fail(
          "Audio service start failed");

      finish(false);

      return;
    }


    bool ok = true;


    while (true) {

      PcmBlock block;


      if (xQueueReceive(
              readyQ,
              &block,
              pdMS_TO_TICKS(100))
          != pdTRUE) {

        if (isStopped()) {

          ok = false;

          break;
        }

        continue;
      }


      if (block.end) {
        break;
      }


      if (!metalio_audio_service::playPcm(
              block.data,
              block.samples,
              3000)) {

        ok = false;

        fail(
            "PCM speaker playback failed");

        break;
      }


      if (xQueueSend(
              freeQ,
              &block,
              portMAX_DELAY)
          != pdTRUE) {

        ok = false;

        fail(
            "PCM free queue failed");

        break;
      }
    }


    metalio_audio_service::waitPlaybackDrained(
        10000);

    metalio_audio_service::stop();


    bool wasFailed;

    {
      std::lock_guard<std::mutex> lock(
          mutex);

      wasFailed = failed;
    }


    if (wasFailed || !ok) {

      finish(false);

    } else if (!isStopped()) {

      finish(true);
    }
  }


  bool start(const char* filePath) {

    if (filePath == nullptr ||
        *filePath == '\0') {

      setError(
          "Invalid OGG path");

      return false;
    }


    bool busy = false;


    {
      std::lock_guard<std::mutex> lock(
          mutex);


      busy =
          running ||
          readerTask != nullptr ||
          outputTask != nullptr;


      if (!busy) {

        path = filePath;

        stopRequested = false;
        success = false;
        failed = false;
        running = true;

        error[0] = '\0';
      }
    }


    if (busy) {

      setError(
          "OGG playback already running");

      return false;
    }


    if (!allocatePcmBuffers() ||
        !startTasks()) {

      requestStop();

      waitTasks(5000);

      destroyPcmBuffers();

      finish(false);

      return false;
    }


    LOG_INF(
        kTag,
        "OGG streaming tasks started");


    return true;
  }


  void requestStop() {

    std::lock_guard<std::mutex> lock(
        mutex);

    stopRequested = true;
  }


  bool waitTasks(uint32_t timeoutMs) {

    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(
            timeoutMs);


    while (
        readerTask != nullptr ||
        outputTask != nullptr) {

      if (
          std::chrono::steady_clock::now() >=
          deadline) {

        return false;
      }


      vTaskDelay(
          pdMS_TO_TICKS(10));
    }


    return true;
  }


  void stop() {

    requestStop();

    waitTasks(10000);

    metalio_audio_service::stop();


    if (!readerTask &&
        !outputTask) {

      destroyPcmBuffers();

      std::lock_guard<std::mutex> lock(
          mutex);

      running = false;
    }


    cv.notify_all();
  }
};


Player::Player()
    : impl_(new Impl()) {}


Player::~Player() {

  stop();

  delete impl_;
}


bool Player::play(
    const char* path) {

  return impl_ &&
         impl_->start(path);
}


bool Player::wait(
    uint32_t timeoutMs) {

  if (!impl_) {
    return false;
  }


  std::unique_lock<std::mutex> lock(
      impl_->mutex);


  const bool done =
      impl_->cv.wait_for(
          lock,
          std::chrono::milliseconds(
              timeoutMs),
          [this] {
            return !impl_->running;
          });


  return done &&
         impl_->success;
}


void Player::stop() {

  if (impl_) {
    impl_->stop();
  }
}


bool Player::isPlaying() const {

  if (!impl_) {
    return false;
  }


  std::lock_guard<std::mutex> lock(
      impl_->mutex);

  return impl_->running;
}


bool Player::lastSuccess() const {

  if (!impl_) {
    return false;
  }


  std::lock_guard<std::mutex> lock(
      impl_->mutex);

  return impl_->success;
}


const char* Player::lastError() const {

  if (!impl_) {
    return "Player unavailable";
  }


  std::lock_guard<std::mutex> lock(
      impl_->mutex);

  return impl_->error;
}

}  // namespace metalio_ogg_file_player


#else

namespace metalio_ogg_file_player {

Player::Player()
    : impl_(nullptr) {}

Player::~Player() = default;

bool Player::play(const char*) {
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

}  // namespace metalio_ogg_file_player

#endif
