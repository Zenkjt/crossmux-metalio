#include "MetalioAudioTestActivity.h"

#include <Arduino.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "MetalioAudioFilePlayer.h"
#include "MetalioAudioService.h"

namespace fui = freeink::ui;

namespace {

constexpr const char* TAG = "MTAUD";
constexpr uint32_t kSampleRate = 16000;
constexpr char kWavPath[] = "/music/test0.wav";

metalio_audio_file_player::Player gWavPlayer;

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
  gWavPlayer.stop();
  metalio_audio_service::stop();
  UiListActivity::onExit();
}

void MetalioAudioTestActivity::rebuildRows() {
  rows[kSpeakerTest] = fui::ListItem{};
  rows[kSpeakerTest].label = "WAV streaming test";
  rows[kSpeakerTest].actionValue = kSpeakerTest;

  rows[kMicTest] = fui::ListItem{};
  rows[kMicTest].label = "Microphone test";
  rows[kMicTest].actionValue = kMicTest;

  rows[kOpusLoopbackTest] = fui::ListItem{};
  rows[kOpusLoopbackTest].label = "Opus loopback";
  rows[kOpusLoopbackTest].actionValue = kOpusLoopbackTest;

  rows[kStatusRow] = fui::ListItem{};
  rows[kStatusRow].label = statusText;
  rows[kStatusRow].actionValue = kStatusRow;
}

void MetalioAudioTestActivity::setStatus(const char* text) {
  if (text == nullptr) {
    statusText[0] = '\0';
  } else {
    std::strncpy(statusText, text, sizeof(statusText) - 1);
    statusText[sizeof(statusText) - 1] = '\0';
  }

  rebuildRows();
  requestUpdate();
}

void MetalioAudioTestActivity::buildScreen(UiScreen& screen) {
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  const Rect content = pageContentRect();

  screen.setContentMarginFromScreen(
      fui::Insets{
          static_cast<int16_t>(content.y),
          static_cast<int16_t>(
              screenWidth - content.x - content.width),
          static_cast<int16_t>(
              screenHeight - content.y - content.height),
          static_cast<int16_t>(content.x)});

  fui::ListProps props;

  props.items = rows.data();
  props.count = static_cast<uint16_t>(rows.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;

  syncListViewport(screen, props);
  screen.list(props);
}

void MetalioAudioTestActivity::activateIndex(int index) {
  if (index == kStatusRow) {
    return;
  }

  app.clearTapFlash();

  switch (index) {
    case kSpeakerTest: {
      setStatus("Streaming /music/test0.wav...");

      const bool started = gWavPlayer.playWav(kWavPath);
      if (!started) {
        LOG_ERR(
            TAG,
            "WAV start failed: %s",
            gWavPlayer.lastError());
        setStatus("WAV stream START FAIL");
        break;
      }

      const bool ok = gWavPlayer.wait(120000);

      if (ok) {
        setStatus("WAV stream PASS");
      } else {
        LOG_ERR(
            TAG,
            "WAV stream failed: %s",
            gWavPlayer.lastError());
        setStatus("WAV stream FAIL");
      }

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

    default:
      break;
  }
}

bool MetalioAudioTestActivity::runWavStreamTest() {
  if (!gWavPlayer.playWav(kWavPath)) {
    LOG_ERR(
        TAG,
        "WAV stream start failed: %s",
        gWavPlayer.lastError());
    return false;
  }

  return gWavPlayer.wait(120000);
}

bool MetalioAudioTestActivity::runMicTest() {
  if (!metalio_audio_service::startCapture(false)) {
    LOG_ERR(TAG, "Mic test: capture start failed");
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
    LOG_ERR(TAG, "Mic test: no PCM captured");
    return false;
  }

  int peak = 0;
  double sumSquares = 0.0;

  for (const int16_t sample : samples) {
    const int value = std::abs(static_cast<int>(sample));
    peak = std::max(peak, value);

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
    LOG_ERR(TAG, "Opus test: capture start failed");
    return false;
  }

  std::vector<uint8_t> packet;
  bool gotPacket = false;

  for (int i = 0; i < 40 && !gotPacket; ++i) {
    gotPacket =
        metalio_audio_service::popEncodedPacket(packet);

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

  if (!metalio_audio_service::waitPlaybackDrained(3000)) {
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
