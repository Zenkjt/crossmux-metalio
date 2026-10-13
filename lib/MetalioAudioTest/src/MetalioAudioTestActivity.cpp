#include "MetalioAudioTestActivity.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>

#include <HalStorage.h>

#include "MetalioAudioService.h"
#include "../../MetalioAudio/src/metalio/MetalioPsramBuffer.h"

namespace fui = freeink::ui;

namespace {

constexpr const char* TAG = "MTAUD";

constexpr uint32_t kSampleRate = 16000;
constexpr std::size_t kToneSamples = 6400;  // 400 ms
constexpr float kToneFrequencyHz = 1000.0f;
constexpr float kPi = 3.14159265358979323846f;

constexpr char kMusicDir[] = "/music";
constexpr std::size_t kMaxTracks = 200;

constexpr std::size_t kDecodeFrames = 960;
constexpr std::size_t kOggMaxBytes = 6 * 1024 * 1024;

constexpr uint32_t kOggPlaybackTimeoutMs = 10000;

bool hasOggExtension(const std::string& path) {
  if (path.size() < 4) return false;

  const std::size_t pos = path.size() - 4;

  return path[pos] == '.' &&
         (path[pos + 1] == 'o' || path[pos + 1] == 'O') &&
         (path[pos + 2] == 'g' || path[pos + 2] == 'G') &&
         (path[pos + 3] == 'g' || path[pos + 3] == 'G');
}


/*
 * Test-only OGG source.
 *
 * Important:
 *
 *   SD /music/file.ogg
 *          ↓
 *   read entire file
 *          ↓
 *       PSRAM
 *          ↓
 *     close SD file
 *          ↓
 *   stb_vorbis_open_memory()
 *          ↓
 *      decode only
 *
 * Once open() returns successfully, the SD file is already closed.
 */
class OggPsrTestSource final {
 public:
  ~OggPsrTestSource() {
    close();
  }

  bool open(HalFile&& file) {
    close();

    file_ = std::move(file);

    if (!file_.isOpen()) {
      LOG_ERR(TAG, "OGG: file is not open");
      return false;
    }

    const std::size_t size = file_.fileSize();

    LOG_INF(
        TAG,
        "OGG: file=%u bytes",
        static_cast<unsigned>(size));

    if (size == 0 || size > kOggMaxBytes) {
      LOG_ERR(
          TAG,
          "OGG: invalid size=%u (max=%u)",
          static_cast<unsigned>(size),
          static_cast<unsigned>(kOggMaxBytes));

      close();
      return false;
    }

    /*
     * Allocate compressed OGG data in PSRAM.
     */
    data_ = static_cast<unsigned char*>(
        heap_caps_malloc(
            size,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    if (data_ == nullptr) {
      LOG_ERR(
          TAG,
          "OGG: PSRAM allocation failed: %u bytes",
          static_cast<unsigned>(size));

      close();
      return false;
    }

    dataSize_ = size;

    LOG_INF(
        TAG,
        "OGG: PSRAM allocated=%u bytes",
        static_cast<unsigned>(size));

    if (!file_.seek(0)) {
      LOG_ERR(TAG, "OGG: seek(0) failed");
      close();
      return false;
    }

    const int bytesRead = file_.read(data_, size);

    if (bytesRead != static_cast<int>(size)) {
      LOG_ERR(
          TAG,
          "OGG: SD read failed: %d/%u bytes",
          bytesRead,
          static_cast<unsigned>(size));

      close();
      return false;
    }

    /*
     * Critical isolation point.
     *
     * The complete compressed OGG is now in PSRAM.
     * Close the SD file BEFORE decoder/playback starts.
     */
    file_.close();

    LOG_INF(
        TAG,
        "OGG: SD read complete; SD file closed");

    int error = 0;

    decoder_ = stb_vorbis_open_memory(
        data_,
        static_cast<int>(dataSize_),
        &error,
        nullptr);

    if (decoder_ == nullptr) {
      LOG_ERR(
          TAG,
          "OGG: stb_vorbis_open_memory failed: %d",
          error);

      close();
      return false;
    }

    const stb_vorbis_info info =
        stb_vorbis_get_info(decoder_);

    sourceRate_ =
        static_cast<uint32_t>(info.sample_rate);

    sourceChannels_ = info.channels;

    if (sourceRate_ == 0 || sourceChannels_ <= 0) {
      LOG_ERR(
          TAG,
          "OGG: invalid decoder info rate=%u channels=%d",
          static_cast<unsigned>(sourceRate_),
          sourceChannels_);

      close();
      return false;
    }

    const unsigned totalFrames =
        stb_vorbis_stream_length_in_samples(decoder_);

    durationMs_ = static_cast<uint32_t>(
        (static_cast<uint64_t>(totalFrames) * 1000ULL) /
        sourceRate_);

    LOG_INF(
        TAG,
        "OGG: decoder PASS rate=%u channels=%d duration=%u ms",
        static_cast<unsigned>(sourceRate_),
        sourceChannels_,
        static_cast<unsigned>(durationMs_));

    eof_ = false;

    return true;
  }

  bool readFrames(
      MetalioPsramVector<int16_t>& out,
      std::size_t maxFrames) {

    out.clear();

    if (decoder_ == nullptr ||
        maxFrames == 0 ||
        eof_) {
      return false;
    }

    /*
     * Decoder working buffer is also PSRAM.
     */
    if (decoded_.size() != kDecodeFrames * 2) {
      decoded_.resize(kDecodeFrames * 2);
    }

    while (out.size() < maxFrames && !eof_) {

      /*
       * First consume any resampled PCM left over
       * from the previous decoder block.
       */
      if (pendingOffset_ < pending_.size()) {

        const std::size_t count =
            std::min(
                maxFrames - out.size(),
                pending_.size() - pendingOffset_);

        out.insert(
            out.end(),
            pending_.begin() + pendingOffset_,
            pending_.begin() + pendingOffset_ + count);

        pendingOffset_ += count;

        continue;
      }

      pending_.clear();
      pendingOffset_ = 0;

      /*
       * Decode directly from the OGG buffer in PSRAM.
       *
       * No SD access occurs here.
       */
      const int frames =
          stb_vorbis_get_samples_short_interleaved(
              decoder_,
              2,
              decoded_.data(),
              static_cast<int>(decoded_.size()));

      if (frames <= 0) {
        eof_ = true;
        break;
      }

      const int channels =
          std::max(1, sourceChannels_);

      const double step =
          static_cast<double>(sourceRate_) /
          static_cast<double>(kSampleRate);

      /*
       * Resample to 16 kHz mono.
       */
      for (double sourcePosition = 0.0;
           sourcePosition < frames;
           sourcePosition += step) {

        const int index0 =
            static_cast<int>(sourcePosition);

        const double frac =
            sourcePosition - index0;

        const int index1 =
            std::min(index0 + 1, frames - 1);

        const int16_t left0 =
            decoded_[index0 * 2];

        const int16_t right0 =
            channels > 1
                ? decoded_[index0 * 2 + 1]
                : left0;

        const int16_t left1 =
            decoded_[index1 * 2];

        const int16_t right1 =
            channels > 1
                ? decoded_[index1 * 2 + 1]
                : left1;

        const int32_t mono0 =
            (static_cast<int32_t>(left0) +
             static_cast<int32_t>(right0)) /
            2;

        const int32_t mono1 =
            (static_cast<int32_t>(left1) +
             static_cast<int32_t>(right1)) /
            2;

        const int32_t sample =
            mono0 +
            static_cast<int32_t>(
                (mono1 - mono0) * frac);

        pending_.push_back(
            static_cast<int16_t>(
                std::clamp(
                    sample,
                    static_cast<int32_t>(-32768),
                    static_cast<int32_t>(32767))));
      }
    }

    return !out.empty();
  }

  bool eof() const {
    return eof_;
  }

  void close() {
    if (decoder_ != nullptr) {
      stb_vorbis_close(decoder_);
      decoder_ = nullptr;
    }

    if (data_ != nullptr) {
      heap_caps_free(data_);
      data_ = nullptr;
    }

    file_.close();

    dataSize_ = 0;
    sourceRate_ = 0;
    sourceChannels_ = 0;
    durationMs_ = 0;

    pending_.clear();
    pendingOffset_ = 0;

    eof_ = true;
  }

 private:
  HalFile file_;

  /*
   * Complete compressed OGG buffer.
   * Allocated in PSRAM.
   */
  unsigned char* data_ = nullptr;
  std::size_t dataSize_ = 0;

  stb_vorbis* decoder_ = nullptr;

  /*
   * Resampled PCM waiting to be consumed.
   * PSRAM vector.
   */
  MetalioPsramVector<int16_t> pending_;
  std::size_t pendingOffset_ = 0;

  /*
   * Decoder working buffer.
   * PSRAM vector.
   */
  MetalioPsramVector<int16_t> decoded_;

  uint32_t sourceRate_ = 0;
  int sourceChannels_ = 0;
  uint32_t durationMs_ = 0;

  bool eof_ = true;
};

}  // namespace


void MetalioAudioTestActivity::onEnter() {
  UiListActivity::onEnter();

  rebuildRows();

  if (!metalio_audio_service::start()) {
    setStatus("Audio service start failed");
  } else {
    setStatus("Ready");
  }
}


void MetalioAudioTestActivity::onExit() {
  metalio_audio_service::stop();

  UiListActivity::onExit();
}


void MetalioAudioTestActivity::rebuildRows() {
  rows[kSpeakerTest] = fui::ListItem{};
  rows[kSpeakerTest].label = "Speaker test";
  rows[kSpeakerTest].actionValue = kSpeakerTest;

  rows[kMicTest] = fui::ListItem{};
  rows[kMicTest].label = "Microphone test";
  rows[kMicTest].actionValue = kMicTest;

  rows[kOpusLoopbackTest] = fui::ListItem{};
  rows[kOpusLoopbackTest].label = "Opus loopback";
  rows[kOpusLoopbackTest].actionValue =
      kOpusLoopbackTest;

  rows[kOggPsrTest] = fui::ListItem{};
  rows[kOggPsrTest].label = "OGG PSRAM playback";
  rows[kOggPsrTest].actionValue = kOggPsrTest;

  rows[kStatusRow] = fui::ListItem{};
  rows[kStatusRow].label = statusText;
  rows[kStatusRow].actionValue = kStatusRow;
}


void MetalioAudioTestActivity::setStatus(
    const char* text) {

  if (text == nullptr) {
    statusText[0] = '\0';
  } else {
    std::strncpy(
        statusText,
        text,
        sizeof(statusText) - 1);

    statusText[sizeof(statusText) - 1] = '\0';
  }

  rebuildRows();
  requestUpdate();
}


void MetalioAudioTestActivity::buildScreen(
    UiScreen& screen) {

  const int screenWidth =
      renderer.getScreenWidth();

  const int screenHeight =
      renderer.getScreenHeight();

  const Rect content =
      pageContentRect();

  screen.setContentMarginFromScreen(
      fui::Insets{
          static_cast<int16_t>(content.y),

          static_cast<int16_t>(
              screenWidth -
              content.x -
              content.width),

          static_cast<int16_t>(
              screenHeight -
              content.y -
              content.height),

          static_cast<int16_t>(
              content.x)});

  fui::ListProps props;

  props.items = rows.data();

  props.count =
      static_cast<uint16_t>(rows.size());

  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;

  syncListViewport(screen, props);

  screen.list(props);
}


void MetalioAudioTestActivity::activateIndex(
    int index) {

  if (index == kStatusRow) {
    return;
  }

  app.clearTapFlash();

  switch (index) {

    case kSpeakerTest: {
      setStatus("Playing 1 kHz tone...");

      std::vector<int16_t> tone(
          kToneSamples);

      for (std::size_t i = 0;
           i < tone.size();
           ++i) {

        const float phase =
            2.0f *
            kPi *
            kToneFrequencyHz *
            static_cast<float>(i) /
            static_cast<float>(kSampleRate);

        tone[i] =
            static_cast<int16_t>(
                std::sin(phase) * 12000.0f);
      }

      const bool ok =
          metalio_audio_service::playPcm(
              tone.data(),
              tone.size(),
              3000) &&
          metalio_audio_service::waitPlaybackDrained(
              3000);

      setStatus(
          ok
              ? "Speaker test PASS"
              : "Speaker test FAIL");

      LOG_INF(
          TAG,
          "Speaker test: %s",
          ok ? "PASS" : "FAIL");

      break;
    }


    case kMicTest: {
      setStatus("Recording microphone...");

      if (runMicTest()) {
        setStatus("Microphone test PASS");
      } else {
        setStatus("Microphone test FAIL");
      }

      break;
    }


    case kOpusLoopbackTest: {
      setStatus("Mic > Opus > speaker...");

      if (runOpusLoopbackTest()) {
        setStatus("Opus loopback PASS");
      } else {
        setStatus("Opus loopback FAIL");
      }

      break;
    }


    case kOggPsrTest: {
      setStatus("Loading OGG into PSRAM...");

      if (runOggPsrTest()) {
        setStatus("OGG PSRAM PASS");
      } else {
        setStatus("OGG PSRAM FAIL");
      }

      break;
    }


    default:
      break;
  }
}


bool MetalioAudioTestActivity::runMicTest() {

  if (!metalio_audio_service::startCapture(false)) {
    LOG_ERR(
        TAG,
        "Mic test: capture start failed");

    return false;
  }

  std::vector<int16_t> samples;

  const bool ok =
      metalio_audio_service::readCapturedPcm(
          samples,
          kSampleRate * 2,
          3000);

  metalio_audio_service::stopCapture();

  if (!ok || samples.empty()) {
    LOG_ERR(
        TAG,
        "Mic test: no PCM captured");

    return false;
  }

  int peak = 0;
  double sumSquares = 0.0;

  for (const int16_t sample : samples) {

    const int value =
        std::abs(static_cast<int>(sample));

    peak =
        std::max(peak, value);

    sumSquares +=
        static_cast<double>(sample) *
        static_cast<double>(sample);
  }

  const double rms =
      std::sqrt(
          sumSquares /
          static_cast<double>(samples.size()));

  LOG_INF(
      TAG,
      "MIC PASS: samples=%u rms=%.1f peak=%d",
      static_cast<unsigned>(samples.size()),
      rms,
      peak);

  return true;
}


bool MetalioAudioTestActivity::runOpusLoopbackTest() {

  if (!metalio_audio_service::startCapture(true)) {
    LOG_ERR(
        TAG,
        "Opus test: capture start failed");

    return false;
  }

  std::vector<uint8_t> packet;

  bool gotPacket = false;

  for (int i = 0;
       i < 40 && !gotPacket;
       ++i) {

    gotPacket =
        metalio_audio_service::popEncodedPacket(
            packet);

    if (!gotPacket) {
      delay(50);
    }
  }

  metalio_audio_service::stopCapture();

  if (!gotPacket || packet.empty()) {
    LOG_ERR(
        TAG,
        "Opus test: no encoded packet received");

    return false;
  }

  if (!metalio_audio_service::pushOpusPacket(
          packet.data(),
          packet.size(),
          true)) {

    LOG_ERR(
        TAG,
        "Opus test: packet enqueue failed");

    return false;
  }

  if (!metalio_audio_service::waitPlaybackDrained(
          3000)) {

    LOG_ERR(
        TAG,
        "Opus test: speaker playback failed");

    return false;
  }

  LOG_INF(
      TAG,
      "OPUS LOOPBACK PASS: %u bytes",
      static_cast<unsigned>(packet.size()));

  return true;
}


bool MetalioAudioTestActivity::runOggPsrTest() {

  /*
   * Find the first .ogg in /music.
   */
  std::vector<String> names =
      Storage.listFiles(
          kMusicDir,
          static_cast<int>(kMaxTracks));

  std::string selectedPath;

  for (const String& name : names) {

    std::string path =
        kMusicDir;

    path += "/";
    path += name.c_str();

    if (hasOggExtension(path)) {
      selectedPath =
          std::move(path);

      break;
    }
  }

  if (selectedPath.empty()) {

    LOG_ERR(
        TAG,
        "OGG: no .ogg file found in %s",
        kMusicDir);

    return false;
  }

  LOG_INF(
      TAG,
      "OGG: selected %s",
      selectedPath.c_str());

  /*
   * Open the OGG from SD.
   */
  HalFile file =
      Storage.open(
          selectedPath.c_str(),
          O_RDONLY);

  if (!file.isOpen()) {

    LOG_ERR(
        TAG,
        "OGG: cannot open %s",
        selectedPath.c_str());

    return false;
  }

  /*
   * Read the entire file into PSRAM.
   * OggPsrTestSource::open() closes the SD
   * file before returning.
   */
  OggPsrTestSource source;

  if (!source.open(std::move(file))) {

    LOG_ERR(
        TAG,
        "OGG: source open failed");

    return false;
  }

  /*
   * Make sure no PCM from an earlier test
   * remains in the playback queue.
   */
  metalio_audio_service::flushPlayback();

  MetalioPsramVector<int16_t> pcm;

  pcm.reserve(kDecodeFrames);

  std::size_t totalSamples = 0;

  /*
   * From here onward:
   *
   *   - OGG compressed data = PSRAM
   *   - decoder = PSRAM data
   *   - decoded PCM = PSRAM
   *   - SD file = CLOSED
   *
   * No SD read should happen in this loop.
   */
  while (!source.eof()) {

    pcm.clear();

    if (!source.readFrames(
            pcm,
            kDecodeFrames)) {
      break;
    }

    if (pcm.empty()) {
      continue;
    }

    bool queued =
        metalio_audio_service::queuePcm(
            pcm.data(),
            pcm.size());

    /*
     * queuePcm() can temporarily be full.
     * Retry without touching SD.
     */
    for (int retry = 0;
         retry < 125 && !queued;
         ++retry) {

      delay(8);

      queued =
          metalio_audio_service::queuePcm(
              pcm.data(),
              pcm.size());
    }

    if (!queued) {

      LOG_ERR(
          TAG,
          "OGG: PCM queue failed");

      source.close();

      metalio_audio_service::flushPlayback();

      return false;
    }

    totalSamples += pcm.size();
  }

  /*
   * Decoder and PSRAM OGG buffer can now be released.
   */
  source.close();

  if (totalSamples == 0) {

    LOG_ERR(
        TAG,
        "OGG: decoded zero samples");

    metalio_audio_service::flushPlayback();

    return false;
  }

  LOG_INF(
      TAG,
      "OGG: SD remains closed; waiting for playback drain, samples=%u",
      static_cast<unsigned>(totalSamples));

  const bool drained =
      metalio_audio_service::waitPlaybackDrained(
          kOggPlaybackTimeoutMs);

  LOG_INF(
      TAG,
      "OGG PSRAM %s: samples=%u",
      drained ? "PASS" : "FAIL",
      static_cast<unsigned>(totalSamples));

  return drained;
}
