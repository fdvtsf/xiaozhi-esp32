#include "application.h"
#include "button.h"
#include "codecs/box_audio_codec.h"
#include "config.h"
#include "display/lcd_display.h"
#include "wifi_board.h"

#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include <driver/spi_common.h>
#include <driver/temperature_sensor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_pm.h>
#include <esp_sleep.h>
#include <esp_timer.h>

#include <atomic>
#include <cmath>
#include <limits>

// GPIO19 drives the reset circuit; GPIO20 drives the LCD backlight. USB connects
// to CH343P/UART0, NOT the S3 native USB peripheral. Reject unsafe manual configs.
#if CONFIG_ESP_CONSOLE_USB_CDC || CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG || \
    CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG
#error "Waveshare LCD-1.3 requires UART0 console and no native USB secondary console"
#endif

namespace {
constexpr const char* TAG = "HoloMateWaveshare";
constexpr float kThermalThrottleOnC = 80.0f;
constexpr float kThermalThrottleOffC = 72.0f;
constexpr float kThermalCriticalOnC = 90.0f;
constexpr float kThermalCriticalOffC = 82.0f;
constexpr float kThermalShutdownC = 100.0f;
constexpr int kThermalShutdownSamples = 3;

enum class ThermalProtectionLevel {
    Normal,
    Throttled,
    Critical,
};

const char* ThermalLevelName(ThermalProtectionLevel level) {
    switch (level) {
        case ThermalProtectionLevel::Throttled:
            return "throttled";
        case ThermalProtectionLevel::Critical:
            return "critical";
        case ThermalProtectionLevel::Normal:
        default:
            return "normal";
    }
}

int ThermalCpuFrequency(ThermalProtectionLevel level) {
    switch (level) {
        case ThermalProtectionLevel::Throttled:
            return 160;
        case ThermalProtectionLevel::Critical:
            return 80;
        case ThermalProtectionLevel::Normal:
        default:
            return 240;
    }
}
}

class HoloMateWaveshareDisplay : public SpiLcdDisplay {
public:
    HoloMateWaveshareDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                             int width, int height, int offset_x, int offset_y, bool mirror_x,
                             bool mirror_y, bool swap_xy)
        : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y,
                        swap_xy) {}

    void SetupUI() override {
        SpiLcdDisplay::SetupUI();

        // This board is an expression-only display. Keep all text and status UI
        // hidden and let the emotion image use every pixel of the LCD.
        SetHideSubtitle(true);
        DisplayLockGuard lock(this);
        HideUiOverlays();
        ConfigureFullscreenEmotion();
    }

    void SetEmotion(const char* emotion) override {
        SpiLcdDisplay::SetEmotion(emotion);

        // The generic display falls back to a font icon when an emotion asset is
        // missing. Suppress that fallback here so this screen never shows text or
        // icons, and reapply sizing for every static image/GIF transition.
        DisplayLockGuard lock(this);
        HideUiOverlays();
        ConfigureFullscreenEmotion();
    }

    void SetChatMessage(const char* role, const char* content) override {
        (void)role;
        (void)content;
    }

    void SetStatus(const char* status) override { (void)status; }

    void ShowNotification(const char* notification, int duration_ms = 3000) override {
        (void)notification;
        (void)duration_ms;
    }

    void ShowNotification(const std::string& notification, int duration_ms = 3000) override {
        (void)notification;
        (void)duration_ms;
    }

    void UpdateStatusBar(bool update_all = false) override { (void)update_all; }

private:
    void HideUiOverlays() {
        lv_obj_t* overlays[] = {
            top_bar_,       status_bar_,        bottom_bar_,         emoji_label_,
            network_label_, status_label_,      notification_label_, mute_label_,
            battery_label_, low_battery_popup_, low_battery_label_,
        };
        for (auto* overlay : overlays) {
            if (overlay != nullptr) {
                lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    void ConfigureFullscreenEmotion() {
        if (emoji_box_ != nullptr) {
            lv_obj_set_size(emoji_box_, LV_HOR_RES, LV_VER_RES);
            lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, 0);
            lv_obj_set_style_pad_all(emoji_box_, 0, 0);
            lv_obj_set_style_border_width(emoji_box_, 0, 0);
            lv_obj_set_style_bg_opa(emoji_box_, LV_OPA_TRANSP, 0);
            lv_obj_clear_flag(emoji_box_, LV_OBJ_FLAG_SCROLLABLE);
        }
        if (emoji_image_ != nullptr) {
            lv_obj_set_size(emoji_image_, LV_HOR_RES, LV_VER_RES);
            lv_obj_center(emoji_image_);
            lv_image_set_inner_align(emoji_image_, LV_IMAGE_ALIGN_STRETCH);
        }
    }
};

class HoloMateWaveshareBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_ = nullptr;
    LcdDisplay* display_ = nullptr;
    Button boot_button_;
    temperature_sensor_handle_t temperature_sensor_ = nullptr;
    esp_timer_handle_t temperature_timer_ = nullptr;
    std::atomic<float> chip_temperature_{std::numeric_limits<float>::quiet_NaN()};
    ThermalProtectionLevel thermal_level_ = ThermalProtectionLevel::Normal;
    int thermal_shutdown_samples_ = 0;

    bool SetThermalProtectionLevel(ThermalProtectionLevel level, float celsius) {
        const int frequency = ThermalCpuFrequency(level);
        esp_pm_config_t pm_config = {
            .max_freq_mhz = frequency,
            .min_freq_mhz = frequency,
            .light_sleep_enable = false,
        };
        const esp_err_t err = esp_pm_configure(&pm_config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Thermal CPU limit failed at %.1f C: %s", celsius,
                     esp_err_to_name(err));
            return false;
        }
        thermal_level_ = level;
        if (std::isfinite(celsius)) {
            ESP_LOGW(TAG, "Thermal protection: level=%s temperature=%.1f C CPU=%d MHz",
                     ThermalLevelName(level), celsius, frequency);
        } else {
            ESP_LOGI(TAG, "Thermal protection initialized: level=%s CPU=%d MHz",
                     ThermalLevelName(level), frequency);
        }
        return true;
    }

    void UpdateThermalProtection(float celsius) {
        if (celsius >= kThermalShutdownC) {
            thermal_shutdown_samples_++;
        } else {
            thermal_shutdown_samples_ = 0;
        }

        if (thermal_shutdown_samples_ >= kThermalShutdownSamples) {
            ESP_LOGE(TAG,
                     "Thermal emergency: %.1f C for %d samples; disabling amplifier and "
                     "entering deep sleep until power cycle",
                     celsius, thermal_shutdown_samples_);
            gpio_set_level(AUDIO_CODEC_PA_PIN, 0);
            esp_deep_sleep_start();
        }

        ThermalProtectionLevel target = thermal_level_;
        switch (thermal_level_) {
            case ThermalProtectionLevel::Normal:
                if (celsius >= kThermalCriticalOnC) {
                    target = ThermalProtectionLevel::Critical;
                } else if (celsius >= kThermalThrottleOnC) {
                    target = ThermalProtectionLevel::Throttled;
                }
                break;
            case ThermalProtectionLevel::Throttled:
                if (celsius >= kThermalCriticalOnC) {
                    target = ThermalProtectionLevel::Critical;
                } else if (celsius <= kThermalThrottleOffC) {
                    target = ThermalProtectionLevel::Normal;
                }
                break;
            case ThermalProtectionLevel::Critical:
                if (celsius <= kThermalCriticalOffC) {
                    target = celsius >= kThermalThrottleOnC
                                 ? ThermalProtectionLevel::Throttled
                                 : ThermalProtectionLevel::Normal;
                }
                break;
        }
        if (target != thermal_level_) {
            SetThermalProtectionLevel(target, celsius);
        }
    }

    void InitializeThermalProtection() {
        SetThermalProtectionLevel(ThermalProtectionLevel::Normal,
                                  std::numeric_limits<float>::quiet_NaN());
        ESP_LOGI(TAG,
                 "Thermal thresholds: throttle %.0f/%.0f C, critical %.0f/%.0f C, "
                 "shutdown %.0f C x%d samples",
                 kThermalThrottleOnC, kThermalThrottleOffC, kThermalCriticalOnC,
                 kThermalCriticalOffC, kThermalShutdownC, kThermalShutdownSamples);
    }

    void SampleTemperature() {
        float celsius = std::numeric_limits<float>::quiet_NaN();
        esp_err_t err = temperature_sensor_enable(temperature_sensor_);
        if (err == ESP_OK) {
            err = temperature_sensor_get_celsius(temperature_sensor_, &celsius);
            const esp_err_t disable_err = temperature_sensor_disable(temperature_sensor_);
            if (err == ESP_OK) {
                err = disable_err;
            }
        }
        if (err != ESP_OK || !std::isfinite(celsius)) {
            chip_temperature_.store(std::numeric_limits<float>::quiet_NaN());
            ESP_LOGW(TAG, "Chip temperature unavailable: %s", esp_err_to_name(err));
            return;
        }
        chip_temperature_.store(celsius);
        ESP_LOGI(TAG, "ESP32-S3 internal temperature: %.1f C (not enclosure temperature)",
                 celsius);
        UpdateThermalProtection(celsius);
    }

    void InitializeTemperature() {
        // Board-lifetime resources; no extra task or work in the audio pipeline.
        // Only this timer accesses the driver. MCP queries use the cached sample.
        temperature_sensor_config_t config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(50, 125);
        esp_err_t err = temperature_sensor_install(&config, &temperature_sensor_);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Temperature sensor install failed: %s", esp_err_to_name(err));
            return;
        }
        esp_timer_create_args_t timer_args = {};
        timer_args.callback = [](void* arg) {
            static_cast<HoloMateWaveshareBoard*>(arg)->SampleTemperature();
        };
        timer_args.arg = this;
        timer_args.dispatch_method = ESP_TIMER_TASK;
        timer_args.name = "chip_temperature";
        timer_args.skip_unhandled_events = true;
        err = esp_timer_create(&timer_args, &temperature_timer_);
        if (err == ESP_OK) {
            err = esp_timer_start_periodic(temperature_timer_, 10 * 1000 * 1000);
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Temperature timer failed: %s", esp_err_to_name(err));
            if (temperature_timer_ != nullptr) {
                esp_timer_delete(temperature_timer_);
                temperature_timer_ = nullptr;
            }
            temperature_sensor_uninstall(temperature_sensor_);
            temperature_sensor_ = nullptr;
        }
    }

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
        display_ = new HoloMateWaveshareDisplay(
            panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        GetBacklight()->RestoreBrightness();
        ESP_LOGI(TAG, "ST7789 240x240, SPI 20MHz, CW 90 degrees, offset (%d,%d)", DISPLAY_OFFSET_X,
                 DISPLAY_OFFSET_Y);
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
        InitializeThermalProtection();
        InitializeTemperature();
    }

    bool GetTemperature(float& celsius) override {
        celsius = chip_temperature_.load();
        return std::isfinite(celsius);
    }

    AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec codec(
            codec_i2c_bus_, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE, AUDIO_INPUT_GAIN_DB, -1, 0.0f,
            BoxAudioCodecInputLayout{AUDIO_INPUT_MIC1_SLOT, AUDIO_INPUT_MIC2_SLOT,
                                     AUDIO_INPUT_REFERENCE_SLOT});
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
