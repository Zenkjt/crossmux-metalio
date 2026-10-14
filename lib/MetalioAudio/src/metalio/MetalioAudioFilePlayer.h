#pragma once

#include <cstdint>

namespace metalio_audio_file_player {

class Player {
 public:
  Player();
  ~Player();

  Player(const Player&) = delete;
  Player& operator=(const Player&) = delete;

  bool playWav(const char* path);
  bool wait(uint32_t timeoutMs = 120000);
  void stop();

  bool isPlaying() const;
  bool lastSuccess() const;
  const char* lastError() const;

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace metalio_audio_file_player
