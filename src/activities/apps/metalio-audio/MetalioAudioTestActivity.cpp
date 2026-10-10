#include "MetalioAudioTestActivity.h"

#include <I18n.h>
#include <Logging.h>

#include <string>

#include "HalDisplay.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "metalio/MetalioAudioService.h"
#include "metalio/MetalioOggOpus.h"

namespace {
constexpr char kMusicPath[] = "/sdcard/metalio/e-ink/music/music.ogg";
constexpr uint32_t kRecordingDurationMs = 10000;
}

void MetalioAudioTestActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void MetalioAudioTestActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goToApps();
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int top = metrics.topPadding + metrics.headerHeight +
                  metrics.verticalSpacing;
  const int rowH = metrics.menuRowHeight;

  int touchX = 0;
  int touchY = 0;
  if (mappedInput.wasScreenTapped(touchX, touchY)) {
    const int row = (touchY - top) / rowH;
    if (touchX >= metrics.contentSidePadding &&
        touchX < width - metrics.contentSidePadding &&
        row >= 0 && row < 3) {
      if (row == 0) {
        GUI.drawPopup(renderer, "Playing music.ogg...");
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
        (void)metalio_ogg_opus::playFile(kMusicPath);
      } else if (row == 1) {
        GUI.drawPopup(renderer, "Recording 10 seconds...");
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
        const std::string path = metalio_ogg_opus::nextRecordingPath();
        if (!path.empty()) {
          (void)metalio_ogg_opus::recordFile(path.c_str(),
                                             kRecordingDurationMs);
        }
      } else {
        GUI.drawPopup(renderer, "Testing speaker...");
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
        metalio_audio_service::playTestTone();
      }
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    GUI.drawPopup(renderer, "Testing speaker...");
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    metalio_audio_service::playTestTone();
    requestUpdate();
  }
}

void MetalioAudioTestActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer,
                 Rect{0, metrics.topPadding, width, metrics.headerHeight},
                 tr(STR_SOUND_FEEDBACK));

  const int top = metrics.topPadding + metrics.headerHeight +
                  metrics.verticalSpacing;
  const int rowH = metrics.menuRowHeight;

  renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, top - 4,
                    "Music: /metalio/e-ink/music/music.ogg");

  const Rect play{metrics.contentSidePadding, top + 16, 
                  width - 2 * metrics.contentSidePadding, rowH};
  const Rect record{metrics.contentSidePadding, top + 16 + rowH,
                    width - 2 * metrics.contentSidePadding, rowH};
  const Rect tone{metrics.contentSidePadding, top + 16 + 2 * rowH,
                  width - 2 * metrics.contentSidePadding, rowH};

  renderer.drawRoundedRect(play.x, play.y, play.width, play.height, 2, 0, true);
  renderer.drawRoundedRect(record.x, record.y, record.width, record.height, 2, 0, true);
  renderer.drawRoundedRect(tone.x, tone.y, tone.width, tone.height, 2, 0, true);

  renderer.drawText(UI_12_FONT_ID, play.x + 12,
                    play.y + (play.height - renderer.getLineHeight(UI_12_FONT_ID)) / 2,
                    "PLAY MUSIC.OGG", false);
  renderer.drawText(UI_12_FONT_ID, record.x + 12,
                    record.y + (record.height - renderer.getLineHeight(UI_12_FONT_ID)) / 2,
                    "RECORD 10 SEC", false);
  renderer.drawText(UI_12_FONT_ID, tone.x + 12,
                    tone.y + (tone.height - renderer.getLineHeight(UI_12_FONT_ID)) / 2,
                    "1 KHZ SPEAKER TEST", false);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
