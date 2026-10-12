#include "MetalioAudioTestActivity.h"

#include <Arduino.h>

#include <algorithm>

#include <Bitmap.h>
#include <HalDisplay.h>
#include <HalStorage.h>

#include "MetalioAudioService.h"
#include "fontIds.h"

namespace {

constexpr const char* TAG = "MUSIC";

constexpr const char* kSkinPath =
    "/metalio/e-ink/ipod/Ipod.bmp.bmp";

constexpr Rect kMenuHit{
    150,
    245,
    180,
    100,
};

constexpr Rect kPreviousHit{
    35,
    515,
    145,
    125,
};

constexpr Rect kNextHit{
    300,
    515,
    145,
    125,
};

constexpr Rect kPlayPauseHit{
    155,
    650,
    170,
    120,
};

constexpr Rect kCenterHit{
    155,
    430,
    170,
    170,
};

constexpr int kWheelCx = 240;
constexpr int kWheelCy = 600;
constexpr int kWheelOuterRx = 185;
constexpr int kWheelOuterRy = 180;
constexpr int kWheelCenterRadius = 78;

bool contains(
    const Rect& rect,
    const int x,
    const int y) {
  return
      x >= rect.x &&
      x < rect.x + rect.width &&
      y >= rect.y &&
      y < rect.y + rect.height;
}

bool inEllipse(
    const int x,
    const int y,
    const int cx,
    const int cy,
    const int rx,
    const int ry) {
  const int dx = x - cx;
  const int dy = y - cy;

  const int64_t lhs =
      static_cast<int64_t>(dx) * dx * ry * ry +
      static_cast<int64_t>(dy) * dy * rx * rx;

  const int64_t rhs =
      static_cast<int64_t>(rx) * rx * ry * ry;

  return lhs <= rhs;
}

}  // namespace

void MetalioAudioTestActivity::onEnter() {
  Activity::onEnter();

  renderer.setOrientation(
      GfxRenderer::Orientation::Portrait);

  volume_ =
      metalio_audio_service::outputVolume();

  playing_ = false;
  firstRender_ = true;

  skinLoaded_ = loadSkin();

  requestRedraw();
}

void MetalioAudioTestActivity::onExit() {
  if (skinBitmap_ != nullptr) {
    delete skinBitmap_;
    skinBitmap_ = nullptr;
  }

  skinFile_.close();
  skinLoaded_ = false;

  Activity::onExit();
}

bool MetalioAudioTestActivity::loadSkin() {
  if (skinBitmap_ != nullptr) {
    delete skinBitmap_;
    skinBitmap_ = nullptr;
  }

  skinFile_.close();
  skinLoaded_ = false;

  if (!Storage.openFileForRead(
          "MUSIC",
          kSkinPath,
          skinFile_)) {
    LOG_ERR(
        TAG,
        "Cannot open iPod skin: %s",
        kSkinPath);

    return false;
  }

  skinBitmap_ =
      new Bitmap(
          skinFile_,
          false,
          false);

  if (skinBitmap_ == nullptr) {
    LOG_ERR(
        TAG,
        "OOM creating iPod Bitmap");

    skinFile_.close();
    return false;
  }

  const BmpReaderError error =
      skinBitmap_->parseHeaders();

  if (error != BmpReaderError::Ok) {
    LOG_ERR(
        TAG,
        "Invalid iPod BMP: %s",
        Bitmap::errorToString(error));

    delete skinBitmap_;
    skinBitmap_ = nullptr;

    skinFile_.close();

    return false;
  }

  if (skinBitmap_->getBpp() != 4) {
    LOG_ERR(
        TAG,
        "Expected 4-bit iPod BMP, got %u-bit",
        static_cast<unsigned>(
            skinBitmap_->getBpp()));

    delete skinBitmap_;
    skinBitmap_ = nullptr;

    skinFile_.close();

    return false;
  }

  LOG_INF(
      TAG,
      "iPod skin loaded: %dx%d %u-bit",
      skinBitmap_->getWidth(),
      skinBitmap_->getHeight(),
      static_cast<unsigned>(
          skinBitmap_->getBpp()));

  skinLoaded_ = true;

  return true;
}

void MetalioAudioTestActivity::requestRedraw() {
  requestUpdate();
}

MetalioAudioTestActivity::HitTarget
MetalioAudioTestActivity::hitTarget(
    const int x,
    const int y) {

  if (contains(kMenuHit, x, y)) {
    return HitTarget::Menu;
  }

  if (contains(kPreviousHit, x, y)) {
    return HitTarget::Previous;
  }

  if (contains(kNextHit, x, y)) {
    return HitTarget::Next;
  }

  if (contains(kPlayPauseHit, x, y)) {
    return HitTarget::PlayPause;
  }

  if (contains(kCenterHit, x, y)) {
    return HitTarget::Center;
  }

  if (!inEllipse(
          x,
          y,
          kWheelCx,
          kWheelCy,
          kWheelOuterRx,
          kWheelOuterRy)) {
    return HitTarget::None;
  }

  const int dx = x - kWheelCx;
  const int dy = y - kWheelCy;

  if (
      dx * dx + dy * dy <=
      kWheelCenterRadius * kWheelCenterRadius) {
    return HitTarget::Center;
  }

  if (std::abs(dy) >= std::abs(dx)) {
    return
        dy < 0
            ? HitTarget::VolumeUp
            : HitTarget::VolumeDown;
  }

  return
      dx < 0
          ? HitTarget::Previous
          : HitTarget::Next;
}

void MetalioAudioTestActivity::adjustVolume(
    const int delta) {

  const int next =
      std::clamp(
          static_cast<int>(volume_) + delta,
          0,
          100);

  volume_ =
      static_cast<uint8_t>(next);

  metalio_audio_service::setOutputVolume(
      volume_);

  LOG_INF(
      TAG,
      "Volume %u%%",
      static_cast<unsigned>(volume_));

  requestRedraw();
}

void MetalioAudioTestActivity::togglePlayPause() {
  playing_ = !playing_;

  LOG_INF(
      TAG,
      "Playback state: %s",
      playing_ ? "PLAY" : "PAUSE");

  requestRedraw();
}

void MetalioAudioTestActivity::loop() {
  int x = 0;
  int y = 0;

  if (mappedInput.wasScreenTapped(x, y)) {
    switch (hitTarget(x, y)) {
      case HitTarget::Menu:
        finish();
        return;

      case HitTarget::Previous:
        LOG_INF(TAG, "Previous track");
        requestRedraw();
        return;

      case HitTarget::Next:
        LOG_INF(TAG, "Next track");
        requestRedraw();
        return;

      case HitTarget::PlayPause:
      case HitTarget::Center:
        togglePlayPause();
        return;

      case HitTarget::VolumeUp:
        adjustVolume(+5);
        return;

      case HitTarget::VolumeDown:
        adjustVolume(-5);
        return;

      case HitTarget::None:
        break;
    }
  }

  if (mappedInput.wasReleased(
          MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasReleased(
          MappedInputManager::Button::Up)) {
    adjustVolume(+5);
    return;
  }

  if (mappedInput.wasReleased(
          MappedInputManager::Button::Down)) {
    adjustVolume(-5);
    return;
  }

  if (mappedInput.wasReleased(
          MappedInputManager::Button::Left)) {
    LOG_INF(TAG, "Previous track");
    requestRedraw();
    return;
  }

  if (mappedInput.wasReleased(
          MappedInputManager::Button::Right)) {
    LOG_INF(TAG, "Next track");
    requestRedraw();
    return;
  }

  if (mappedInput.wasReleased(
          MappedInputManager::Button::Confirm)) {
    togglePlayPause();
    return;
  }
}

void MetalioAudioTestActivity::drawSkin() {
  if (!skinLoaded_ ||
      skinBitmap_ == nullptr) {
    renderer.clearScreen();

    renderer.drawText(
        UI_12_FONT_ID,
        30,
        50,
        "iPod skin not found");

    renderer.drawText(
        UI_10_FONT_ID,
        30,
        80,
        kSkinPath);

    return;
  }

    renderer.clearScreen();

  if (
      skinBitmap_->rewindToData() !=
      BmpReaderError::Ok) {
    LOG_ERR(
        TAG,
        "Failed to rewind iPod BMP");

    renderer.clearScreen();

    renderer.drawText(
        UI_12_FONT_ID,
        30,
        50,
        "iPod skin rewind failed");

    return;
  }

  if (!renderer.drawBitmap(
          *skinBitmap_,
          0,
          0,
          renderer.getScreenWidth(),
          renderer.getScreenHeight(),
          0,
          0)) {
    LOG_ERR(
        TAG,
        "Failed to render iPod skin");

    renderer.clearScreen();

    renderer.drawText(
        UI_12_FONT_ID,
        30,
        50,
        "iPod skin render failed");
  }
}

void MetalioAudioTestActivity::render(
    RenderLock&&) {

  renderer.setOrientation(
      GfxRenderer::Orientation::Portrait);

  drawSkin();

  constexpr int kDisplayX = 92;
  constexpr int kDisplayY = 93;
  constexpr int kDisplayWidth = 296;

  renderer.drawText(
      UI_10_FONT_ID,
      kDisplayX,
      kDisplayY,
      "MUSIC");

  renderer.drawText(
      UI_12_FONT_ID,
      kDisplayX,
      kDisplayY + 32,
      title_);

  renderer.drawText(
      UI_10_FONT_ID,
      kDisplayX,
      kDisplayY + 58,
      artist_);

  const char* state =
      playing_
          ? "PLAY"
          : "PAUSE";

  const int stateWidth =
      renderer.getTextWidth(
          UI_10_FONT_ID,
          state);

  renderer.drawText(
      UI_10_FONT_ID,
      kDisplayX +
          kDisplayWidth -
          stateWidth,
      kDisplayY,
      state);

  constexpr int kProgressY =
      kDisplayY + 82;

  renderer.drawRect(
      kDisplayX,
      kProgressY,
      kDisplayWidth,
      2,
      true);

  if (playing_) {
    renderer.fillRect(
        kDisplayX,
        kProgressY,
        kDisplayWidth / 3,
        2,
        true);
  }

  renderer.drawText(
      UI_10_FONT_ID,
      kDisplayX,
      kProgressY + 22,
      "00:00");

  constexpr const char* kDuration =
      "00:00";

  const int durationWidth =
      renderer.getTextWidth(
          UI_10_FONT_ID,
          kDuration);

  renderer.drawText(
      UI_10_FONT_ID,
      kDisplayX +
          kDisplayWidth -
          durationWidth,
      kProgressY + 22,
      kDuration);

  /*
   * The volume is intentionally controlled by
   * the click-wheel / hardware buttons only.
   * No volume popup or +/- overlay is drawn.
   */

  renderer.displayBuffer(
      firstRender_
          ? HalDisplay::HALF_REFRESH
          : HalDisplay::FAST_REFRESH);

  firstRender_ = false;
}
