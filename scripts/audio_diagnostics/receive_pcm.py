#!/usr/bin/env python3
"""Receive tagged XiaoZhi audio diagnostic UDP packets and write WAV files."""

import argparse
import array
import json
import socket
import struct
import sys
import time
import wave
from collections import defaultdict
from pathlib import Path


HEADER = struct.Struct("!4sBBBBIII")
POINT_NAMES = {0: "raw-codec", 1: "afe-input", 2: "afe-output", 3: "tdm-slots"}


def point_name(point, channels):
    # The production HoloMate ES7210 path is reordered to Mic1/Mic2/Ref before
    # this point. Keep the legacy name for one/two-channel captures.
    if point == 0 and channels == 3:
        return "mmr-input"
    return POINT_NAMES[point]


def parse_packet(packet):
    if len(packet) < HEADER.size:
        raise ValueError("short packet")
    magic, version, point, channels, flags, sequence, rate, samples = HEADER.unpack_from(packet)
    payload = packet[HEADER.size:]
    if magic != b"XADP" or version != 1:
        raise ValueError("unsupported packet")
    if point not in POINT_NAMES or channels == 0 or rate == 0:
        raise ValueError("invalid metadata")
    if flags & 1 == 0:
        raise ValueError("PCM is not little-endian signed 16-bit")
    if samples == 0 or samples * 2 != len(payload) or samples % channels:
        raise ValueError("invalid PCM payload length")
    return point, channels, rate, sequence, payload


def mono_channel(payload, channels, channel):
    if channels == 1:
        return payload
    samples = array.array("h")
    samples.frombytes(payload)
    if sys.byteorder != "little":
        samples.byteswap()
    mono = array.array("h", samples[channel::channels])
    if sys.byteorder != "little":
        mono.byteswap()
    return mono.tobytes()


def write_wav(path, payload, channels, rate):
    with wave.open(str(path), "wb") as output:
        output.setnchannels(channels)
        output.setsampwidth(2)
        output.setframerate(rate)
        output.writeframes(payload)


def write_captures(output_prefix, packets):
    written = []
    report = {"protocol": 1, "cross_node_alignment": "unknown",
              "note": "Separate WAVs at sequence gaps. No padding or gap concatenation. "
                      "Receive times are host arrival times, not sample timestamps. "
                      "Missing durations and samples lost before packetization are unknown.",
              "streams": {}}
    for point, entries in sorted(packets.items()):
        if not entries:
            continue
        # Signed modular order supports reordering and uint32 wrap in short captures.
        anchor = entries[0][0]
        entries = sorted(entries, key=lambda item: ((item[0] - anchor + 2**31) % 2**32) - 2**31)
        channels, rate = entries[0][1:3]
        if any(item[1:3] != (channels, rate) for item in entries):
            raise ValueError("Stream format changed; restart the capture")
        unique = {}
        duplicates = 0
        for item in entries:
            if item[0] in unique:
                if unique[item[0]][3] != item[3]:
                    raise ValueError("Conflicting sequence; device may have restarted")
                duplicates += 1
            else:
                unique[item[0]] = item
        valid = list(unique.values())
        runs = []
        gaps = []
        for item in valid:
            if not runs:
                runs.append([])
            elif (item[0] - runs[-1][-1][0]) % 2**32 != 1:
                previous = runs[-1][-1][0]
                gaps.append({"after_sequence": previous, "before_sequence": item[0],
                             "missing_packets": (item[0] - previous - 1) % 2**32,
                             "missing_duration_seconds": None})
                runs.append([])
            runs[-1].append(item)
        name = point_name(point, channels)
        stream = {"rate": rate, "channels": channels, "duplicates": duplicates,
                  "gaps": gaps, "segments": [],
                  "packets": [{"sequence": item[0], "frames": len(item[3]) // (2 * channels),
                               "arrival_seconds": item[4] if len(item) > 4 else None}
                              for item in valid]}
        for index, run in enumerate(runs, 1):
            payload = b"".join(item[3] for item in run)
            suffix = f"-part{index:03d}" if len(runs) > 1 else ""
            stem = f"{output_prefix.name}-{name}{suffix}"
            path = output_prefix.with_name(stem + ".wav")
            write_wav(path, payload, channels, rate)
            written.append(path)
            if channels > 1:
                for channel in range(channels):
                    channel_path = output_prefix.with_name(f"{stem}-ch{channel}.wav")
                    write_wav(channel_path, mono_channel(payload, channels, channel), 1, rate)
                    written.append(channel_path)
            stream["segments"].append({"file": path.name, "first_sequence": run[0][0],
                                       "last_sequence": run[-1][0],
                                       "duration_seconds": len(payload) / (2 * channels * rate),
                                       "first_arrival_seconds": run[0][4] if len(run[0]) > 4 else None})
        report["streams"][name] = stream
        seconds = sum(len(item[3]) for item in valid) / (2 * channels * rate)
        print(f"{name}: {seconds:.2f}s, {rate} Hz, {channels} ch, "
              f"packets={len(valid)}, missing={sum(g['missing_packets'] for g in gaps)}, "
              f"continuous_segments={len(runs)}, duplicates={duplicates}")
    manifest = output_prefix.with_name(output_prefix.name + "-manifest.json")
    manifest.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"Manifest: {manifest.resolve()}")
    return written


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8010)
    parser.add_argument("--duration", type=float, default=15)
    parser.add_argument("--output", type=Path, default=Path("audio-diagnostic"))
    args = parser.parse_args()
    if not 1 <= args.port <= 65535 or args.duration <= 0:
        parser.error("port and duration must be positive")
    if list(args.output.parent.glob(args.output.name + "-*.wav")) or args.output.with_name(
            args.output.name + "-manifest.json").exists():
        parser.error("Output prefix already exists; choose a new prefix to preserve evidence")

    receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    receiver.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024 * 1024)
    receiver.bind(("0.0.0.0", args.port))
    receiver.settimeout(0.25)
    packets = defaultdict(list)
    invalid = 0
    started = time.monotonic()
    deadline = started + args.duration
    print(f"Listening on UDP {args.port} for {args.duration:g}s; speak the wake word now", flush=True)
    try:
        while time.monotonic() < deadline:
            try:
                packet, _ = receiver.recvfrom(2048)
            except socket.timeout:
                continue
            try:
                point, channels, rate, sequence, payload = parse_packet(packet)
            except ValueError:
                invalid += 1
                continue
            packets[point].append((sequence, channels, rate, payload, time.monotonic() - started))
    finally:
        receiver.close()

    if not packets:
        raise SystemExit("No diagnostic PCM received; check firmware target IP and Windows Firewall")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    written = write_captures(args.output, packets)
    print(f"invalid packets={invalid}")
    for path in written:
        print(path.resolve())


if __name__ == "__main__":
    main()
