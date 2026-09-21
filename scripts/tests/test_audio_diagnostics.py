import importlib.util
import json
import struct
import tempfile
import unittest
import wave
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "receive_pcm", ROOT / "scripts/audio_diagnostics/receive_pcm.py")
receiver = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(receiver)


class AudioDiagnosticsReceiverTests(unittest.TestCase):
    def test_gap_is_split_not_concatenated_or_padded(self):
        entries = [(10, 1, 16000, struct.pack('<hh', 100, 200), 0.1),
                   (12, 1, 16000, struct.pack('<hhh', 300, 400, 500), 0.2)]
        with tempfile.TemporaryDirectory() as directory:
            prefix = Path(directory) / 'capture'
            paths = receiver.write_captures(prefix, {2: entries})
            self.assertEqual(len(paths), 2)
            self.assertFalse((Path(directory) / 'capture-afe-output.wav').exists())
            for path, entry in zip(paths, entries):
                with wave.open(str(path), 'rb') as wav:
                    self.assertEqual(wav.readframes(10), entry[3])
            report = json.loads((Path(directory) / 'capture-manifest.json').read_text())
            stream = report['streams']['afe-output']
            self.assertEqual(stream['gaps'][0]['missing_packets'], 1)
            self.assertIsNone(stream['gaps'][0]['missing_duration_seconds'])
            self.assertEqual(stream['segments'][1]['first_arrival_seconds'], 0.2)

    def test_reorders_deduplicates_and_handles_wrap(self):
        entries = [(0, 1, 16000, b'\x03\x00'),
                   (2**32 - 1, 1, 16000, b'\x02\x00'),
                   (2**32 - 2, 1, 16000, b'\x01\x00'),
                   (0, 1, 16000, b'\x03\x00')]
        with tempfile.TemporaryDirectory() as directory:
            paths = receiver.write_captures(Path(directory) / 'capture', {2: entries})
            self.assertEqual(len(paths), 1)
            with wave.open(str(paths[0]), 'rb') as wav:
                self.assertEqual(wav.readframes(3), struct.pack('<hhh', 1, 2, 3))

    def test_rejects_conflicting_sequence_and_format_change(self):
        with tempfile.TemporaryDirectory() as directory:
            prefix = Path(directory) / 'capture'
            with self.assertRaisesRegex(ValueError, 'Conflicting'):
                receiver.write_captures(prefix, {2: [(1, 1, 16000, b'\x01\x00'),
                                                    (1, 1, 16000, b'\x02\x00')]})
            with self.assertRaisesRegex(ValueError, 'format changed'):
                receiver.write_captures(prefix, {2: [(1, 1, 16000, b'\x01\x00'),
                                                    (2, 1, 24000, b'\x02\x00')]})

    def test_rejects_empty_payload(self):
        with self.assertRaisesRegex(ValueError, 'payload'):
            receiver.parse_packet(receiver.HEADER.pack(b'XADP', 1, 2, 1, 1, 0, 16000, 0))

    def test_writes_reordered_mmr_channels(self):
        payload = struct.pack("<hhhhhh", 101, 201, 301, 102, 202, 302)
        packet = receiver.HEADER.pack(b"XADP", 1, 0, 3, 1, 6, 24000, 6) + payload
        point, channels, rate, sequence, parsed = receiver.parse_packet(packet)
        with tempfile.TemporaryDirectory() as directory:
            prefix = Path(directory) / "capture"
            paths = receiver.write_captures(prefix, {point: [(sequence, channels, rate, parsed)]})
            self.assertEqual(len(paths), 4)
            expected = ((101, 102), (201, 202), (301, 302))
            for channel, samples in enumerate(expected):
                path = prefix.with_name(f"capture-mmr-input-ch{channel}.wav")
                with wave.open(str(path), "rb") as wav:
                    self.assertEqual((wav.getnchannels(), wav.getframerate()), (1, 24000))
                    self.assertEqual(wav.readframes(2), struct.pack("<hh", *samples))

    def test_parse_packet_and_write_channel_zero(self):
        payload = struct.pack("<hhhh", 100, -10, 200, -20)
        packet = receiver.HEADER.pack(b"XADP", 1, 1, 2, 1, 7, 16000, 4) + payload
        point, channels, rate, sequence, parsed = receiver.parse_packet(packet)
        self.assertEqual((point, channels, rate, sequence), (1, 2, 16000, 7))
        self.assertEqual(parsed, payload)
        with tempfile.TemporaryDirectory() as directory:
            prefix = Path(directory) / "capture"
            paths = receiver.write_captures(prefix, {point: [(sequence, channels, rate, parsed)]})
            self.assertEqual(len(paths), 3)
            with wave.open(str(prefix.with_name("capture-afe-input-ch0.wav")), "rb") as wav:
                self.assertEqual((wav.getnchannels(), wav.getframerate()), (1, 16000))
                self.assertEqual(wav.readframes(2), struct.pack("<hh", 100, 200))
            with wave.open(str(prefix.with_name("capture-afe-input-ch1.wav")), "rb") as wav:
                self.assertEqual(wav.readframes(2), struct.pack("<hh", -10, -20))

    def test_writes_all_four_tdm_slots(self):
        payload = struct.pack("<hhhhhhhh", 1, 2, 3, 4, 11, 12, 13, 14)
        packet = receiver.HEADER.pack(b"XADP", 1, 3, 4, 1, 8, 24000, 8) + payload
        point, channels, rate, sequence, parsed = receiver.parse_packet(packet)
        with tempfile.TemporaryDirectory() as directory:
            prefix = Path(directory) / "capture"
            paths = receiver.write_captures(prefix, {point: [(sequence, channels, rate, parsed)]})
            self.assertEqual(len(paths), 5)
            expected_channels = ((1, 11), (2, 12), (3, 13), (4, 14))
            for channel, expected in enumerate(expected_channels):
                channel_path = prefix.with_name(f"capture-tdm-slots-ch{channel}.wav")
                with wave.open(str(channel_path), "rb") as wav:
                    self.assertEqual(wav.readframes(2), struct.pack("<hh", *expected))

    def test_rejects_malformed_packet(self):
        with self.assertRaisesRegex(ValueError, "payload"):
            receiver.parse_packet(
                receiver.HEADER.pack(b"XADP", 1, 2, 1, 1, 0, 16000, 2) + b"\0\0")


if __name__ == "__main__":
    unittest.main()
