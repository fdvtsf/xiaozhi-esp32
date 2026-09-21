import importlib.util
import json
from pathlib import Path
import re
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "build_holomate_waveshare", ROOT / "scripts/build_holomate_waveshare.py")
builder = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(builder)
BOARD_DIR = builder.CONFIG.parent


class HoloMateWaveshareTests(unittest.TestCase):
    def test_temperature_is_cached_and_outside_audio_pipeline(self):
        source = (BOARD_DIR / "holomate_waveshare_board.cc").read_text(encoding="utf-8")
        self.assertIn("TEMPERATURE_SENSOR_CONFIG_DEFAULT(50, 125)", source)
        self.assertIn("esp_timer_start_periodic(temperature_timer_, 10 * 1000 * 1000)", source)
        self.assertIn("temperature_sensor_disable(temperature_sensor_)", source)
        self.assertIn("std::atomic<float> chip_temperature_", source)
        getter = source.split("bool GetTemperature(float& celsius) override {", 1)[1].split("}", 1)[0]
        self.assertIn("chip_temperature_.load()", getter)
        self.assertIn("std::isfinite(celsius)", getter)
        self.assertNotIn("temperature_sensor_get_celsius", getter)
        self.assertNotIn("ESP_ERROR_CHECK(temperature_sensor", source)

    def test_active_thermal_protection_preserves_normal_performance(self):
        source = (BOARD_DIR / "holomate_waveshare_board.cc").read_text(encoding="utf-8")
        settings = builder.load_settings()
        self.assertEqual(settings["CONFIG_PM_ENABLE"], "y")
        self.assertIn("kThermalThrottleOnC = 80.0f", source)
        self.assertIn("kThermalCriticalOnC = 90.0f", source)
        self.assertIn("kThermalShutdownC = 100.0f", source)
        self.assertIn("kThermalShutdownSamples = 3", source)
        self.assertIn("esp_pm_configure(&pm_config)", source)
        self.assertIn("esp_deep_sleep_start()", source)
        self.assertRegex(source, r"case ThermalProtectionLevel::Normal:[\s\S]*?return 240;")

    def test_wake_word_invocation_uses_server_greeting(self):
        settings = builder.load_settings()
        self.assertEqual(settings["CONFIG_SEND_WAKE_WORD_DATA"], "y")

    def test_diagnostics_only_changes_logging_setting(self):
        normal = builder.load_settings()
        diagnostic = builder.load_settings(audio_diagnostics=True)
        self.assertEqual({key for key in normal if normal[key] != diagnostic[key]},
                         {"CONFIG_AUDIO_DIAGNOSTICS"})
        self.assertEqual(normal["CONFIG_AUDIO_DIAGNOSTICS"], "n")
        self.assertEqual(diagnostic["CONFIG_AUDIO_DIAGNOSTICS"], "y")
        self.assertEqual(normal["CONFIG_AUDIO_DIAGNOSTIC_PCM_UDP"], "n")
        self.assertEqual(normal["CONFIG_AUDIO_DIAGNOSTIC_TDM_SLOTS"], "n")
        self.assertNotEqual(builder.DEFAULT_BUILD, builder.DIAGNOSTIC_BUILD)

    def test_pcm_diagnostics_enables_master_and_validates_server(self):
        settings = builder.load_settings(pcm_server="192.168.1.7:8010")
        self.assertEqual(settings["CONFIG_AUDIO_DIAGNOSTICS"], "y")
        self.assertEqual(settings["CONFIG_AUDIO_DIAGNOSTIC_PCM_UDP"], "y")
        self.assertEqual(settings["CONFIG_AUDIO_DIAGNOSTIC_UDP_SERVER"],
                         '"192.168.1.7:8010"')
        self.assertEqual(settings["CONFIG_AUDIO_DIAGNOSTIC_TDM_SLOTS"], "n")
        self.assertEqual(builder.validate_pcm_server("10.0.0.1:65535"), "10.0.0.1:65535")
        with self.assertRaisesRegex(Exception, "IPv4:port"):
            builder.validate_pcm_server("host:0")

    def test_diagnostics_uses_isolated_build_directory(self):
        expected = {**builder.load_settings(True), "CONFIG_IDF_TARGET": '"esp32s3"'}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            idf = root / "idf"
            (idf / "tools").mkdir(parents=True)
            (idf / "tools/idf.py").touch()
            build = root / builder.DIAGNOSTIC_BUILD

            def fake_idf(command, **kwargs):
                self.assertEqual(command[command.index("-B") + 1], str(build))
                if command[-1] == "reconfigure":
                    (build / "sdkconfig").write_text(
                        "\n".join(f"{key}={value}" for key, value in expected.items()),
                        encoding="utf-8")
                    (build / "build.ninja").touch()
                elif command[-1] == "build":
                    (build / "xiaozhi.bin").write_bytes(b"diagnostic firmware")

            with mock.patch.object(builder, "ROOT", root), \
                    mock.patch.dict(builder.os.environ, {"IDF_PATH": str(idf)}), \
                    mock.patch.object(builder.sys, "argv", ["builder", "--audio-diagnostics"]), \
                    mock.patch.dict(builder.sys.modules, {
                        "release": SimpleNamespace(_detect_idf_version=lambda: (6, 0, 2))}), \
                    mock.patch.object(builder.subprocess, "run", side_effect=fake_idf):
                builder.main()
            self.assertFalse((root / builder.DEFAULT_BUILD).exists())
            # A diagnostic sdkconfig is still distinguishable from normal firmware.
            with self.assertRaisesRegex(RuntimeError, "CONFIG_AUDIO_DIAGNOSTICS"):
                builder.validate_settings(build / "sdkconfig", builder.load_settings())

    def test_stale_generated_config_is_repaired_in_owned_build_directory(self):
        expected = {**builder.load_settings(), "CONFIG_IDF_TARGET": '"esp32s3"'}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            idf = root / "idf"
            (idf / "tools").mkdir(parents=True)
            (idf / "tools/idf.py").touch()
            build = root / builder.DEFAULT_BUILD
            build.mkdir()
            (build / "variant.defaults").write_text("stale\n", encoding="utf-8")
            (build / "sdkconfig").write_text(
                'CONFIG_IDF_TARGET="esp32s3"\nCONFIG_BOARD_TYPE_HOLOMATE=y\n',
                encoding="utf-8")
            (build / "sdkconfig.old").write_text("also stale\n", encoding="utf-8")

            def fake_idf(command, **kwargs):
                if command[-1] == "reconfigure":
                    (build / "sdkconfig").write_text(
                        "\n".join(f"{key}={value}" for key, value in expected.items()),
                        encoding="utf-8")
                    (build / "build.ninja").touch()
                elif command[-1] == "build":
                    (build / "xiaozhi.bin").write_bytes(b"repaired firmware")

            with mock.patch.object(builder, "ROOT", root), \
                    mock.patch.dict(builder.os.environ, {"IDF_PATH": str(idf)}), \
                    mock.patch.object(builder.sys, "argv", ["builder"]), \
                    mock.patch.dict(builder.sys.modules, {
                        "release": SimpleNamespace(_detect_idf_version=lambda: (6, 0, 2))}), \
                    mock.patch.object(builder.subprocess, "run", side_effect=fake_idf) as run:
                builder.main()

            self.assertEqual([call.args[0][-1] for call in run.call_args_list],
                             ["reconfigure", "build"])
            self.assertFalse((build / "sdkconfig.old").exists())
            builder.validate_settings(build / "sdkconfig", builder.load_settings())

    def test_tdm_mapping_has_dedicated_build(self):
        settings = builder.load_settings(pcm_server="192.168.1.7:8010", tdm_map=True)
        self.assertEqual(settings["CONFIG_AUDIO_DIAGNOSTICS"], "y")
        self.assertEqual(settings["CONFIG_AUDIO_DIAGNOSTIC_PCM_UDP"], "y")
        self.assertEqual(settings["CONFIG_AUDIO_DIAGNOSTIC_TDM_SLOTS"], "y")
        self.assertNotEqual(builder.TDM_MAP_BUILD, builder.DIAGNOSTIC_BUILD)

    def test_exported_idf_path_wins_over_stale_python_path(self):
        with mock.patch.dict(builder.os.environ, {
                "PATH": "stale-path",
                "XIAOZHI_IDF_TOOL_PATH": "verified-idf-path",
        }, clear=True):
            environment = builder.build_environment()
        self.assertEqual(environment["PATH"], "verified-idf-path")
        self.assertNotIn("XIAOZHI_IDF_TOOL_PATH", environment)
        self.assertEqual(environment["IDF_COMPONENT_CHECK_NEW_VERSION"], "0")

    def test_incremental_build_keeps_defaults_and_skips_forced_reconfigure(self):
        expected = {**builder.load_settings(), "CONFIG_IDF_TARGET": '"esp32s3"'}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            idf = root / "idf"
            (idf / "tools").mkdir(parents=True)
            (idf / "tools/idf.py").touch()
            build = root / builder.DEFAULT_BUILD

            def fake_idf(command, **kwargs):
                if command[-1] == "reconfigure":
                    (build / "sdkconfig").write_text(
                        "\n".join(f"{key}={value}" for key, value in expected.items()),
                        encoding="utf-8")
                    (build / "build.ninja").touch()
                elif command[-1] == "build":
                    (build / "xiaozhi.bin").write_bytes(b"test firmware")

            with mock.patch.object(builder, "ROOT", root), \
                    mock.patch.dict(builder.os.environ, {"IDF_PATH": str(idf)}), \
                    mock.patch.object(builder.sys, "argv", ["build_holomate_waveshare.py"]), \
                    mock.patch.dict(builder.sys.modules, {
                        "release": SimpleNamespace(_detect_idf_version=lambda: (6, 0, 2))}), \
                    mock.patch.object(builder.subprocess, "run", side_effect=fake_idf) as run:
                builder.main()
                self.assertEqual([call.args[0][-1] for call in run.call_args_list],
                                 ["reconfigure", "build"])
                timestamp = (build / "variant.defaults").stat().st_mtime_ns
                run.reset_mock()
                builder.main()
                self.assertEqual([call.args[0][-1] for call in run.call_args_list], ["build"])
                self.assertEqual((build / "variant.defaults").stat().st_mtime_ns, timestamp)

    def test_retained_holomate_defaults(self):
        old = builder.read_settings(ROOT / "sdkconfig.holomate.defaults")
        new = builder.load_settings()
        for key, value in old.items():
            if (not key.startswith("CONFIG_BOARD_TYPE_") and
                    key != "CONFIG_SEND_WAKE_WORD_DATA"):
                self.assertEqual(new[key], value, key)
        self.assertEqual(new["CONFIG_USE_AUDIO_PROCESSOR"], "y")
        self.assertEqual(new["CONFIG_ESP_CONSOLE_UART_DEFAULT"], "y")
        self.assertEqual(new["CONFIG_ESP_CONSOLE_SECONDARY_NONE"], "y")
        self.assertEqual(new["CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG"], "n")

    def test_pin_map_and_audio_baseline(self):
        header = (BOARD_DIR / "config.h").read_text(encoding="utf-8")
        pins = dict((key, int(value)) for key, value in
                    re.findall(r"#define (\w+) GPIO_NUM_(\d+)\b", header))
        expected = {
            "AUDIO_CODEC_PA_PIN": 14, "AUDIO_CODEC_I2C_SDA_PIN": 13,
            "AUDIO_CODEC_I2C_SCL_PIN": 12, "AUDIO_I2S_GPIO_MCLK": 11,
            # DI / DO labels are from the ESP32 development board's perspective.
            "AUDIO_I2S_GPIO_BCLK": 10, "AUDIO_I2S_GPIO_DIN": 7,
            "AUDIO_I2S_GPIO_WS": 8, "AUDIO_I2S_GPIO_DOUT": 9,
            "DISPLAY_MOSI_PIN": 41, "DISPLAY_CLK_PIN": 40,
            "DISPLAY_CS_PIN": 39, "DISPLAY_DC_PIN": 38,
            "DISPLAY_RST_PIN": 42, "DISPLAY_BACKLIGHT_PIN": 20,
        }
        self.assertEqual(pins, expected)
        self.assertEqual(len(pins.values()), len(set(pins.values())))
        self.assertIn("#define AUDIO_INPUT_REFERENCE true", header)
        self.assertIn("#define AUDIO_INPUT_GAIN_DB 36.0f", header)
        self.assertIn("#define AUDIO_INPUT_MIC1_SLOT 0", header)
        self.assertIn("#define AUDIO_INPUT_MIC2_SLOT 2", header)
        self.assertIn("#define AUDIO_INPUT_REFERENCE_SLOT 1", header)
        self.assertIn("#define DISPLAY_MIRROR_X false", header)
        self.assertIn("#define DISPLAY_MIRROR_Y true", header)
        self.assertIn("#define DISPLAY_SWAP_XY true", header)
        self.assertIn("#define DISPLAY_OFFSET_X 80", header)
        self.assertIn("#define DISPLAY_OFFSET_Y 0", header)
        source = (BOARD_DIR / "holomate_waveshare_board.cc").read_text(encoding="utf-8")
        self.assertEqual(source.count("DECLARE_BOARD("), 1)
        self.assertIn("PowerSaveLevel::PERFORMANCE", source)
        self.assertIn("BoxAudioCodecInputLayout{AUDIO_INPUT_MIC1_SLOT, AUDIO_INPUT_MIC2_SLOT,",
                      source)

    def test_es7210_mmr_layout_is_extracted_not_only_declared(self):
        codec_header = (ROOT / "main/audio/codecs/box_audio_codec.h").read_text(encoding="utf-8")
        codec_source = (ROOT / "main/audio/codecs/box_audio_codec.cc").read_text(encoding="utf-8")
        afe_source = (ROOT / "main/audio/engines/afe_audio_engine.cc").read_text(encoding="utf-8")
        self.assertIn("int mic2_slot = -1", codec_header)
        self.assertIn("dest[output++] = slots[input_layout_.mic1_slot]", codec_source)
        self.assertIn("dest[output++] = slots[input_layout_.mic2_slot]", codec_source)
        self.assertIn("dest[output] = slots[input_layout_.reference_slot]", codec_source)
        self.assertIn("kMicSelectionBySlot", codec_source)
        self.assertIn('input_format.push_back(\'M\')', afe_source)
        self.assertIn('input_format.push_back(\'R\')', afe_source)
        self.assertIn("AFE input layout: input_channels=%d", afe_source)
        self.assertIn('const bool dual_mic_with_reference = input_format == "MMR"', afe_source)
        self.assertIn("dual_mic_with_reference ? AFE_TYPE_FD : AFE_TYPE_VC", afe_source)
        self.assertIn("dual_mic_with_reference ? AFE_MODE_LOW_COST : AFE_MODE_HIGH_PERF",
                      afe_source)

    def test_expression_only_fullscreen_display(self):
        source = (BOARD_DIR / "holomate_waveshare_board.cc").read_text(encoding="utf-8")
        self.assertIn("class HoloMateWaveshareDisplay : public SpiLcdDisplay", source)
        self.assertIn("SetHideSubtitle(true)", source)
        self.assertIn("void SetChatMessage(const char* role, const char* content) override", source)
        self.assertIn("void SetStatus(const char* status) override", source)
        self.assertIn("void UpdateStatusBar(bool update_all = false) override", source)
        for member in ("top_bar_", "status_bar_", "bottom_bar_", "emoji_label_",
                       "network_label_", "mute_label_", "battery_label_",
                       "low_battery_popup_"):
            self.assertIn(member, source)
        self.assertIn("lv_obj_set_size(emoji_box_, LV_HOR_RES, LV_VER_RES)", source)
        self.assertIn("lv_obj_set_size(emoji_image_, LV_HOR_RES, LV_VER_RES)", source)
        self.assertIn("lv_image_set_inner_align(emoji_image_, LV_IMAGE_ALIGN_STRETCH)", source)
        self.assertIn("display_ = new HoloMateWaveshareDisplay", source)

    def test_release_board_identity(self):
        config = json.loads(builder.CONFIG.read_text(encoding="utf-8"))
        self.assertEqual(config["manufacturer"], "waveshare")
        self.assertEqual(config["target"], "esp32s3")
        cmake = (ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn(f'set(BOARD_TYPE "{builder.BOARD}")', cmake)
        kconfig = (ROOT / "main/Kconfig.projbuild").read_text(encoding="utf-8")
        symbol = "BOARD_TYPE_HOLOMATE_WAVESHARE_S3_LCD_1_3"
        self.assertIn(f"config {symbol}", kconfig)
        aec = kconfig.split("config USE_DEVICE_AEC", 1)[1].split("config USE_SERVER_AEC", 1)[0]
        self.assertIn(symbol, aec)

    def test_generated_config_validation_rejects_wrong_board_and_usb(self):
        expected = builder.load_settings()
        actual = {**expected, "CONFIG_IDF_TARGET": '"esp32s3"'}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "sdkconfig"

            def write_settings():
                path.write_text("\n".join(f"# {key} is not set" if value == "n" else
                                          f"{key}={value}" for key, value in actual.items()),
                                encoding="utf-8")

            write_settings()
            builder.validate_settings(path, expected)
            actual["CONFIG_BOARD_TYPE_HOLOMATE"] = "y"
            write_settings()
            with self.assertRaisesRegex(RuntimeError, "Unexpected selected boards"):
                builder.validate_settings(path, expected)
            del actual["CONFIG_BOARD_TYPE_HOLOMATE"]
            actual["CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG"] = "y"
            write_settings()
            with self.assertRaisesRegex(RuntimeError, "Unsafe native USB"):
                builder.validate_settings(path, expected)


if __name__ == "__main__":
    unittest.main()
