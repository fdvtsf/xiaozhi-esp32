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
            if not key.startswith("CONFIG_BOARD_TYPE_"):
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
        self.assertIn("#define AUDIO_INPUT_GAIN_DB 37.5f", header)
        self.assertIn("#define DISPLAY_OFFSET_Y 80", header)
        source = (BOARD_DIR / "holomate_waveshare_board.cc").read_text(encoding="utf-8")
        self.assertEqual(source.count("DECLARE_BOARD("), 1)
        self.assertIn("PowerSaveLevel::PERFORMANCE", source)

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
