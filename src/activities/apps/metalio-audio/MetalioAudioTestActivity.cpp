#include "MetalioAudioTestActivity.h"

#include <Arduino.h>

#include <algorithm>
#include <cstdio>

#include <Bitmap.h>
#include <FsHelpers.h>

#include "MetalioAudioService.h"
#include "fontIds.h"

namespace {

constexpr const char* TAG = "MUSIC";

/*
 * The supplied iPod skin is a real 4-bit grayscale BMP.
 *
 * It is deliberately kept as an external SD asset rather than converted
 * into a huge C array.
 */
constexpr const char* kSkinPath =
    "/sdcard/metalio/e-ink/ipod/Ipod.bmp.bmp";

/*
 * iPod-style invisible touch zones.
 *
 * The complete iPod body is rendered from the BMP.
 * Nothing is drawn over these controls.
 */

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

/*
 * Approximate click-wheel geometry.
 *
 * These are hitboxes only. The wheel itself comes from the supplied image.
 */
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

  loadSkin();

  requestRedraw();
}

void MetalioAudioTestActivity::onExit() {
  Activity::onExit();
}

bool MetalioAudioTestActivity::loadSkin() {
  /*
   * Skin loading is intentionally deferred until the first render.
   *
   * Bitmap owns the SD file handle internally.
   *
   * The renderer already supports BMP rendering and 4-bit grayscale.
   */

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

  /*
   * MENU
   */
  if (contains(kMenuHit, x, y)) {
    return HitTarget::Menu;
  }

  /*
   * Previous / Next
   */
  if (contains(kPreviousHit, x, y)) {
    return HitTarget::Previous;
  }

  if (contains(kNextHit, x, y)) {
    return HitTarget::Next;
  }

  /*
   * Play / Pause
   */
  if (contains(kPlayPauseHit, x, y)) {
    return HitTarget::PlayPause;
  }

  /*
   * Center button.
   */
  if (contains(kCenterHit, x, y)) {
    return HitTarget::Center;
  }

  /*
   * Click wheel.
   */
  if (!inEllipse(
          x,
          y,
          kWheelCx,
          kWheelCy,
          kWheelOuterRx,
          kWheelOuterRy)) {

    return HitTarget::None;
  }

  /*
   * Center of wheel.
   */
  const int dx = x - kWheelCx;
  const int dy = y - kWheelCy;

  if (
      dx * dx + dy * dy <=
      kWheelCenterRadius * kWheelCenterRadius) {

    return HitTarget::Center;
  }

  /*
   * Outer wheel sectors.

       UP
        |
   LEFT-+-RIGHT
        |
      DOWN
  */

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

  std::snprintf(
      volumeText_,
      sizeof(volumeText_),
      "Volume %u%%",
      static_cast<unsigned>(volume_));

  volumeOverlayUntil_ =
      millis() + 1400;

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

  /*
   * Touch control.
   */
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

  /*
   * Physical / logical controls.
   */

  if (
      mappedInput.wasReleased(
          MappedInputManager::Button::Back)) {

    finish();
    return;
  }

  if (
      mappedInput.wasReleased(
          MappedInputManager::Button::Up)) {

    adjustVolume(+5);
    return;
  }

  if (
      mappedInput.wasReleased(
          MappedInputManager::Button::Down)) {

    adjustVolume(-5);
    return;
  }

  if (
      mappedInput.wasReleased(
          MappedInputManager::Button::Left)) {

    LOG_INF(TAG, "Previous track");
    requestRedraw();
    return;
  }

  if (
      mappedInput.wasReleased(
          MappedInputManager::Button::Right)) {

    LOG_INF(TAG, "Next track");
    requestRedraw();
    return;
  }

  if (
      mappedInput.wasReleased(
          MappedInputManager::Button::Confirm)) {

    togglePlayPause();
    return;
  }

  /*
   * Expire temporary volume overlay.
   */
  if (
      volumeOverlayUntil_ != 0 &&
      millis() >= volumeOverlayUntil_) {

    volumeOverlayUntil_ = 0;
    requestRedraw();
  }
}

void MetalioAudioTestActivity::drawSkin() {

  /*
   * Open the BMP from SD and let the existing CrossMux Bitmap
   * implementation handle the 4-bit grayscale conversion.
   *
   * The renderer has native:
   *
   *   drawBitmap(...)
   *   drawBitmapCropToFill(...)
   *
   * so there is no reason to convert the supplied image into
   * another representation.
   */

  auto file = FsHelpers::openFile(
      kSkinPath,
      "rb");

  if (!file) {
    LOG_ERR(
        TAG,
        "Cannot open iPod skin: %s",
        kSkinPath);

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

  Bitmap bitmap(
      *file,
      false,
      false);

  const BmpReaderError error =
      bitmap.parseHeaders();

  if (error != BmpReaderError::Ok) {

    LOG_ERR(
        TAG,
        "Invalid iPod BMP: %s",
        Bitmap::errorToString(error));

    renderer.clearScreen();

    renderer.drawText(
        UI_12_FONT_ID,
        30,
        50,
        "Invalid iPod BMP");

    return;
  }

  /*
   * Fit the complete supplied iPod image into
   * the logical 480x800 screen.
   *
   * If the supplied BMP is already 480x800,
   * this becomes a 1:1 render.
   */
  renderer.drawBitmapCropToFill(
      bitmap,
      0,
      0,
      renderer.getScreenWidth(),
      renderer.getScreenHeight());
}

void MetalioAudioTestActivity::render(
    RenderLock&&) {

  renderer.setOrientation(
      GfxRenderer::Orientation::Portrait);

  /*
   * 1. Draw the complete iPod body.
   */
  drawSkin();

  /*
   * 2. Dynamic LCD content.
   *
   * This is deliberately drawn only inside the
   * LCD area of the supplied skin.
   *
   * These coordinates are the initial reference
   * positions and will be tuned against the actual
   * BMP once displayed on hardware.
   */

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

  /*
   * Playback state.
   */
  const char* state =
      playing_
          ? "▶"
          : "Ⅱ";

  renderer.drawText(
      UI_10_FONT_ID,
      kDisplayX + kDisplayWidth -
          renderer.getTextWidth(
              UI_10_FONT_ID,
              state),
      kDisplayY,
      state);

  /*
   * Progress bar.
   *
   * Real track duration/position will be connected
   * when the decoder/player layer is added.
   */
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

  const char* duration =
      "00:00";

  renderer.drawText(
      UI_10_FONT_ID,
      kDisplayX +
          kDisplayWidth -
          renderer.getTextWidth(
              UI_10_FONT_ID,
              duration),
      kProgressY + 22,
      duration);

  /*
   * Temporary volume overlay.
   *
   * It appears only when volume changes,
   * so the iPod body remains visually clean.
   */
  if (
      volumeOverlayUntil_ != 0 &&
      millis() < volumeOverlayUntil_) {

    const int width =
        renderer.getTextWidth(
            UI_12_FONT_ID,
            volumeText_);

    const int x =
        (renderer.getScreenWidth() - width) / 2;

    renderer.fillRect(
        x - 8,
        180,
        width + 16,
        30,
        true);

    renderer.drawText(
        UI_12_FONT_ID,
        x,
        184,
        volumeText_,
        false);
  }

  /*
   * Full skin + dynamic screen.
   */
  renderer.displayBuffer(
      HalDisplay::FAST_REFRESH);
}
