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
constexpr int AUDIO_CODEC_DMA_DESC_NUM = 6;
constexpr int AUDIO_CODEC_DMA_FRAME_NUM = 240;

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
  Serial.println("[METALIO-AUDIO] I2S pins: BCLK=6 WS=43 DOUT=7 DIN=17 (Metalio E-INK4 fixed mapping)");
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
  Serial.printf("[METALIO-AUDIO] UART %s -> %u bytes: ", command,
                static_cast<unsigned>(n));
  for (size_t i = 0; i < n; ++i) {
    unsigned char c = static_cast<unsigned char>(buf[i]);
    if (c >= 32 && c <= 126) Serial.write(c);
    else Serial.printf("\\x%02X", c);
  }
  Serial.println();
}

inline void destroyAudioBus() {
  auto& bus = audioBus();
  if (bus.txEnabled && bus.tx)
    logErr("TX disable", i2s_channel_disable(bus.tx));
  if (bus.rxEnabled && bus.rx)
    logErr("RX disable", i2s_channel_disable(bus.rx));
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
    delay(500);
    logUart("AT+MODE=1");

    i2s_chan_config_t chan = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_SLAVE,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };

    esp_err_t err = i2s_new_channel(&chan, &bus.tx, &bus.rx);
    logErr("i2s_new_channel", err);
    Serial.printf("[METALIO-AUDIO] I2S handles: TX=%p RX=%p\n",
                  static_cast<void*>(bus.tx), static_cast<void*>(bus.rx));
    if (err != ESP_OK) {
      destroyAudioBus();
      return false;
    }

    i2s_std_config_t config = {};
    config.clk_cfg.sample_rate_hz = AUDIO_RATE;
    config.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
#ifdef I2S_HW_VERSION_2
    config.clk_cfg.ext_clk_freq_hz = 0;
#endif
    config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    config.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_32BIT;
    config.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
    config.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
    config.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    config.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_32BIT;
    config.slot_cfg.ws_pol = false;
    config.slot_cfg.bit_shift = true;
#ifdef I2S_HW_VERSION_2
    config.slot_cfg.left_align = true;
    config.slot_cfg.big_endian = false;
    config.slot_cfg.bit_order_lsb = false;
#endif
    config.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    config.gpio_cfg.bclk = GPIO_NUM_6;
    config.gpio_cfg.ws = GPIO_NUM_43;
    config.gpio_cfg.dout = GPIO_NUM_7;
    config.gpio_cfg.din = GPIO_NUM_17;

    const esp_err_t txErr = i2s_channel_init_std_mode(bus.tx, &config);
    logErr("TX init", txErr);
    const esp_err_t rxErr = i2s_channel_init_std_mode(bus.rx, &config);
    logErr("RX init", rxErr);
    if (txErr != ESP_OK || rxErr != ESP_OK) {
      destroyAudioBus();
      return false;
    }

    Serial.println("[METALIO-AUDIO] I2S init complete: Zao-compatible SLAVE / DMA 6x240 / 16k / 32-bit stereo / WS32 / bit_shift=1");
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
  if (!channel) return false;
  Serial.printf("[METALIO-AUDIO] enabling %s channel=%p\n",
                input ? "RX" : "TX", static_cast<void*>(channel));
  const esp_err_t err = i2s_channel_enable(channel);
  logErr(input ? "RX enable" : "TX enable", err);
  if (err != ESP_OK) return false;
  enabled = true;
  return true;
}

inline void stopAudio(const void* owner, bool input) {
  auto& bus = audioBus();
  if ((input ? bus.microphone : bus.speaker) != owner) return;
  bool& enabled = input ? bus.rxEnabled : bus.txEnabled;
  if (enabled) {
    i2s_chan_handle_t channel = input ? bus.rx : bus.tx;
    if (channel) logErr(input ? "RX disable" : "TX disable", i2s_channel_disable(channel));
    enabled = false;
  }
}

inline void releaseAudio(const void* owner, bool input) {
  auto& bus = audioBus();
  const void*& slot = input ? bus.microphone : bus.speaker;
  if (slot != owner) return;
  stopAudio(owner, input);
  slot = nullptr;
  if (bus.speaker == nullptr && bus.microphone == nullptr) destroyAudioBus();
}

}  // namespace metalio
}  // namespace freeink
