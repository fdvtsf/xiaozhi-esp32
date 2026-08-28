# HoloMate + Waveshare ESP32-S3-LCD-1.3

微雪 ESP32-S3-LCD-1.3 主板 + 原 HoloMate ES8311/ES7210 音频模块。
这是独立板型，不替换 `main/boards/holoMate`，不能与旧板混刷固件。

- 芯片：ESP32-S3R8，16MB Flash、8MB Octal PSRAM。
- LCD：ST7789V2，240x240，SPI Mode 0 / 20MHz，RGB565。
- ESP-IDF：使用现有 6.0.2 环境；发布配置要求 >= 6.0，未验证旧 IDF。
- 板型 / BOARD_NAME：`holomate-waveshare-s3-lcd-1.3`。
- 发布包前缀：`waveshare-holomate-waveshare-s3-lcd-1.3`。

## 接线

DO/DI 按**开发板（ESP32）视角**标注，这是容易接反的地方：
**DI = GPIO7，麦克风数据输入开发板；DO = GPIO9，开发板输出播放数据。**
连接器即使在音频板上，本文 DI/DO 也不是按音频芯片视角命名。
此前按音频模块视角配置的 DIN=9 / DOUT=7 已于 2026-08-28 更正。
以后说明接线或烧录本板时，应提醒用户确认这一方向，换线必须先断电。

| 接线标识（DI/DO 按开发板视角） | ESP32 GPIO | 配置 |
| --- | ---: | --- |
| EN | 14 | AUDIO_CODEC_PA_PIN |
| SDA | 13 | AUDIO_CODEC_I2C_SDA_PIN |
| SCL | 12 | AUDIO_CODEC_I2C_SCL_PIN |
| MCK | 11 | AUDIO_I2S_GPIO_MCLK |
| BCK | 10 | AUDIO_I2S_GPIO_BCLK |
| DO | 9 | AUDIO_I2S_GPIO_DOUT，播放输出 |
| WS | 8 | AUDIO_I2S_GPIO_WS |
| DI | 7 | AUDIO_I2S_GPIO_DIN，录音输入 |
| 5V | USB VBUS / 板上 5V | 先使用稳定 USB 供电 |
| GND | GND | 必须共地 |

I/O 电平及 I2C 上拉必须为 3.3V，不能向 ESP GPIO 输入 5V。
板上 5V 引脚来自 USB VBUS；电池供电时不能假定此引脚仍有 5V。
AEC 依赖原模块有效的扬声器参考信号通路；GPIO 接通不代表参考信号一定正确。
ES8311 使用驱动默认地址，ES7210 使用 codec API 的 `0x82` 地址表示法，
对应普通 7-bit I2C 扫描地址 `0x41`。

| 板载 LCD | GPIO |
| --- | ---: |
| MOSI | 41 |
| CLK | 40 |
| CS | 39 |
| DC | 38 |
| RESET | 42 |
| 背光 PWM | **20** |

默认方向跟随工厂例程 `Normal`：X/Y 镜像均开启，不交换 XY，Y 偏移 80。
偏移仅传给 `SpiLcdDisplay`，不要再额外调用 `esp_lcd_panel_set_gap` 加同样偏移。
若改变方向，需要同时复测偏移、裁切和文字方向。

## 与原 HoloMate 的一致性

| 项目 | 新板处理 |
| --- | --- |
| ES8311 + ES7210、24kHz、参考通道 | 复用 `BoxAudioCodec`，`AUDIO_INPUT_REFERENCE=true` |
| 麦克风模拟增益 | 保留 37.5dB，定义于新板 `config.h` |
| 设备端 AEC / 实时对话路径 | `CONFIG_USE_DEVICE_AEC=y`，复用现有应用及 AFE |
| AFE/VAD 参数 | 直接共享 `main/audio/engines/afe_audio_engine.cc`，不复制另一套参数 |
| 当前 AFE 基线 | `AEC_MODE_VOIP_HIGH_PERF`、`AEC_NLP_LEVEL_VERYAGGR`、`VAD_MODE_0`、`vad_min_noise_ms=100` |
| Jarvis 唤醒 | 仅启用 Jarvis 模型，关闭默认“你好小智”模型 |
| 唤醒词音频上传 | `CONFIG_SEND_WAKE_WORD_DATA=n` |
| 空闲性能模式 | 与原板相同，固定 `PowerSaveLevel::PERFORMANCE` |
| 配网页面的 OTA 服务器地址 | 继承 `WifiBoard::StartNetwork()` 的 `show_ota_config=true` |
| 语音音量控制 | 继承共享 MCP 音量工具 |
| 字体 / 表情 | 使用已迁移的 fonts 2.0；240x240 屏采用 20px 字体及 64px 默认表情 |

这里的迁移基线是当前保留的代码，不恢复已回退的语音连续对话开关、VAD_MODE_2
实验、四通道音频抓取、Idle AEC A/B 测试等临时修改。`vad_min_noise_ms=100`
是噪声/静音判定相关参数，不是“持续说话 100ms 即打断”的设定。
此前讨论的 Intro/Idle/Outro 表情状态机不在本次板级适配中实现。

原板的配置文件没有被修改。新板的 Wi-Fi 凭据、激活身份、音量、OTA URL 等 NVS
数据仍属于新设备，需要重新配置；本迁移不复制旧设备的 NVS/身份信息。
37.5dB 在旧声学结构上已验证，但更换麦克风/扬声器安装位置后仍需复测削波和误打断。

## USB、按键及未启用外设

- Type-C 通过 CH343P 接 UART0（TX GPIO43 / RX GPIO44），不是原生 USB。
- **GPIO19 接复位控制，GPIO20 接背光。禁止启用 USB CDC 或 USB Serial/JTAG
  主/辅助控制台。** 发布配置关闭辅助 USB 控制台，板级源文件也有编译检查。
- 新板原理图没有用户按键；`BOOT_BUTTON_GPIO=GPIO_NUM_NC`，默认不占用 GPIO0。
  若需要手动对话按钮，可外接 GPIO1 到 GND 的按钮并在本板 `config.h` 中设置；
  点击回调通过主任务调度，行为沿用原 HoloMate。
- 没有 Wi-Fi 配置时沿用小智自动进入配网的行为，不添加工厂扫描任务。
- 本次不初始化 RGB LED（GPIO15）、IMU（I2C SDA47/SCL48、INT45/46）、
  TF 卡（MISO16/CS17/MOSI18/CLK21）和电池采样（GPIO6）。
- 音频使用 I2C1，保留 I2C0 供后续 IMU 适配；不引入例程的旧版 I2C 驱动。

## 构建

配置的唯一来源是本目录的 `config.json`，不要手动修改生成的 sdkconfig。
不要将旧板的 `sdkconfig.holomate.defaults` 加入新板的 defaults 链。

### 独立目录开发构建（推荐本机使用）

在已激活 ESP-IDF 6.0.2 的终端、工程根目录执行：

```powershell
python scripts/build_holomate_waveshare.py
```

输出在 `build-holomate-waveshare-idf6/`。脚本从 `config.json` 生成
`variant.defaults`，使用 `sdkconfig.defaults;sdkconfig.defaults.esp32s3;variant.defaults`
配置链；生成的 `sdkconfig` 也在此目录中，原板配置和固件不受影响。
Windows 构建强制 Ninja response files，避免长命令行失败。
默认跳过组件管理器可选的“是否有新版本”联网检查，仍保留依赖解析和完整性检查；
不会关闭 TLS 校验，也不会升级组件。
配置检查会拒绝错误板型、丢失 AEC/Jarvis 配置或启用原生 USB 的构建。
如果旧构建缓存与更新后的配置冲突，使用新的独立目录，不自动删除已有构建：

```powershell
python scripts/build_holomate_waveshare.py --build-dir build-holomate-waveshare-idf6-next
```

本机现有环境路径：`C:\esp\v6.0.2\esp-idf`，对应
`C:\Users\fh\.espressif\python_env\idf6.0_py3.11_env`。
正常情况下在 PowerShell 中运行 `C:\esp\v6.0.2\esp-idf\export.ps1` 激活环境即可；
如发生 Python/依赖问题，复用 `build-flash-holomate` 技能的环境修复流程，
不要重新安装另一套 Python/IDF。该技能原脚本只构建旧板，不能把它生成的固件刷入新板。

### 官方发布入口

```powershell
python scripts/release.py waveshare/holomate-waveshare-s3-lcd-1.3 --name holomate-waveshare-s3-lcd-1.3
```

这是标准发布入口，会更新工程根目录 `sdkconfig` 和默认 `build/`；开发时若需要
保留旧板构建状态，优先使用上面的独立目录脚本。Windows 环境建议设置
`CMAKE_NINJA_FORCE_RESPONSE_FILE=ON` 后再使用发布入口。

### 烧录（仅在确认端口及接线后）

```powershell
idf.py -B build-holomate-waveshare-idf6 -p COMx flash
idf.py -B build-holomate-waveshare-idf6 -p COMx monitor
```

将 `COMx` 替换为本块 CH343P 的实际端口，不沿用旧 HoloMate 的端口号。
第一次使用完整 `flash`，包含分区、程序及生成资源；不要只烧 `xiaozhi.bin`。
烧录不是设备身份迁移。OTA 服务端也需要为新板型配置独立的固件发布渠道。

## 验证清单

主机测试：`python -m unittest discover -s scripts/tests -v`。

2026-08-28 软件验证结果：ESP-IDF 6.0.2 下新板和原 HoloMate 均编译通过，
14 项主机测试通过。新板 `xiaozhi.bin` 为 2,813,616 字节，应用分区剩余约 32%；
生成资源包包含 `wn9_jarvis_tts`。已核对新板独立编译入口、AEC/唤醒配置及
原生 USB 辅助控制台关闭状态。

2026-08-28 已将 DI/DO 更正为开发板视角的 DIN=GPIO7 / DOUT=GPIO9，
重新编译并通过 COM8（CH343）完整烧录，所有写入区域哈希校验通过，已复位启动。
设备 MAC 为 `b8:1f:3f:ab:ea:44`，未擦除 NVS；录音、播放和 Jarvis 唤醒效果仍待实机确认。

当前已知问题：用户反馈唤醒曾有响应，随后失效，屏幕也未显示状态文字。
重启抓取的日志确认已进入 idle、加载 `wn9_jarvis_tts`，ES7210 取消静音并开启采音，
但之后约 90 秒未出现唤醒事件，也未见重启或报错。尚未确认原始 PCM 是否有效或 AFE
是否持续处理，不能据此认定为供电、接线或算法问题；本提交为适配调试基线，非完整验收版本。
未加入额外诊断固件或更改共享音频参数。

编译成功只证明软件构建通过，实机仍需逐项确认：

1. 启动日志识别 ESP32-S3、16MB Flash、8MB PSRAM、新板型，无反复复位。
2. LCD 全屏可见、文字方向正常、红绿蓝正确、背光可调。
3. 配网成功，“OTA服务器地址”可配置，新设备正常激活。
4. 播放正常、麦克风有效、Jarvis 可唤醒，语音音量控制正常。
5. 播放中说话可打断；安静时不误打断，近距离大声说话不削波。
6. 屏幕持续更新与连续对话同时运行，观察欠载、内存余量、重连及长时间稳定性。

## 参考资料与例程差异

- [微雪产品页](https://www.waveshare.com/product/esp32-s3-lcd-1.3.htm)
- [原理图](https://files.waveshare.com/wiki/ESP32-S3-LCD-1.3/ESP32S3_1.3inch.pdf)
- 本地示例的 `02_WIFI_STA` 仅示范联网，屏幕参数来自 `03_FactoryProgram`。
- 工厂 `main.c` 的 GPIO4 背光定义不是实际 PWM 接脚；实际 PWM 与原理图均为 GPIO20。
- 工厂例程使用名为 `esp_lcd_sh8601` 的驱动，但本板官方规格为 ST7789V2。
  本适配使用 ESP-IDF ST7789 驱动，颜色/偏移需要实机验收。
- 不复制工厂 LVGL8 的初始化、GPIO7..14 测试、Wi-Fi/BLE 扫描和屏幕随姿态熄灭任务。
