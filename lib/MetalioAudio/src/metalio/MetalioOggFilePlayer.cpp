
    if (!allocatePcmBuffers()) {
      destroyPcmBuffers();
      finish(false);
      return false;
    }
    if (!startTasks()) {
      requestStop();
      const bool tasksStopped = waitTasks(5000);
      if (tasksStopped) destroyPcmBuffers();
      else setError("Task startup rollback timed out; buffers retained");
      finish(false);
      return false;
    }
    LOG_INF(kTag, "OGG streaming tasks started");
    return true;
  }

  void requestStop() {
    std::lock_guard<std::mutex> lock(mutex);
    stopRequested = true;
  }

  bool waitTasks(uint32_t timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    while (true) {
      bool done;
      {
        std::lock_guard<std::mutex> lock(mutex);
        done = readerTask == nullptr && outputTask == nullptr;
      }
      if (done) return true;
      if (std::chrono::steady_clock::now() >= deadline) return false;
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }

  void stop() {
    requestStop();
    if (!waitTasks(10000)) {
      setError("Timed out waiting for OGG tasks to stop; buffers retained");
      // Worker tasks still reference Impl and its queues. Do not free them here.
      return;
    }
    metalio_audio_service::stop();
    destroyPcmBuffers();
    {
      std::lock_guard<std::mutex> lock(mutex);
      running = false;
    }
    cv.notify_all();
  }
};

Player::Player() : impl_(new (std::nothrow) Impl()) {}
Player::~Player() {
  stop();
  if (impl_ != nullptr) {
    // If a worker exceeded the first stop timeout, wait rather than delete
    // Impl while that worker still holds its address.
    while (true) {
      bool tasksDone;
      {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        tasksDone = impl_->readerTask == nullptr && impl_->outputTask == nullptr;
      }
      if (tasksDone) break;
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
  delete impl_;
}
bool Player::play(const char* path) { return impl_ != nullptr && impl_->start(path); }
bool Player::wait(uint32_t timeoutMs) {
  if (impl_ == nullptr) return false;
  std::unique_lock<std::mutex> lock(impl_->mutex);
  const bool done = impl_->cv.wait_for(
      lock, std::chrono::milliseconds(timeoutMs),
      [this] { return !impl_->running; });
  return done && impl_->success;
}
void Player::stop() { if (impl_ != nullptr) impl_->stop(); }
bool Player::isPlaying() const {
  if (impl_ == nullptr) return false;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->running;
}
bool Player::lastSuccess() const {
  if (impl_ == nullptr) return false;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->success;
}
const char* Player::lastError() const {
  if (impl_ == nullptr) return "Player unavailable";
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->error;
}

}  // namespace metalio_ogg_file_player

#else

namespace metalio_ogg_file_player {
Player::Player() : impl_(nullptr) {}
Player::~Player() = default;
bool Player::play(const char*) { return false; }
bool Player::wait(uint32_t) { return false; }
void Player::stop() {}
bool Player::isPlaying() const { return false; }
bool Player::lastSuccess() const { return false; }
const char* Player::lastError() const { return "Metalio E-INK4 only"; }
}  // namespace metalio_ogg_file_player

#endif
