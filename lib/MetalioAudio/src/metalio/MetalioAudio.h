#pragma once

// Metalio E-INK4 audio HAL.
// Local copy of the required FreeInk SDK audio-bus implementation.
// Kept inside MetalioAudio so the project does not modify freeink-sdk.

#include <BoardConfig.h>
#include <HardwareSerial.h>
#include <driver/i2s_std.h>
#include <MetalioEInk4Board.h>
#include <detail/MetalioPcm.h>

namespace freeink {
namespace metalio {

constexpr uint32_t AUDIO_RATE = 16000;

struct AudioBus {
  i2s_chan_handle_t tx = nullptr;
  i2s_chan_handle_t rx = nullptr;

  const void* speaker = nullptr;
  const void* microphone = nullptr;

  bool txEnabled = false;
  bool rxEnabled = false;

  HardwareSerial uart{2};
};

inline AudioBus& audioBus() {
  static AudioBus bus;
  return bus;
}

inline void destroyAudioBus() {
  auto& bus = audioBus();

  if (bus.txEnabled && bus.tx != nullptr) {
    i2s_channel_disable(bus.tx);
  }

  if (bus.rxEnabled && bus.rx != nullptr) {
    i2s_channel_disable(bus.rx);
  }

  bus.txEnabled = false;
  bus.rxEnabled = false;

  if (bus.tx != nullptr) {
    i2s_del_channel(bus.tx);
    bus.tx = nullptr;
  }

  if (bus.rx != nullptr) {
    i2s_del_channel(bus.rx);
    bus.rx = nullptr;
  }

  bus.uart.end();
}

inline bool acquireAudio(const void* owner, bool input) {
  auto& bus = audioBus();

  const void*& slot = input ? bus.microphone : bus.speaker;

  if (slot != nullptr) {
    return slot == owner;
  }

  if (bus.tx == nullptr || bus.rx == nullptr) {
    if (!ensureBooted() || !setAmplifier(false)) {
      return false;
    }

    // Metalio vendor BT audio initialization.
    // UART2: RX=GPIO47, TX=GPIO48.
    bus.uart.begin(115200, SERIAL_8N1, 47, 48);

    if (bus.uart.print("AT+RX=2\r\n") != 9) {
      bus.uart.end();
      return false;
    }

    bus.uart.flush();
    delay(700);

    if (bus.uart.print("AT+MODE=1\r\n") != 11) {
      bus.uart.end();
      return false;
    }

    bus.uart.flush();

    i2s_chan_config_t chan =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_SLAVE);

    chan.auto_clear = true;

    if (i2s_new_channel(&chan, &bus.tx, &bus.rx) != ESP_OK) {
      destroyAudioBus();
      return false;
    }

    i2s_std_config_t config = {};

    config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE);

    config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_32BIT,
        I2S_SLOT_MODE_STEREO);

    config.gpio_cfg.mclk = I2S_GPIO_UNUSED;

    config.gpio_cfg.bclk =
        static_cast<gpio_num_t>(BoardConfig::ACTIVE.audio.bclk);

    config.gpio_cfg.ws =
        static_cast<gpio_num_t>(BoardConfig::ACTIVE.audio.lrclk);

    config.gpio_cfg.dout =
        static_cast<gpio_num_t>(BoardConfig::ACTIVE.audio.dout);

    config.gpio_cfg.din =
        static_cast<gpio_num_t>(BoardConfig::ACTIVE.mic.data);

    if (i2s_channel_init_std_mode(bus.tx, &config) != ESP_OK ||
        i2s_channel_init_std_mode(bus.rx, &config) != ESP_OK) {
      destroyAudioBus();
      return false;
    }
  }

  slot = owner;
  return true;
}

inline bool startAudio(const void* owner, bool input) {
  auto& bus = audioBus();

  if ((input ? bus.microphone : bus.speaker) != owner) {
    return false;
  }

  bool& enabled = input ? bus.rxEnabled : bus.txEnabled;

  if (enabled) {
    return true;
  }

  i2s_chan_handle_t channel = input ? bus.rx : bus.tx;

  if (channel == nullptr) {
    return false;
  }

  if (i2s_channel_enable(channel) != ESP_OK) {
    return false;
  }

  enabled = true;
  return true;
}

inline void stopAudio(const void* owner, bool input) {
  auto& bus = audioBus();

  if ((input ? bus.microphone : bus.speaker) != owner) {
    return;
  }

  bool& enabled = input ? bus.rxEnabled : bus.txEnabled;

  if (enabled) {
    i2s_chan_handle_t channel = input ? bus.rx : bus.tx;

    if (channel != nullptr) {
      i2s_channel_disable(channel);
    }

    enabled = false;
  }
}

inline void releaseAudio(const void* owner, bool input) {
  auto& bus = audioBus();

  const void*& slot = input ? bus.microphone : bus.speaker;

  if (slot != owner) {
    return;
  }

  stopAudio(owner, input);
  slot = nullptr;

  if (bus.speaker == nullptr && bus.microphone == nullptr) {
    destroyAudioBus();
  }
}

}  // namespace metalio
}  // namespace freeink
