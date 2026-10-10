#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace metalio_ogg_opus {

bool playFile(const char* path);
bool recordFile(const char* path, uint32_t durationMs);

// Create a unique recording path under the official Metalio SD layout.
std::string nextRecordingPath();

}  // namespace metalio_ogg_opus
