#include "MetalioAudioTestActivity.h"

#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#include "components/UITheme.h"
#include "metalio/MetalioAudioService.h"

namespace fui = freeink::ui;

void MetalioAudioTestActivity::onEnter() {
  UiListActivity::onEnter();

  std::fill(std::begin(statusText), std::end(statusText), '\0');
  setStatus("Starting audio service...");

  if (!metalio_audio_service::start()) {
    setStatus("Audio service start FAILED");
    LOG_ERR("MTAUD", "Metalio audio service start failed");
  } else {
    setStatus("Ready");
  }

  rebuildRows();
  requestUpdate();
}

void MetalioAudioTestActivity::onExit() {
  metalio_audio_service::stop();
  UiListActivity::onExit();
}

void MetalioAudioTestActivity::setStatus(const char* text) {
  std::snprintf(statusText, sizeof(statusText), "%s", text ? text : "");
}

void MetalioAudioTestActivity::rebuildRows() {
  rows[kSpeakerTest] = fui::ListItem{};
  rows[kSpeakerTest].label = "Speaker: test tone";
  rows[kSpeakerTest].actionValue = kSpeakerTest;

  rows[kMicTest] = fui::ListItem{};
  rows[kMicTest].label = "Microphone: PCM capture";
  rows[kMicTest].actionValue = kMicTest;

  rows[kOpusLoopbackTest] = fui::ListItem{};
  rows[kOpusLoopbackTest].label = "Opus: mic -> opus -> speaker";
  rows[kOpusLoopbackTest].actionValue = kOpusLoopbackTest;

  rows[kStatusRow] = fui::ListItem{};
  rows[kStatusRow].label = statusText;
  rows[kStatusRow].actionValue = kStatusRow;
}

void MetalioAudioTestActivity::buildScreen(UiScreen& screen) {
  const int sw = renderer.getScreenWidth();
  const int sh = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  const int left = metrics.contentSidePadding;
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int right = metrics.contentSidePadding;
  const int bottom = metrics.bottomPadding;

  screen.setContentMarginFromScreen(
      fui::Insets{
          static_cast<int16_t>(top),
          static_cast<int16_t>(right),
          static_cast<int16_t>(std::max(0, sh - bottom - top)),
          static_cast<int16_t>(left)});

  fui::ListProps props;
  props.items = rows.data();
  props.count = static_cast<uint16_t>(kActionCount);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  syncListViewport(screen, props);
  screen.list(props);
}

bool MetalioAudioTestActivity::runMicTest() {
  std::vector<int16_t> samples;
  if (!metalio_audio_service::recordPcm(samples, 960, 2000)) {
    LOG_ERR("MTAUD", "Microphone PCM capture failed");
    return false;
  }

  if (samples.empty()) {
    LOG_ERR("MTAUD", "Microphone PCM capture returned no samples");
    return false;
  }

  int32_t peak = 0;
  for (const int16_t sample : samples) {
    const int32_t value = sample < 0 ? -static_cast<int32_t>(sample) : sample;
    peak = std::max(peak, value);
  }

  LOG_INF("MTAUD", "MIC OK: %u samples, peak=%ld",
          static_cast<unsigned>(samples.size()), static_cast<long>(peak));
  return true;
}

bool MetalioAudioTestActivity::runOpusLoopbackTest() {
  std::vector<int16_t> pcm;
  if (!metalio_audio_service::recordPcm(pcm, 960, 2000) || pcm.size() != 960) {
    LOG_ERR("MTAUD", "Opus test: PCM capture failed");
    return false;
  }

  std::vector<uint8_t> packet;
  if (!metalio_audio_service::encodePcmFrame(pcm.data(), pcm.size(), packet) || packet.empty()) {
    LOG_ERR("MTAUD", "Opus test: encode failed");
    return false;
  }

  std::vector<int16_t> decoded;
  if (!metalio_audio_service::decodeOpusPacket(packet.data(), packet.size(), decoded) ||
      decoded.empty()) {
    LOG_ERR("MTAUD", "Opus test: decode failed");
    return false;
  }

  if (!metalio_audio_service::playPcm(decoded.data(), decoded.size(), 3000)) {
    LOG_ERR("MTAUD", "Opus test: speaker playback failed");
    return false;
  }

  LOG_INF("MTAUD", "OPUS OK: %u bytes -> %u samples",
          static_cast<unsigned>(packet.size()),
          static_cast<unsigned>(decoded.size()));
  return true;
}

void MetalioAudioTestActivity::activateIndex(const int index) {
  if (index == kStatusRow) return;

  app.clearTapFlash();

  bool ok = false;

  if (index == kSpeakerTest) {
    setStatus("Playing test tone...");
    rebuildRows();
    requestUpdate();

    metalio_audio_service::playTestTone();

    setStatus("Speaker test: PASS");
    ok = true;
  } else if (index == kMicTest) {
    setStatus("Capturing microphone...");
    rebuildRows();
    requestUpdate();

    ok = runMicTest();
    setStatus(ok ? "Microphone test: PASS" : "Microphone test: FAIL");
  } else if (index == kOpusLoopbackTest) {
    setStatus("Running Opus loopback...");
    rebuildRows();
    requestUpdate();

    ok = runOpusLoopbackTest();
    setStatus(ok ? "Opus loopback: PASS" : "Opus loopback: FAIL");
  }

  rebuildRows();
  requestUpdate();
}
