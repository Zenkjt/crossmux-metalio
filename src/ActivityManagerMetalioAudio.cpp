#include "activities/ActivityManager.h"

#include "apps/metalio-audio/MetalioAudioTestActivity.h"

void ActivityManager::goToMetalioAudioTest() {
  replaceActivityWith<MetalioAudioTestActivity>();
}
