#!/usr/bin/env python3
"""Receive XiaoZhi multi-tap audio diagnostics and write WAV/JSONL files."""

import argparse
import datetime
import json
import pathlib
import socket
import struct
import wave


MAGIC = 0x47444158
VERSION = 1
HEADER = struct.Struct("<IBBBBIIQIHHII")
EVENT = struct.Struct("<HHi")

KIND_PCM = 1
KIND_EVENT = 2

STREAM_NAMES = {
    1: "codec_input",
    2: "afe_output",
    3: "uplink_pcm",
    4: "playback_pcm",
    5: "events",
}

EVENT_NAMES = {
    1: "aec_state",
    2: "vad_state",
    3: "wake_word_detected",
    4: "wake_word_state",
    5: "voice_processing_state",
}


class Session:
    def __init__(self, root: pathlib.Path, session_id: int):
        self.path = root / f"session_{session_id:08x}"
        self.path.mkdir(parents=True, exist_ok=True)
        self.wav_files = {}
        self.expected_sequences = {}
        self.first_timestamps = {}
        self.events = (self.path / "events.jsonl").open("a", encoding="utf-8")
        self.packet_loss = 0

    def write_pcm(self, stream, sequence, timestamp_us, sample_rate, channels,
                  bits_per_sample, frames, payload):
        if bits_per_sample != 16:
            print(f"Ignoring unsupported {bits_per_sample}-bit PCM stream {stream}")
            return

        key = (stream, sample_rate, channels)
        writer = self.wav_files.get(key)
        if writer is None:
            name = STREAM_NAMES.get(stream, f"stream_{stream}")
            duplicate_stream = any(existing[0] == stream for existing in self.wav_files)
            suffix = f"_{sample_rate}hz_{channels}ch" if duplicate_stream else ""
            writer = wave.open(str(self.path / f"{name}{suffix}.wav"), "wb")
            writer.setnchannels(channels)
            writer.setsampwidth(bits_per_sample // 8)
            writer.setframerate(sample_rate)
            self.wav_files[key] = writer
            self.first_timestamps[STREAM_NAMES.get(stream, str(stream))] = timestamp_us

        expected = self.expected_sequences.get(stream)
        if expected is not None and sequence != expected:
            missing = (sequence - expected) & 0xFFFFFFFF
            if missing < 0x80000000:
                self.packet_loss += missing
                print(f"Stream {stream}: expected packet {expected}, got {sequence} "
                      f"({missing} missing)")
        self.expected_sequences[stream] = (sequence + 1) & 0xFFFFFFFF

        expected_bytes = frames * channels * (bits_per_sample // 8)
        if len(payload) != expected_bytes:
            print(f"Ignoring malformed PCM packet: expected {expected_bytes} bytes, "
                  f"got {len(payload)}")
            return
        writer.writeframesraw(payload)

    def write_event(self, sequence, timestamp_us, payload):
        if len(payload) != EVENT.size:
            return
        event_id, _, value = EVENT.unpack(payload)
        record = {
            "timestamp_us": timestamp_us,
            "sequence": sequence,
            "event": EVENT_NAMES.get(event_id, f"event_{event_id}"),
            "value": value,
        }
        self.events.write(json.dumps(record, ensure_ascii=False) + "\n")
        self.events.flush()
        print(f"EVENT {record['event']}={value} at {timestamp_us}")

    def close(self):
        for writer in self.wav_files.values():
            writer.close()
        self.events.close()
        (self.path / "summary.json").write_text(
            json.dumps({
                "packet_loss": self.packet_loss,
                "first_timestamp_us": self.first_timestamps,
            }, indent=2),
            encoding="utf-8",
        )


def parse_args():
    parser = argparse.ArgumentParser(
        description="Receive XiaoZhi audio diagnostic UDP packets"
    )
    parser.add_argument("--bind", default="0.0.0.0", help="Local IPv4 address")
    parser.add_argument("--port", type=int, default=8000, help="Local UDP port")
    parser.add_argument(
        "--output",
        type=pathlib.Path,
        default=pathlib.Path("audio-diagnostics"),
        help="Output directory",
    )
    return parser.parse_args()


def main():
    args = parse_args()
    timestamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    output = args.output / timestamp
    output.mkdir(parents=True, exist_ok=True)

    receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    receiver.bind((args.bind, args.port))
    receiver.settimeout(1.0)
    sessions = {}
    print(f"Listening on {args.bind}:{args.port}; writing to {output.resolve()}")
    print("Press Ctrl+C to stop and finalize WAV files")

    try:
        while True:
            try:
                packet, _ = receiver.recvfrom(65535)
            except socket.timeout:
                continue
            if len(packet) < HEADER.size:
                continue

            (magic, version, kind, stream, _flags, session_id, sequence,
             timestamp_us, sample_rate, channels, bits_per_sample,
             samples_per_channel, payload_bytes) = HEADER.unpack_from(packet)
            if magic != MAGIC or version != VERSION:
                continue
            payload = packet[HEADER.size:]
            if payload_bytes != len(payload):
                continue

            session = sessions.get(session_id)
            if session is None:
                session = Session(output, session_id)
                sessions[session_id] = session
                print(f"Started session {session_id:08x}")

            if kind == KIND_PCM:
                session.write_pcm(stream, sequence, timestamp_us, sample_rate,
                                  channels, bits_per_sample, samples_per_channel,
                                  payload)
            elif kind == KIND_EVENT:
                session.write_event(sequence, timestamp_us, payload)
    except KeyboardInterrupt:
        print("\nStopping receiver")
    finally:
        receiver.close()
        for session in sessions.values():
            session.close()


if __name__ == "__main__":
    main()
