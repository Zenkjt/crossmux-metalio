#pragma once

#include "activities/Activity.h"

class MetalioAudioTestActivity final : public Activity {
 public:
  MetalioAudioTestActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("MetalioAudioTest", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
