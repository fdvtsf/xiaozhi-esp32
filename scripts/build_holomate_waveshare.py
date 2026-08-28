"""Build the HoloMate Waveshare variant without touching the old board's sdkconfig.

Run with the ESP-IDF Python interpreter after activating ESP-IDF 6.0.2.
config.json is the source of truth shared with scripts/release.py.
"""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
BOARD = "holomate-waveshare-s3-lcd-1.3"
CONFIG = ROOT / "main/boards/waveshare" / BOARD / "config.json"
DEFAULT_BUILD = "build-holomate-waveshare-idf6"


def load_settings():
    config = json.loads(CONFIG.read_text(encoding="utf-8"))
    build = next(item for item in config["builds"] if item["name"] == BOARD)
    return dict(line.split("=", 1) for line in build["sdkconfig_append"])


def read_settings(path):
    settings = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("CONFIG_") and "=" in line:
            key, value = line.split("=", 1)
            settings[key] = value
        elif line.startswith("# CONFIG_") and line.endswith(" is not set"):
            settings[line[2:-len(" is not set")]] = "n"
    return settings


def validate_settings(path, expected):
    actual = read_settings(path)
    expected = {**expected, "CONFIG_IDF_TARGET": '"esp32s3"'}
    mismatches = [f"{key}: expected {value}, got {actual.get(key)}"
                  for key, value in expected.items() if actual.get(key) != value]
    boards = [key for key, value in actual.items()
              if key.startswith("CONFIG_BOARD_TYPE_") and value == "y"]
    if boards != ["CONFIG_BOARD_TYPE_HOLOMATE_WAVESHARE_S3_LCD_1_3"]:
        mismatches.append(f"Unexpected selected boards: {boards}")
    for key in ("CONFIG_ESP_CONSOLE_USB_CDC", "CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG"):
        if actual.get(key) == "y":
            mismatches.append(f"Unsafe native USB console: {key}")
    if mismatches:
        raise RuntimeError("Generated configuration does not match this board:\n" +
                           "\n".join(mismatches) +
                           "\nUse a new --build-dir if this directory contains stale settings.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default=DEFAULT_BUILD,
                        help="Separate build directory beneath this project")
    args = parser.parse_args()
    build_dir = (ROOT / args.build_dir).resolve()
    if not build_dir.is_relative_to(ROOT) or build_dir == ROOT:
        parser.error("--build-dir must stay beneath the project root")
    if not build_dir.name.startswith("build-holomate-waveshare"):
        parser.error("--build-dir name must start with build-holomate-waveshare")

    idf_path = os.environ.get("IDF_PATH")
    if not idf_path or not (Path(idf_path) / "tools/idf.py").is_file():
        parser.error("Activate ESP-IDF 6.0.2 first (export.ps1 / export.sh)")
    # Use the release entry point's version detection / gating as well.
    import release
    if release._detect_idf_version() < (6, 0, 0):
        parser.error("This board variant requires ESP-IDF >= 6.0; tested with 6.0.2")

    settings = load_settings()
    build_dir.mkdir(parents=True, exist_ok=True)
    defaults = build_dir / "variant.defaults"
    defaults_text = ("# Generated from board config.json; do not edit.\n" +
                     "\n".join(f"{key}={value}" for key, value in settings.items()) + "\n")
    configuration_changed = (not defaults.exists() or
                             defaults.read_text(encoding="utf-8") != defaults_text)
    if configuration_changed:
        defaults.write_text(defaults_text, encoding="utf-8")
    sdkconfig = build_dir / "sdkconfig"
    # Reject a stale / foreign configuration before running CMake.
    if sdkconfig.exists():
        validate_settings(sdkconfig, settings)
    defaults_chain = ";".join(str(path).replace("\\", "/") for path in
                              (ROOT / "sdkconfig.defaults",
                               ROOT / "sdkconfig.defaults.esp32s3", defaults))
    command = [sys.executable, str(Path(idf_path) / "tools/idf.py"),
               "-B", str(build_dir), "-D", "IDF_TARGET=esp32s3",
               "-D", "CMAKE_NINJA_FORCE_RESPONSE_FILE=ON",
               "-D", f"SDKCONFIG={sdkconfig}", "-D", f"SDKCONFIG_DEFAULTS={defaults_chain}",
               "-D", f"BOARD_NAME={BOARD}"]
    environment = os.environ.copy()
    # Retain dependency resolution/integrity checks, but skip the optional online
    # "newer versions available" scan when building an already locked project.
    environment.setdefault("IDF_COMPONENT_CHECK_NEW_VERSION", "0")
    if configuration_changed or not (build_dir / "build.ninja").exists():
        subprocess.run(command + ["reconfigure"], cwd=ROOT, env=environment, check=True)
    validate_settings(sdkconfig, settings)
    subprocess.run(command + ["build"], cwd=ROOT, env=environment, check=True)
    validate_settings(sdkconfig, settings)
    firmware = build_dir / "xiaozhi.bin"
    if not firmware.is_file():
        raise RuntimeError(f"Firmware not generated: {firmware}")
    print(f"Validated {BOARD}: {firmware} ({firmware.stat().st_size} bytes)")
    print("Build only. No device was flashed.")


if __name__ == "__main__":
    main()
