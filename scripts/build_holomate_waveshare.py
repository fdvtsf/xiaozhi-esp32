"""Build the HoloMate Waveshare variant without touching the old board's sdkconfig.

Run with the ESP-IDF Python interpreter after activating ESP-IDF 6.0.2.
config.json is the source of truth shared with scripts/release.py.
"""

import argparse
import ipaddress
import json
import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
BOARD = "holomate-waveshare-s3-lcd-1.3"
CONFIG = ROOT / "main/boards/waveshare" / BOARD / "config.json"
DEFAULT_BUILD = "build-holomate-waveshare-idf6"
DIAGNOSTIC_BUILD = "build-holomate-waveshare-idf6-diagnostics"
TDM_MAP_BUILD = "build-holomate-waveshare-idf6-tdm-map"


def validate_pcm_server(value):
    host, separator, port_text = value.rpartition(":")
    try:
        port = int(port_text)
        ipaddress.IPv4Address(host)
    except ValueError as error:
        raise argparse.ArgumentTypeError("use an IPv4:port receiver address") from error
    if not separator or not 1 <= port <= 65535:
        raise argparse.ArgumentTypeError("use an IPv4:port receiver address")
    return value


def load_settings(audio_diagnostics=False, pcm_server=None, tdm_map=False):
    config = json.loads(CONFIG.read_text(encoding="utf-8"))
    build = next(item for item in config["builds"] if item["name"] == BOARD)
    settings = dict(line.split("=", 1) for line in build["sdkconfig_append"])
    settings["CONFIG_AUDIO_DIAGNOSTICS"] = "y" if audio_diagnostics or pcm_server or tdm_map else "n"
    settings["CONFIG_AUDIO_DIAGNOSTIC_PCM_UDP"] = "y" if pcm_server else "n"
    settings["CONFIG_AUDIO_DIAGNOSTIC_TDM_SLOTS"] = "y" if tdm_map else "n"
    if pcm_server:
        settings["CONFIG_AUDIO_DIAGNOSTIC_UDP_SERVER"] = f'"{pcm_server}"'
    return settings


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
                  for key, value in expected.items() if actual.get(key, "n") != value]
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


def build_environment():
    environment = os.environ.copy()
    # Codex/PowerShell can expose duplicate case variants of Windows' PATH.
    # export.ps1 updates the PowerShell-visible value, while Python may inherit
    # the stale variant.  The wrapper snapshots the verified IDF PATH under a
    # unique name so the idf.py child receives CMake/Ninja/toolchain reliably.
    exported_path = environment.pop("XIAOZHI_IDF_TOOL_PATH", None)
    if exported_path:
        environment["PATH"] = exported_path
    environment.setdefault("IDF_COMPONENT_CHECK_NEW_VERSION", "0")
    return environment


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir",
                        help="Separate build directory beneath this project")
    parser.add_argument("--audio-diagnostics", action="store_true",
                        help="Log PCM levels/AFE progress; use a separate diagnostic build by default")
    parser.add_argument("--audio-pcm-server", type=validate_pcm_server,
                        help="Also stream tagged PCM points to an IPv4:port UDP receiver")
    parser.add_argument("--audio-tdm-map", action="store_true",
                        help="Capture all four ES7210 slots; requires --audio-pcm-server")
    args = parser.parse_args()
    if args.audio_tdm_map and not args.audio_pcm_server:
        parser.error("--audio-tdm-map requires --audio-pcm-server")
    diagnostics = args.audio_diagnostics or args.audio_pcm_server or args.audio_tdm_map
    default_directory = (TDM_MAP_BUILD if args.audio_tdm_map else
                         DIAGNOSTIC_BUILD if diagnostics else DEFAULT_BUILD)
    directory = args.build_dir or default_directory
    build_dir = (ROOT / directory).resolve()
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

    settings = load_settings(args.audio_diagnostics, args.audio_pcm_server, args.audio_tdm_map)
    build_dir.mkdir(parents=True, exist_ok=True)
    defaults = build_dir / "variant.defaults"
    defaults_text = ("# Generated from board config.json; do not edit.\n" +
                     "\n".join(f"{key}={value}" for key, value in settings.items()) + "\n")
    configuration_changed = (not defaults.exists() or
                             defaults.read_text(encoding="utf-8") != defaults_text)
    if configuration_changed:
        defaults.write_text(defaults_text, encoding="utf-8")
    sdkconfig = build_dir / "sdkconfig"
    # These dedicated build directories are owned by this helper. If board
    # defaults evolved or an earlier reconfigure was interrupted, regenerate
    # sdkconfig from the checked-in source of truth instead of requiring manual
    # cleanup on every configuration change.
    if sdkconfig.exists():
        try:
            validate_settings(sdkconfig, settings)
        except RuntimeError as error:
            print(f"Repairing stale generated configuration: {error}")
            sdkconfig.unlink()
            old_sdkconfig = build_dir / "sdkconfig.old"
            if old_sdkconfig.exists():
                old_sdkconfig.unlink()
            configuration_changed = True
    defaults_chain = ";".join(str(path).replace("\\", "/") for path in
                              (ROOT / "sdkconfig.defaults",
                               ROOT / "sdkconfig.defaults.esp32s3", defaults))
    command = [sys.executable, str(Path(idf_path) / "tools/idf.py"),
               "-B", str(build_dir), "-D", "IDF_TARGET=esp32s3",
               "-D", "CMAKE_NINJA_FORCE_RESPONSE_FILE=ON",
               "-D", f"SDKCONFIG={sdkconfig}", "-D", f"SDKCONFIG_DEFAULTS={defaults_chain}",
               "-D", f"BOARD_NAME={BOARD}"]
    # Retain dependency resolution/integrity checks, but skip the optional online
    # "newer versions available" scan when building an already locked project.
    environment = build_environment()
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
