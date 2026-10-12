#include "MetalioAudioTestActivity.h"

#if FREEINK_DEVICE_METALIO_EINK4

#include <Arduino.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include <Bitmap.h>
#include <HalDisplay.h>
#include <HalStorage.h>

#include "MetalioAudioService.h"
#include "MetalioMusicPlayer.h"
#include "fontIds.h"

namespace {

using metalio_music_player::MusicPlayer;
using metalio_music_player::PlayerState;
using metalio_music_player::TrackInfo;

constexpr const char* TAG = "MUSIC";
constexpr const char* kSkinPath =
    "/metalio/e-ink/ipod/Ipod.bmp.bmp";

constexpr Rect kMenuHit{150, 245, 180, 100};
constexpr Rect kPreviousHit{35, 515, 145, 125};
constexpr Rect kNextHit{300, 515, 145, 125};
constexpr Rect kPlayPauseHit{155, 650, 170, 120};
constexpr Rect kCenterHit{155, 430, 170, 170};

constexpr int kWheelCx = 240;
constexpr int kWheelCy = 600;
constexpr int kWheelOuterRx = 185;
constexpr int kWheelOuterRy = 180;
constexpr int kWheelCenterRadius = 78;

bool contains(const Rect& rect, int x, int y) {
  return x >= rect.x && x < rect.x + rect.width &&
         y >= rect.y && y < rect.y + rect.height;
}

bool inEllipse(int x, int y, int cx, int cy, int rx, int ry) {
  const int dx = x - cx;
  const int dy = y - cy;
  const int64_t lhs =
      static_cast<int64_t>(dx) * dx * ry * ry +
      static_cast<int64_t>(dy) * dy * rx * rx;
  const int64_t rhs =
      static_cast<int64_t>(rx) * rx * ry * ry;
  return lhs <= rhs;
}

std::string formatTime(uint32_t ms) {
  const uint32_t seconds = ms / 1000;
  char text[16];
  std::snprintf(text, sizeof(text), "%lu:%02lu",
                static_cast<unsigned long>(seconds / 60),
                static_cast<unsigned long>(seconds % 60));
  return text;
}

const char* stateText(PlayerState state) {
  switch (state) {
    case PlayerState::Loading: return "LOAD";
    case PlayerState::Playing: return "PLAY";
    case PlayerState::Paused: return "PAUSE";
    case PlayerState::Error: return "ERR";
    case PlayerState::Stopped:
    default: return "STOP";
  }
}

std::string trackTitle(const TrackInfo& track) {
  if (!track.title.empty()) return track.title;
  if (!track.path.empty()) return track.path;
  return "Select music";
}

std::string trackArtist(const TrackInfo& track) {
  if (!track.artist.empty()) return track.artist;
  return "SD / music";
}

MusicPlayer& player() {
  return MusicPlayer::instance();
}

}  // namespace

void MetalioAudioTestActivity::onEnter() {
  Activity::onEnter();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  firstRender_ = true;
  player().begin();
  player().refreshLibrary();
  volume_ = player().volume();
  skinLoaded_ = loadSkin();

  LOG_INF(TAG, "Music library: %u tracks in /music",
          static_cast<unsigned>(player().trackCount()));

  requestRedraw();
}

void MetalioAudioTestActivity::onExit() {
  player().shutdown();

  if (skinBitmap_ != nullptr) {
    delete skinBitmap_;
    skinBitmap_ = nullptr;
  }
  skinFile_.close();
  skinLoaded_ = false;

  Activity::onExit();
}

void MetalioAudioTestActivity::requestRedraw() {
  requestUpdate();
}

bool MetalioAudioTestActivity::loadSkin() {
  if (skinBitmap_ != nullptr) {
    delete skinBitmap_;
    skinBitmap_ = nullptr;
  }
  skinFile_.close();
  skinLoaded_ = false;

  if (!Storage.openFileForRead("MUSIC", kSkinPath, skinFile_)) {
    LOG_ERR(TAG, "Cannot open iPod skin: %s", kSkinPath);
    return false;
  }

  skinBitmap_ = new Bitmap(skinFile_, false, false);
  if (skinBitmap_ == nullptr) {
    LOG_ERR(TAG, "OOM creating iPod Bitmap");
    skinFile_.close();
    return false;
  }

  const BmpReaderError error = skinBitmap_->parseHeaders();
  if (error != BmpReaderError::Ok) {
    LOG_ERR(TAG, "Invalid iPod BMP: %s", Bitmap::errorToString(error));
    delete skinBitmap_;
    skinBitmap_ = nullptr;
    skinFile_.close();
    return false;
  }

  if (skinBitmap_->getBpp() != 4) {
    LOG_ERR(TAG, "Expected 4-bit iPod BMP, got %u-bit",
            static_cast<unsigned>(skinBitmap_->getBpp()));
    delete skinBitmap_;
    skinBitmap_ = nullptr;
    skinFile_.close();
    return false;
  }

  LOG_INF(TAG, "iPod skin loaded: %dx%d %u-bit",
          skinBitmap_->getWidth(), skinBitmap_->getHeight(),
          static_cast<unsigned>(skinBitmap_->getBpp()));
  skinLoaded_ = true;
  return true;
}

MetalioAudioTestActivity::HitTarget
MetalioAudioTestActivity::hitTarget(int x, int y) {
  if (contains(kMenuHit, x, y)) return HitTarget::Menu;
  if (contains(kPreviousHit, x, y)) return HitTarget::Previous;
  if (contains(kNextHit, x, y)) return HitTarget::Next;
  if (contains(kPlayPauseHit, x, y)) return HitTarget::PlayPause;
  if (contains(kCenterHit, x, y)) return HitTarget::Center;

  if (!inEllipse(x, y, kWheelCx, kWheelCy,
                 kWheelOuterRx, kWheelOuterRy)) {
    return HitTarget::None;
  }

  const int dx = x - kWheelCx;
  const int dy = y - kWheelCy;
  if (dx * dx + dy * dy <=
      kWheelCenterRadius * kWheelCenterRadius) {
    return HitTarget::Center;
  }

  if (std::abs(dy) >= std::abs(dx)) {
    return dy < 0 ? HitTarget::VolumeUp : HitTarget::VolumeDown;
  }
  return dx < 0 ? HitTarget::Previous : HitTarget::Next;
}

void MetalioAudioTestActivity::adjustVolume(int delta) {
  if (delta > 0) {
    player().volumeUp(static_cast<uint8_t>(delta));
  } else if (delta < 0) {
    player().volumeDown(static_cast<uint8_t>(-delta));
  }
  volume_ = player().volume();
  LOG_INF(TAG, "Volume %u%%", static_cast<unsigned>(volume_));
  requestRedraw();
}

void MetalioAudioTestActivity::togglePlayPause() {
  if (player().trackCount() == 0) {
    LOG_ERR(TAG, "No audio files in /music");
    return;
  }
  player().togglePause();
  requestRedraw();
}

void MetalioAudioTestActivity::previousTrack() {
  if (player().trackCount() == 0) return;
  player().previousTrack();
  requestRedraw();
}

void MetalioAudioTestActivity::nextTrack() {
  if (player().trackCount() == 0) return;
  player().nextTrack();
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
        previousTrack();
        return;
      case HitTarget::Next:
        nextTrack();
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

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    adjustVolume(+5);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    adjustVolume(-5);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    previousTrack();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    nextTrack();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    togglePlayPause();
    return;
  }
}

void MetalioAudioTestActivity::drawSkin() {
  if (!skinLoaded_ || skinBitmap_ == nullptr) {
    renderer.clearScreen();
    renderer.drawText(UI_12_FONT_ID, 30, 50, "iPod skin not found");
    renderer.drawText(UI_10_FONT_ID, 30, 80, kSkinPath);
    return;
  }

  renderer.clearScreen();

  if (skinBitmap_->rewindToData() != BmpReaderError::Ok) {
    LOG_ERR(TAG, "Failed to rewind iPod BMP");
    renderer.clearScreen();
    renderer.drawText(UI_12_FONT_ID, 30, 50, "iPod skin rewind failed");
    return;
  }

  if (!renderer.drawBitmap(*skinBitmap_, 0, 0,
                           renderer.getScreenWidth(),
                           renderer.getScreenHeight(), 0, 0)) {
    LOG_ERR(TAG, "Failed to render iPod skin");
    renderer.clearScreen();
    renderer.drawText(UI_12_FONT_ID, 30, 50, "iPod skin render failed");
  }
}

void MetalioAudioTestActivity::render(RenderLock&&) {
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  drawSkin();

  constexpr int kDisplayX = 92;
  constexpr int kDisplayY = 93;
  constexpr int kDisplayWidth = 296;

  const TrackInfo track = player().currentTrack();
  const std::string title = trackTitle(track);
  const std::string artist = trackArtist(track);
  const PlayerState state = player().state();

  renderer.drawText(UI_10_FONT_ID, kDisplayX, kDisplayY, "MUSIC");
  renderer.drawText(UI_12_FONT_ID, kDisplayX, kDisplayY + 32,
                    title.c_str());
  renderer.drawText(UI_10_FONT_ID, kDisplayX, kDisplayY + 58,
                    artist.c_str());

  const char* stateLabel = stateText(state);
  const int stateWidth = renderer.getTextWidth(UI_10_FONT_ID, stateLabel);
  renderer.drawText(UI_10_FONT_ID,
                    kDisplayX + kDisplayWidth - stateWidth,
                    kDisplayY, stateLabel);

  constexpr int kProgressY = kDisplayY + 82;
  const uint32_t elapsed = player().elapsedMs();
  const uint32_t duration = player().durationMs();

  renderer.drawRect(kDisplayX, kProgressY, kDisplayWidth, 2, true);
  if (duration > 0) {
    const int filled = static_cast<int>(
        (static_cast<uint64_t>(elapsed) * kDisplayWidth) / duration);
    if (filled > 0) {
      renderer.fillRect(kDisplayX, kProgressY,
                        std::min(filled, kDisplayWidth), 2, true);
    }
  }

  const std::string elapsedText = formatTime(elapsed);
  const std::string durationText =
      duration > 0 ? formatTime(duration) : "--:--";
  renderer.drawText(UI_10_FONT_ID, kDisplayX, kProgressY + 22,
                    elapsedText.c_str());

  const int durationWidth =
      renderer.getTextWidth(UI_10_FONT_ID, durationText.c_str());
  renderer.drawText(UI_10_FONT_ID,
                    kDisplayX + kDisplayWidth - durationWidth,
                    kProgressY + 22,
                    durationText.c_str());

  renderer.displayBuffer(firstRender_
                             ? HalDisplay::HALF_REFRESH
                             : HalDisplay::FAST_REFRESH);
  firstRender_ = false;
}

#else

void MetalioAudioTestActivity::onEnter() {}
void MetalioAudioTestActivity::onExit() {}
void MetalioAudioTestActivity::loop() {}
void MetalioAudioTestActivity::render(RenderLock&&) {}

#endif
