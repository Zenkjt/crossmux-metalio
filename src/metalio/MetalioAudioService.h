#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace metalio_audio_service {

// Layer-2 PCM service for the Metalio E-Ink 4 audio HAL.
// The service owns FreeRTOS I/O tasks and presents a stable PCM API to
// higher-level CrossMux applications. Opus/AI transport stays above this layer.

bool start();
void stop();

bool playPcm(const int16_t* samples, std::size_t count,
             uint32_t timeoutMs = 2000);
bool queuePcm(const int16_t* samples, std::size_t count);
void flushPlayback();

bool startCapture();
void stopCapture();
bool readCapturedPcm(std::vector<int16_t>& samples,
                     std::size_t maxSamples,
                     uint32_t timeoutMs = 1000);
bool recordPcm(std::vector<int16_t>& samples,
               std::size_t count,
               uint32_t timeoutMs = 1000);

void setOutputVolume(uint8_t volume);
uint8_t outputVolume();

void playTestTone();

}  // namespace metalio_audio_service
