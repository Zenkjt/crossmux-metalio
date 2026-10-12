#pragma once

#include <cstdint>

#include <HalStorage.h>

#include "activities/Activity.h"

class Bitmap;

class MetalioAudioTestActivity final : public Activity {
 public:
  MetalioAudioTestActivity(
      GfxRenderer& renderer,
      MappedInputManager& mappedInput)
      : Activity("MetalioMusic", renderer, mappedInput) {}

  ~MetalioAudioTestActivity() override = default;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class HitTarget {
    None,
    Menu,
    Previous,
    Next,
    PlayPause,
    Center,
    VolumeUp,
    VolumeDown,
  };

  static HitTarget hitTarget(int x, int y);

  void adjustVolume(int delta);
  void togglePlayPause();
  void requestRedraw();

  bool loadSkin();
  void drawSkin();

  bool skinLoaded_ = false;
  bool playing_ = false;

  uint8_t volume_ = 70;

  uint32_t volumeOverlayUntil_ = 0;
  char volumeText_[24] = {};

  HalFile skinFile_;
  Bitmap* skinBitmap_ = nullptr;

  const char* title_ = "Metalio Music";
  const char* artist_ = "Select music from /Music";
};
