
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace metalio_music_player {

struct TrackInfo {
  std::string path;
  std::string title;
  std::string artist;
  uint32_t durationMs = 0;
};

enum class PlayerState : uint8_t {
  Stopped,
  Loading,
  Playing,
  Paused,
  Error,
};

class MusicPlayer {
 public:
  static MusicPlayer& instance();

  bool begin();
  void shutdown();

  bool refreshLibrary();
  std::size_t trackCount() const;
  TrackInfo track(std::size_t index) const;

  std::size_t selectedIndex() const;
  void selectNext();
  void selectPrevious();

  bool playSelected();
  bool playIndex(std::size_t index);
  void togglePause();
  void nextTrack();
  void previousTrack();

  void volumeUp(uint8_t step = 5);
  void volumeDown(uint8_t step = 5);

  PlayerState state() const;
  TrackInfo currentTrack() const;
  uint32_t elapsedMs() const;
  uint32_t durationMs() const;
  uint8_t volume() const;
  std::string errorText() const;

 public:
  struct Impl;

 private:
  MusicPlayer();
  ~MusicPlayer() = default;
  MusicPlayer(const MusicPlayer&) = delete;
  MusicPlayer& operator=(const MusicPlayer&) = delete;

  Impl* impl_;
};

}  // namespace metalio_music_player
