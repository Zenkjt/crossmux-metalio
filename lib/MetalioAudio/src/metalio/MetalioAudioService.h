#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace metalio_audio_service {

// Full Metalio audio infrastructure:
// hardware codec -> PCM input/output tasks -> Opus codec task -> queues.
// Audio is deliberately not started at boot; callers own start/stop.

bool start();
void stop();

bool playPcm(const int16_t* samples, std::size_t count,
             uint32_t timeoutMs = 3000);
bool queuePcm(const int16_t* samples, std::size_t count);
void flushPlayback();

bool startCapture(bool encodeToOpus = false);
void stopCapture();
bool readCapturedPcm(std::vector<int16_t>& samples,
                     std::size_t maxSamples,
                     uint32_t timeoutMs = 1000);
bool recordPcm(std::vector<int16_t>& samples,
               std::size_t count,
               uint32_t timeoutMs = 5000);

// Layer-2 Opus packet path.
bool encodePcmFrame(const int16_t* samples, std::size_t count,
                    std::vector<uint8_t>& packet);
bool decodeOpusPacket(const uint8_t* packet, std::size_t packetBytes,
                      std::vector<int16_t>& samples);
bool pushOpusPacket(const uint8_t* packet, std::size_t packetBytes,
                    bool wait = false);
bool popEncodedPacket(std::vector<uint8_t>& packet);

void setOutputVolume(uint8_t volume);
uint8_t outputVolume();

void playTestTone();

}  // namespace metalio_audio_service
