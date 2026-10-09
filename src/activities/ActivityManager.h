#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cassert>
#include <memory>
#include <string>
#include <vector>

#include "GfxRenderer.h"
#include "Logging.h"
#include "MappedInputManager.h"
#include "Memory.h"
#include "activities/MainTab.h"
#include "util/ScreenshotInfo.h"

class Activity;
class RenderLock;

enum class HomeMenuItem { NONE, FILE_BROWSER, RECENTS, LIBRARY, OPDS_BROWSER, FILE_TRANSFER, SETTINGS_MENU, APPS };

class ActivityManager {
  friend class RenderLock;

 protected:
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;
  std::vector<std::unique_ptr<Activity>> stackActivities;
  std::unique_ptr<Activity> currentActivity;
  MainTabFocus mainTabFocus = MainTabFocus::Tabs;
  bool mainTabEntryReleasePending = false;

 private:
  enum class StandbyBackState : uint8_t { Idle, Pressed, WaitingForRelease };
  StandbyBackState standbyBackState = StandbyBackState::Idle;
  bool handleHomeStandbyInput();
  void resetHomeStandbyInput();

 protected:
  void exitActivity(const RenderLock& lock);
  bool handleMainTabInput();

  std::unique_ptr<Activity> pendingActivity;
  enum class PendingAction { None, Push, Pop, Replace };
  std::atomic<PendingAction> pendingAction{PendingAction::None};

  TaskHandle_t renderTaskHandle = nullptr;
  static void renderTaskTrampoline(void* param);
  [[noreturn]] virtual void renderTaskLoop();

  TaskHandle_t waitingTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  std::atomic<bool> requestedUpdate{false};
  std::atomic<uint32_t> idleRenderGeneration{0};

 public:
  explicit ActivityManager(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : renderer(renderer), mappedInput(mappedInput), renderingMutex(xSemaphoreCreateMutex()) {
    assert(renderingMutex != nullptr && "Failed to create rendering mutex");
    stackActivities.reserve(10);
  }
  ~ActivityManager() { assert(false); };

  void begin();
  void loop();
  void cancelIdleRender() { idleRenderGeneration.fetch_add(1, std::memory_order_relaxed); }
  bool idleRenderCancelled(uint32_t generation) const {
    return generation != idleRenderGeneration.load(std::memory_order_relaxed) || isSwitchPending() ||
           requestedUpdate.load();
  }

  void replaceActivity(std::unique_ptr<Activity>&& newActivity);

  template <typename T, typename... Args>
  bool replaceActivityWith(Args&&... args) {
    auto activity = makeUniqueNoThrow<T>(renderer, mappedInput, std::forward<Args>(args)...);
    if (!activity) {
      LOG_ERR("ACT", "OOM: activity (%u bytes)", static_cast<unsigned>(sizeof(T)));
      return false;
    }
    replaceActivity(std::move(activity));
    return true;
  }

  void goToFileTransfer();
  void goToJoinNetwork();
  void goToUsbDrive();
  void goToSettings();
  void goToUglyAvatar();
  void goToReadingStatsMenu();
  void goToReadingStats();
  void goToInxRecent();
  void goToMainTab(MainTab tab);
  void goToFileBrowser(std::string path = {});
  void goToLibrary();
  void goToRecentBooks();
  void goToBrowser();
  void goToPlugins(bool showOpds);
  void goToReader(std::string path, bool allowFastInitialRefresh = false);
  void goToSleep(bool fromTimeout = false);
  void goToBoot();
  bool goToPostOtaBoot(bool allowAutoPreload);
  void goToFullScreenMessage(std::string message, EpdFontFamily::Style style = EpdFontFamily::REGULAR);
  void goToCrashReport();
  void goToApps();
  void goToSudoku();
  void goToSokoban();
  void goToGomoku();
  void goToMinesweeper();
  void goToPixelSwitch();
  void goToCalculator();
  void goToWoodfish();
  void goToAirPage();
  void goToBuddy();
  void goToStandby();
  void goToGame2048();
  void goToMetalioAudioTest();
#ifdef ENABLE_CHINESE_VERSION
  void goToChineseChess();
#endif
#ifdef ENABLE_CHINESE_VERSION
  void goToWeRead();
#endif
  void goHome(HomeMenuItem initialMenuItem = HomeMenuItem::NONE);
  MainTabFocus getMainTabFocus() const { return mainTabFocus; }

  void pushActivity(std::unique_ptr<Activity>&& activity);
  void popActivity();

  bool preventAutoSleep() const;
  bool requiresExclusiveStorageLoop() const;
  bool isReaderActivity() const;
  bool keepsBluetoothAlive() const;
  bool deferBluetoothStart() const;
  bool handleForcedRefresh();
  bool skipLoopDelay() const;
  ScreenshotInfo getScreenshotInfo() const;
  void prepareForSleep();

  bool isSwitchPending() const { return pendingAction.load() != PendingAction::None; }

  void requestUpdate(bool immediate = false);
  void requestUpdateAndWait();
};

extern ActivityManager activityManager;
