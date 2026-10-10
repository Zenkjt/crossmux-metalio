#include "activities/ActivityManager.h"

#include "activities/apps/metalio-audio/MetalioAudioTestActivity.h"

void ActivityManager::goToMetalioAudioTest() {
  replaceActivityWith<MetalioAudioTestActivity>();
}
