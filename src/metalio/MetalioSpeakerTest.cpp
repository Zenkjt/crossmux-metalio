    freeink::metalio::releaseAudio(&owner, false);
    return;
  }

  LOG_INF(
      "METALIO-AUDIO",
      "Speaker path active: UART2 115200, I2S slave, %u Hz, 32-bit stereo, "
      "BCLK=%d WS=%d DOUT=%d",
      static_cast<unsigned>(kSampleRate),
      static_cast<int>(BoardConfig::ACTIVE.audio.bclk),
      static_cast<int>(BoardConfig::ACTIVE.audio.lrclk),
      static_cast<int>(BoardConfig::ACTIVE.audio.dout));

  bool ok = true;

  for (int bufferIndex = 0; bufferIndex < kToneBuffers; ++bufferIndex) {
    fillToneBuffer(static_cast<size_t>(bufferIndex) * kFramesPerBuffer);

    size_t bytesWritten = 0;
    const esp_err_t err = i2s_channel_write(
        bus.tx,
        toneBuffer,
        sizeof(toneBuffer),
        &bytesWritten,
        pdMS_TO_TICKS(100));

    if (err != ESP_OK || bytesWritten != sizeof(toneBuffer)) {
      LOG_ERR("METALIO-AUDIO",
              "I2S write failed: err=%d bytes=%u/%u",
              static_cast<int>(err),
              static_cast<unsigned>(bytesWritten),
              static_cast<unsigned>(sizeof(toneBuffer)));
      ok = false;
      break;
    }
  }

  freeink::metalio::setAmplifier(false);
  freeink::metalio::stopAudio(&owner, false);
  freeink::metalio::releaseAudio(&owner, false);

  LOG_INF("METALIO-AUDIO", "Local speaker test %s",
          ok ? "completed" : "failed");
}

}  // namespace metalio_speaker_test

#else

namespace metalio_speaker_test {
void run() {}
}  // namespace metalio_speaker_test

#endif
