#pragma once

#include <array>
#include <cstdint>

#include "activities/UiListActivity.h"

class MetalioAudioTestActivity final : public UiListActivity {
 public:
  MetalioAudioTestActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("MetalioAudioTest", renderer, mappedInput) {}
  ~MetalioAudioTestActivity() override = default;

  void onEnter() override;
  void onExit() override;

 protected:
  int listCount() const override { return kActionCount; }
  const char* headerTitle() const override { return "Metalio Audio Test"; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override { finish(); }

 private:
  static constexpr int kSpeakerTest = 0;
  static constexpr int kMicTest = 1;
  static constexpr int kOpusLoopbackTest = 2;
  static constexpr int kStatusRow = 3;
  static constexpr int kActionCount = 4;

  void rebuildRows();
  void setStatus(const char* text);
  bool runMicTest();
  bool runOpusLoopbackTest();

  std::array<freeink::ui::ListItem, kActionCount> rows{};
  char statusText[96] = "Ready";
};
