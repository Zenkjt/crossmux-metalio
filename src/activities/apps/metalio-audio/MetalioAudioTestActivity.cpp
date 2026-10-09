#include "MetalioAudioTestActivity.h"

#include <I18n.h>
#include <Logging.h>

#include "HalDisplay.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "metalio/MetalioSpeakerTest.h"

void MetalioAudioTestActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void MetalioAudioTestActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goToApps();
    return;
  }

  int touchX = 0;
  int touchY = 0;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect testButton{
      metrics.contentSidePadding,
      renderer.getScreenHeight() / 2,
      renderer.getScreenWidth() - 2 * metrics.contentSidePadding,
      metrics.menuRowHeight,
  };

  if (mappedInput.wasScreenTapped(touchX, touchY)) {
    if (touchX >= testButton.x && touchX < testButton.x + testButton.width &&
        touchY >= testButton.y && touchY < testButton.y + testButton.height) {
      GUI.drawPopup(renderer, "Testing speaker...");
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      metalio_speaker_test::runTest();
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    GUI.drawPopup(renderer, "Testing speaker...");
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    metalio_speaker_test::runTest();
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

  const int centerY = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, centerY,
                    "Local speaker hardware test");
  renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, centerY + renderer.getLineHeight(UI_12_FONT_ID) + 8,
                    "1 kHz tone, about 500 ms");

  const Rect button{
      metrics.contentSidePadding,
      height / 2,
      width - 2 * metrics.contentSidePadding,
      metrics.menuRowHeight,
  };
  renderer.drawRoundedRect(button.x, button.y, button.width, button.height, 2, 0, true);
  const char* label = tr(STR_SELECT);
  const int labelWidth = renderer.getTextWidth(UI_12_FONT_ID, label);
  const int labelHeight = renderer.getTextHeight(UI_12_FONT_ID);
  renderer.drawText(UI_12_FONT_ID,
                    button.x + (button.width - labelWidth) / 2,
                    button.y + (button.height - labelHeight) / 2,
                    label,
                    false);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
