#include "activities/ActivityManager.h"

#if FREEINK_DEVICE_METALIO_EINK4

#include "activities/apps/metalio-audio/MetalioAudioTestActivity.h"

void ActivityManager::goToMetalioAudioTest() {
  replaceActivityWith<MetalioAudioTestActivity>();
}

#endif
