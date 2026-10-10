#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace metalio_audio_service {

bool start();
void stop();

bool playPcm(const int16_t* samples, std::size_t count, uint32_t timeoutMs = 1000);
bool recordPcm(std::vector<int16_t>& samples, std::size_t count,
               uint32_t timeoutMs = 1000);

bool queuePcm(const int16_t* samples, std::size_t count);
void flushPlayback();

void setOutputVolume(uint8_t volume);
uint8_t outputVolume();

void playTestTone();

}  // namespace metalio_audio_service
