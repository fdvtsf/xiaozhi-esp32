#pragma once

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE 24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000
#define AUDIO_INPUT_REFERENCE true
#define AUDIO_INPUT_GAIN_DB 37.5f

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_11
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_10
#define AUDIO_I2S_GPIO_WS GPIO_NUM_8
// DI / DO labels use the ESP32 development board's perspective, not the codec's.
// DI: microphone data into ESP32; DO: playback data out of ESP32.
#define AUDIO_I2S_GPIO_DIN GPIO_NUM_7
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_9
#define AUDIO_CODEC_PA_PIN GPIO_NUM_14
#define AUDIO_CODEC_I2C_SDA_PIN GPIO_NUM_13
#define AUDIO_CODEC_I2C_SCL_PIN GPIO_NUM_12
#define AUDIO_CODEC_ES8311_ADDR ES8311_CODEC_DEFAULT_ADDR
#define AUDIO_CODEC_ES7210_ADDR 0x82

// No user button is shown in the board schematic. Optional external switch to GND.
#define BOOT_BUTTON_GPIO GPIO_NUM_NC

#define DISPLAY_MOSI_PIN GPIO_NUM_41
#define DISPLAY_CLK_PIN GPIO_NUM_40
#define DISPLAY_CS_PIN GPIO_NUM_39
#define DISPLAY_DC_PIN GPIO_NUM_38
#define DISPLAY_RST_PIN GPIO_NUM_42
#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_20
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false
#define DISPLAY_SPI_MODE 0
#define DISPLAY_SPI_CLOCK_HZ (20 * 1000 * 1000)
#define DISPLAY_WIDTH 240
#define DISPLAY_HEIGHT 240

// Factory demo's Normal orientation: MADCTL=0xC0, visible rows 80..319.
#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y true
#define DISPLAY_SWAP_XY false
#define DISPLAY_OFFSET_X 0
#define DISPLAY_OFFSET_Y 80
#define DISPLAY_INVERT_COLOR true
