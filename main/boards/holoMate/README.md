# HoloMate

HoloMate 是基于 ESP32-S3 的自定义开发板。本适配当前专注于音频功能，使用
ES8311 音频 Codec 和 ES7210 双麦 ADC，暂不启用显示屏。

## 音频配置

- 输入采样率：24 kHz
- 输出采样率：24 kHz
- 设备端 AEC：开启
- ES8311：默认 I2C 地址
- ES7210：I2C 地址 `0x82`

## GPIO

| 功能 | GPIO |
| --- | ---: |
| I2S MCLK | 2 |
| I2S WS | 45 |
| I2S BCLK | 17 |
| I2S DIN | 16 |
| I2S DOUT | 15 |
| I2C SDA | 8 |
| I2C SCL | 18 |
| 功放控制 | 46 |
| Boot 按键 | 0 |

## 配置

HoloMate 的默认配置位于项目根目录的 `sdkconfig.holomate.defaults`：

```text
CONFIG_BOARD_TYPE_HOLOMATE=y
CONFIG_USE_DEVICE_AEC=y
```

编译时将该文件加入 `SDKCONFIG_DEFAULTS`。
