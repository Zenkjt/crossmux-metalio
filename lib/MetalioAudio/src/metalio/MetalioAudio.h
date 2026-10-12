#pragma once

#include <BoardConfig.h>
#include <HardwareSerial.h>
#include <driver/i2s_std.h>
#include <esp_err.h>
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

inline void logErr(const char* op, esp_err_t err) {
  Serial.printf("[METALIO-AUDIO] %s -> %s (0x%X)\n",
                op, esp_err_to_name(err), static_cast<unsigned>(err));
}

inline void logPins() {
  Serial.printf("[METALIO-AUDIO] I2S pins: BCLK=%d WS=%d DOUT=%d DIN=%d\n",
                static_cast<int>(BoardConfig::ACTIVE.audio.bclk),
                static_cast<int>(BoardConfig::ACTIVE.audio.lrclk),
                static_cast<int>(BoardConfig::ACTIVE.audio.dout),
                static_cast<int>(BoardConfig::ACTIVE.mic.data));
  Serial.println("[METALIO-AUDIO] Expected Metalio E-INK4: BCLK=6 WS=43 DOUT=7 DIN=17");
  Serial.println("[METALIO-AUDIO] BT UART: RX=47 TX=48 baud=115200");
}

inline void logUart(const char* command) {
  auto& bus = audioBus();
  delay(100);
  char buf[256];
  size_t n = 0;
  while (bus.uart.available() && n < sizeof(buf) - 1)
    buf[n++] = static_cast<char>(bus.uart.read());
  buf[n] = 0;

  Serial.printf("[METALIO-AUDIO] UART %s -> %u bytes: ",
                command, static_cast<unsigned>(n));
  for (size_t i = 0; i < n; ++i) {
    unsigned char c = static_cast<unsigned char>(buf[i]);
    if (c >= 32 && c <= 126) Serial.write(c);
    else Serial.printf("\\x%02X", c);
  }
  Serial.println();
}

inline void destroyAudioBus() {
  auto& bus = audioBus();

  if (bus.txEnabled && bus.tx) {
    logErr("TX disable", i2s_channel_disable(bus.tx));
  }
  if (bus.rxEnabled && bus.rx) {
    logErr("RX disable", i2s_channel_disable(bus.rx));
  }

  bus.txEnabled = false;
  bus.rxEnabled = false;

  if (bus.tx) {
    logErr("TX delete", i2s_del_channel(bus.tx));
    bus.tx = nullptr;
  }
  if (bus.rx) {
    logErr("RX delete", i2s_del_channel(bus.rx));
    bus.rx = nullptr;
  }

  bus.uart.end();
}

inline bool acquireAudio(const void* owner, bool input) {
  auto& bus = audioBus();
  const void*& slot = input ? bus.microphone : bus.speaker;

  if (slot != nullptr) return slot == owner;

  if (bus.tx == nullptr || bus.rx == nullptr) {
    if (!ensureBooted() || !setAmplifier(false)) {
      Serial.println("[METALIO-AUDIO] ensureBooted/setAmplifier failed");
      return false;
    }

    logPins();

    bus.uart.begin(115200, SERIAL_8N1, 47, 48);

    size_t n = bus.uart.print("AT+RX=2\r\n");
    Serial.printf("[METALIO-AUDIO] AT+RX=2 write -> %u bytes (expected 9)\n",
                  static_cast<unsigned>(n));
    if (n != 9) {
      bus.uart.end();
      return false;
    }
    bus.uart.flush();
    delay(700);
    logUart("AT+RX=2");

    n = bus.uart.print("AT+MODE=1\r\n");
    Serial.printf("[METALIO-AUDIO] AT+MODE=1 write -> %u bytes (expected 11)\n",
                  static_cast<unsigned>(n));
    if (n != 11) {
      bus.uart.end();
      return false;
    }
    bus.uart.flush();
    logUart("AT+MODE=1");

    i2s_chan_config_t chan =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_SLAVE);
    chan.auto_clear = true;

    esp_err_t err = i2s_new_channel(&chan, &bus.tx, &bus.rx);
    logErr("i2s_new_channel", err);
    Serial.printf("[METALIO-AUDIO] I2S handles: TX=%p RX=%p\n",
                  static_cast<void*>(bus.tx), static_cast<void*>(bus.rx));
    if (err != ESP_OK) {
      destroyAudioBus();
      return false;
    }

    i2s_std_config_t config = {};
    config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE);
    config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
    config.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    config.gpio_cfg.bclk =
        static_cast<gpio_num_t>(BoardConfig::ACTIVE.audio.bclk);
    config.gpio_cfg.ws =
        static_cast<gpio_num_t>(BoardConfig::ACTIVE.audio.lrclk);
    config.gpio_cfg.dout =
        static_cast<gpio_num_t>(BoardConfig::ACTIVE.audio.dout);
    config.gpio_cfg.din =
        static_cast<gpio_num_t>(BoardConfig::ACTIVE.mic.data);

    const esp_err_t txErr = i2s_channel_init_std_mode(bus.tx, &config);
    logErr("TX init", txErr);
    const esp_err_t rxErr = i2s_channel_init_std_mode(bus.rx, &config);
    logErr("RX init", rxErr);

    if (txErr != ESP_OK || rxErr != ESP_OK) {
      destroyAudioBus();
      return false;
    }

    Serial.println("[METALIO-AUDIO] I2S init complete: role=SLAVE rate=16000 width=32 stereo");
  }

  slot = owner;
  return true;
}

inline bool startAudio(const void* owner, bool input) {
  auto& bus = audioBus();
  if ((input ? bus.microphone : bus.speaker) != owner) return false;

  bool& enabled = input ? bus.rxEnabled : bus.txEnabled;
  if (enabled) return true;

  i2s_chan_handle_t channel = input ? bus.rx : bus.tx;
  if (!channel) {
    Serial.printf("[METALIO-AUDIO] startAudio %s: NULL channel\n",
                  input ? "RX" : "TX");
    return false;
  }

  Serial.printf("[METALIO-AUDIO] enabling %s channel=%p\n",
                input ? "RX" : "TX", static_cast<void*>(channel));

  const esp_err_t err = i2s_channel_enable(channel);
  logErr(input ? "RX enable" : "TX enable", err);
  if (err != ESP_OK) return false;

  enabled = true;
  Serial.printf("[METALIO-AUDIO] %s channel enabled\n",
                input ? "RX" : "TX");
  return true;
}

inline void stopAudio(const void* owner, bool input) {
  auto& bus = audioBus();
  if ((input ? bus.microphone : bus.speaker) != owner) return;

  bool& enabled = input ? bus.rxEnabled : bus.txEnabled;
  if (enabled) {
    i2s_chan_handle_t channel = input ? bus.rx : bus.tx;
    if (channel) logErr(input ? "RX disable" : "TX disable",
                        i2s_channel_disable(channel));
    enabled = false;
  }
}

inline void releaseAudio(const void* owner, bool input) {
  auto& bus = audioBus();
  const void*& slot = input ? bus.microphone : bus.speaker;
  if (slot != owner) return;

  stopAudio(owner, input);
  slot = nullptr;

  if (bus.speaker == nullptr && bus.microphone == nullptr)
    destroyAudioBus();
}

}  // namespace metalio
}  // namespace freeink
