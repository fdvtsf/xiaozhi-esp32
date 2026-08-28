#include "application.h"
#include "button.h"
#include "codecs/box_audio_codec.h"
#include "config.h"
#include "display/lcd_display.h"
#include "wifi_board.h"

#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>

// GPIO19 drives the reset circuit; GPIO20 drives the LCD backlight. USB connects
// to CH343P/UART0, NOT the S3 native USB peripheral. Reject unsafe manual configs.
#if CONFIG_ESP_CONSOLE_USB_CDC || CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG || \
    CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG
#error "Waveshare LCD-1.3 requires UART0 console and no native USB secondary console"
#endif

namespace {
constexpr const char* TAG = "HoloMateWaveshare";
}

class HoloMateWaveshareBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_ = nullptr;
    LcdDisplay* display_ = nullptr;
    Button boot_button_;

    void InitializeAudioI2c() {
        // Match BoxAudioCodec; reserve I2C0 for the onboard IMU (not enabled here).
        i2c_master_bus_config_t config = {};
        config.i2c_port = I2C_NUM_1;
        config.sda_io_num = AUDIO_CODEC_I2C_SDA_PIN;
        config.scl_io_num = AUDIO_CODEC_I2C_SCL_PIN;
        config.clk_source = I2C_CLK_SRC_DEFAULT;
        config.glitch_ignore_cnt = 7;
        config.flags.enable_internal_pullup = true;
        ESP_ERROR_CHECK(i2c_new_master_bus(&config, &codec_i2c_bus_));
    }

    void InitializeDisplay() {
        // Create PWM at zero brightness before resetting / initializing the LCD.
        GetBacklight();
        spi_bus_config_t bus_config = {};
        bus_config.mosi_io_num = DISPLAY_MOSI_PIN;
        bus_config.miso_io_num = GPIO_NUM_NC;
        bus_config.sclk_io_num = DISPLAY_CLK_PIN;
        bus_config.quadwp_io_num = GPIO_NUM_NC;
        bus_config.quadhd_io_num = GPIO_NUM_NC;
        bus_config.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_CH_AUTO));

        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = DISPLAY_SPI_CLOCK_HZ;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        esp_lcd_panel_handle_t panel = nullptr;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));

        // Reuse XiaoZhi's LVGL9 port (20-row DMA buffer), not the factory LVGL8 tasks.
        // Apply the visible-area offset here only; do not also set a panel gap.
        display_ = new SpiLcdDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                     DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                     DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        GetBacklight()->RestoreBrightness();
        ESP_LOGI(TAG, "ST7789 240x240, SPI 20MHz, backlight GPIO20, offset (0,80)");
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            Application::GetInstance().Schedule([this]() {
                auto& app = Application::GetInstance();
                if (app.GetDeviceState() == kDeviceStateStarting) {
                    EnterWifiConfigMode();
                    return;
                }
                app.ToggleChatState();
            });
        });
    }

public:
    HoloMateWaveshareBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeAudioI2c();
        InitializeDisplay();
        InitializeButtons();
    }

    AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec codec(
            codec_i2c_bus_, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE, AUDIO_INPUT_GAIN_DB);
        return &codec;
    }

    Display* GetDisplay() override { return display_; }

    Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    void SetPowerSaveLevel(PowerSaveLevel level) override {
        (void)level;
        WifiBoard::SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    }
};

DECLARE_BOARD(HoloMateWaveshareBoard);
