#pragma once

namespace metalio_speaker_test {

// Explicit hardware bring-up test launched from the Metalio Audio Test app.
void runTest();

// Legacy boot-hook entry point. Kept as a no-op for source compatibility.
void run();

}  // namespace metalio_speaker_test
