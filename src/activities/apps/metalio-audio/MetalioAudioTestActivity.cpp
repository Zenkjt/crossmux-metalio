#include "MetalioAudioTestActivity.h"

#include <Logging.h>

#include <algorithm>
#include <vector>

#include "metalio/MetalioAudioService.h"

namespace fui = freeink::ui;

void MetalioAudioTestActivity::onEnter() {
  UiListActivity::onEnter();

  std::fill(std::begin(statusText), std::end(statusText), '\0');
  setStatus("Starting audio service...");

  if (!metalio_audio_service::start()) {
    setStatus("Audio service start FAILED");
    LOG_ERR("MTAUD", "Metalio audio service start failed");
    rebuildRows();
    requestUpdate();
    return;
  }

  setStatus("Ready");
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

  int16_t peak = 0;
  for (const int16_t sample : samples) {
    peak = std::max<int16_t>(peak, sample < 0 ? static_cast<int16_t>(-sample) : sample);
  }

  LOG_INF("MTAUD", "MIC OK: %u samples, peak=%d",
          static_cast<unsigned>(samples.size()), static_cast<int>(peak));
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
  if (!metalio_audio_service::decodeOpusPacket(packet.data(), packet.size(), decoded) || decoded.empty()) {
    LOG_ERR("MTAUD", "Opus test: decode failed");
    return false;
  }

  if (!metalio_audio_service::playPcm(decoded.data(), decoded.size(), 3000)) {
    LOG_ERR("MTAUD", "Opus test: speaker playback failed");
    return false;
  }

  LOG_INF("MTAUD", "OPUS OK: %u bytes -> %u samples",
          static_cast<unsigned>(packet.size()), static_cast<unsigned>(decoded.size()));
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
