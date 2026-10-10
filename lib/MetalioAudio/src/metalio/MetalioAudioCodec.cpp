#include "MetalioAudioCodec.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>

#define TAG "MetalioAudioCodec"

namespace {
constexpr int kDmaDescNum = 6;
constexpr int kDmaFrameNum = 240;
}

MetalioAudioCodec::MetalioAudioCodec() = default;

MetalioAudioCodec::MetalioAudioCodec(int inputSampleRate,
                                     int outputSampleRate,
                                     gpio_num_t bclk,
                                     gpio_num_t ws,
                                     gpio_num_t dout,
                                     gpio_num_t din)
    : inputSampleRate_(inputSampleRate),
      outputSampleRate_(outputSampleRate),
      bclk_(bclk),
      ws_(ws),
      dout_(dout),
      din_(din) {
    createDuplexChannels();
}

MetalioAudioCodec::~MetalioAudioCodec() {
    stopInput();
    stopOutput();
    destroyChannels();
}

void MetalioAudioCodec::createDuplexChannels() {
    if (txHandle_ != nullptr || rxHandle_ != nullptr) {
        return;
    }

    const i2s_chan_config_t chanCfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_SLAVE,
        .dma_desc_num = kDmaDescNum,
        .dma_frame_num = kDmaFrameNum,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };

    ESP_ERROR_CHECK(i2s_new_channel(&chanCfg, &txHandle_, &rxHandle_));

    const i2s_std_config_t stdCfg = {
        .clk_cfg = {
            .sample_rate_hz = static_cast<uint32_t>(outputSampleRate_),
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
#ifdef I2S_HW_VERSION_2
            .ext_clk_freq_hz = 0,
#endif
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
            .ws_pol = false,
            .bit_shift = true,
#ifdef I2S_HW_VERSION_2
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
#endif
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = bclk_,
            .ws = ws_,
            .dout = dout_,
            .din = din_,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(txHandle_, &stdCfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rxHandle_, &stdCfg));

    ESP_LOGI(TAG,
             "I2S duplex created: %d Hz, BCLK=%d WS=%d DOUT=%d DIN=%d",
             outputSampleRate_,
             static_cast<int>(bclk_),
             static_cast<int>(ws_),
             static_cast<int>(dout_),
             static_cast<int>(din_));
}

void MetalioAudioCodec::destroyChannels() {
    if (rxHandle_ != nullptr) {
        ESP_ERROR_CHECK(i2s_del_channel(rxHandle_));
        rxHandle_ = nullptr;
    }

    if (txHandle_ != nullptr) {
        ESP_ERROR_CHECK(i2s_del_channel(txHandle_));
        txHandle_ = nullptr;
    }
}

bool MetalioAudioCodec::startInput() {
    if (rxHandle_ == nullptr) {
        return false;
    }

    if (!inputRunning_) {
        const esp_err_t err = i2s_channel_enable(rxHandle_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2S input enable failed: %s", esp_err_to_name(err));
            return false;
        }
        inputRunning_ = true;
    }

    return true;
}

void MetalioAudioCodec::stopInput() {
    if (rxHandle_ != nullptr && inputRunning_) {
        const esp_err_t err = i2s_channel_disable(rxHandle_);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "I2S input disable failed: %s", esp_err_to_name(err));
        }
        inputRunning_ = false;
    }
}

bool MetalioAudioCodec::startOutput() {
    if (txHandle_ == nullptr) {
        return false;
    }

    if (!outputRunning_) {
        const esp_err_t err = i2s_channel_enable(txHandle_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2S output enable failed: %s", esp_err_to_name(err));
            return false;
        }
        outputRunning_ = true;
    }

    return true;
}

void MetalioAudioCodec::stopOutput() {
    if (txHandle_ != nullptr && outputRunning_) {
        const esp_err_t err = i2s_channel_disable(txHandle_);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "I2S output disable failed: %s", esp_err_to_name(err));
        }
        outputRunning_ = false;
    }
}

std::size_t MetalioAudioCodec::read(int16_t* dest,
                                    std::size_t samples,
                                    uint32_t timeoutMs) {
    if (!inputRunning_ || rxHandle_ == nullptr || dest == nullptr || samples == 0) {
        return 0;
    }

    std::vector<int32_t> buffer(samples);
    size_t bytesRead = 0;

    const esp_err_t err = i2s_channel_read(
        rxHandle_,
        buffer.data(),
        samples * sizeof(int32_t),
        &bytesRead,
        pdMS_TO_TICKS(timeoutMs));

    if (err != ESP_OK) {
        if (err != ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "I2S input read failed: %s", esp_err_to_name(err));
        }
        return 0;
    }

    const std::size_t count = bytesRead / sizeof(int32_t);

    for (std::size_t i = 0; i < count; ++i) {
        const int32_t value = buffer[i] >> 12;
        dest[i] = static_cast<int16_t>(
            std::clamp<int32_t>(value, INT16_MIN, INT16_MAX));
    }

    return count;
}

std::size_t MetalioAudioCodec::write(const int16_t* data,
                                     std::size_t samples,
                                     uint32_t timeoutMs) {
    if (!outputRunning_ || txHandle_ == nullptr || data == nullptr || samples == 0) {
        return 0;
    }

    std::vector<int32_t> buffer(samples * 2);

    const double volume = static_cast<double>(outputVolume_) / 100.0;
    const int64_t volumeFactor =
        static_cast<int64_t>(std::pow(volume, 2.0) * 65536.0);

    for (std::size_t i = 0; i < samples; ++i) {
        const int64_t temp = static_cast<int64_t>(data[i]) * volumeFactor;
        const int32_t processed = static_cast<int32_t>(
            std::clamp<int64_t>(temp, INT32_MIN, INT32_MAX));

        buffer[i * 2] = processed;
        buffer[i * 2 + 1] = processed;
    }

    size_t bytesWritten = 0;
    const esp_err_t err = i2s_channel_write(
        txHandle_,
        buffer.data(),
        buffer.size() * sizeof(int32_t),
        &bytesWritten,
        pdMS_TO_TICKS(timeoutMs));

    if (err != ESP_OK) {
        if (err != ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "I2S output write failed: %s", esp_err_to_name(err));
        }
        return 0;
    }

    return bytesWritten / (2 * sizeof(int32_t));
}

void MetalioAudioCodec::setOutputVolume(uint8_t volume) {
    outputVolume_ = std::min<uint8_t>(volume, 100);
}

uint8_t MetalioAudioCodec::outputVolume() const {
    return outputVolume_;
}

bool MetalioAudioCodec::inputRunning() const {
    return inputRunning_;
}

bool MetalioAudioCodec::outputRunning() const {
    return outputRunning_;
}
