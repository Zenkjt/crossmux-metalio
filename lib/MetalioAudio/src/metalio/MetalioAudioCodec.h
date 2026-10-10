#pragma once

#include <cstddef>
#include <cstdint>

#include <driver/gpio.h>
#include <driver/i2s_std.h>

class MetalioAudioCodec {
public:
    MetalioAudioCodec();
    MetalioAudioCodec(int inputSampleRate,
                      int outputSampleRate,
                      gpio_num_t bclk,
                      gpio_num_t ws,
                      gpio_num_t dout,
                      gpio_num_t din);
    ~MetalioAudioCodec();

    bool startInput();
    void stopInput();
    bool startOutput();
    void stopOutput();

    std::size_t read(int16_t* dest,
                     std::size_t samples,
                     uint32_t timeoutMs = 1000);
    std::size_t write(const int16_t* data,
                      std::size_t samples,
                      uint32_t timeoutMs = 1000);

    void setOutputVolume(uint8_t volume);
    uint8_t outputVolume() const;

    bool inputRunning() const;
    bool outputRunning() const;

private:
    void createDuplexChannels();
    void destroyChannels();

    int inputSampleRate_ = 16000;
    int outputSampleRate_ = 16000;

    gpio_num_t bclk_ = GPIO_NUM_NC;
    gpio_num_t ws_ = GPIO_NUM_NC;
    gpio_num_t dout_ = GPIO_NUM_NC;
    gpio_num_t din_ = GPIO_NUM_NC;

    i2s_chan_handle_t txHandle_ = nullptr;
    i2s_chan_handle_t rxHandle_ = nullptr;

    bool inputRunning_ = false;
    bool outputRunning_ = false;
    uint8_t outputVolume_ = 70;
};
