#include "MetalioAudioTestActivity.h"

#include <Arduino.h>

#include "MetalioAudioService.h"

#include "ui/UiScreen.h"

namespace {
constexpr const char* TAG = "MTAUD";
}

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
  rows[0].label = "Speaker test";
  rows[1].label = "Microphone test";
  rows[2].label = "Opus loopback";
  rows[3].label = statusText;
}

void MetalioAudioTestActivity::setStatus(const char* text) {
  strncpy(statusText, text, sizeof(statusText) - 1);
  statusText[sizeof(statusText) - 1] = '\0';
  rebuildRows();
  requestRender();
}

void MetalioAudioTestActivity::buildScreen(UiScreen& screen) {
  const int sw = renderer.getScreenWidth();
  const int sh = renderer.getScreenHeight();
  const Rect content = appContentRect();

  screen.setContentMarginFromScreen(
      fui::Insets{
          static_cast<int16_t>(content.y),
          static_cast<int16_t>(sw - content.x - content.width),
          static_cast<int16_t>(sh - content.y - content.height),
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
  switch (index) {
    case kSpeakerTest:
      setStatus("Playing test tone...");
      metalio_audio_service::playTestTone();
      setStatus("Speaker test done");
      break;

    case kMicTest:
      setStatus("Testing microphone...");
      if (runMicTest()) {
        setStatus("Microphone OK");
      } else {
        setStatus("Microphone failed");
      }
      break;

    case kOpusLoopbackTest:
      setStatus("Running Opus loopback...");
      if (runOpusLoopbackTest()) {
        setStatus("Opus loopback OK");
      } else {
        setStatus("Opus loopback failed");
      }
      break;

    default:
      break;
  }
}

bool MetalioAudioTestActivity::runMicTest() {
  if (!metalio_audio_service::startCapture(false)) {
    LOG_ERR(TAG, "Mic test: capture start failed");
    return false;
  }

  std::vector<int16_t> samples;

  const bool ok = metalio_audio_service::readCapturedPcm(
      samples, 16000, 3000);

  metalio_audio_service::stopCapture();

  if (!ok || samples.empty()) {
    LOG_ERR(TAG, "Mic test: no PCM captured");
    return false;
  }

  LOG_INF(TAG, "MIC OK: %u samples",
          static_cast<unsigned>(samples.size()));

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
    gotPacket = metalio_audio_service::popEncodedPacket(packet);

    if (!gotPacket) {
      delay(50);
    }
  }

  metalio_audio_service::stopCapture();

  if (!gotPacket || packet.empty()) {
    LOG_ERR(TAG, "Opus test: no encoded packet received");
    return false;
  }

  if (!metalio_audio_service::pushOpusPacket(
          packet.data(), packet.size(), true)) {
    LOG_ERR(TAG, "Opus test: packet enqueue failed");
    return false;
  }

  if (!metalio_audio_service::waitPlaybackDrained(3000)) {
    LOG_ERR(TAG, "Opus test: speaker playback failed");
    return false;
  }

  LOG_INF(TAG, "OPUS LOOPBACK OK: %u bytes",
          static_cast<unsigned>(packet.size()));

  return true;
}
