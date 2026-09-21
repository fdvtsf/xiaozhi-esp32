#!/usr/bin/env python3
"""Compare XiaoZhi codec, ES7210 TDM-slot, and AFE diagnostic audio."""

import argparse
import wave
from pathlib import Path

import numpy as np


def read_wav(path: Path):
    with wave.open(str(path), "rb") as source:
        channels = source.getnchannels()
        rate = source.getframerate()
        data = np.frombuffer(source.readframes(source.getnframes()), dtype="<i2")
    return rate, data.reshape(-1, channels).astype(np.float64) / 32768.0


def codec_capture_path(prefix: Path):
    mmr = prefix.with_name(prefix.name + "-mmr-input.wav")
    if mmr.exists():
        return mmr
    return prefix.with_name(prefix.name + "-raw-codec.wav")


def db(value):
    return 20.0 * np.log10(max(float(value), 1e-12))


def frame_rms(signal, rate, frame_ms=20):
    size = max(1, rate * frame_ms // 1000)
    usable = len(signal) // size * size
    frames = signal[:usable].reshape(-1, size)
    return np.sqrt(np.mean(frames * frames, axis=1)), size


def speech_regions(signal, rate, start_seconds, threshold_override=None):
    rms, frame_size = frame_rms(signal, rate)
    rms_db = 20.0 * np.log10(np.maximum(rms, 1e-12))
    start_frame = min(len(rms_db), int(start_seconds * rate / frame_size))
    tail = rms_db[start_frame:]
    noise = float(np.percentile(tail, 25)) if len(tail) else -90.0
    threshold = (threshold_override if threshold_override is not None
                 else max(-46.0, noise + 12.0))
    active = np.flatnonzero(rms_db >= threshold)
    regions = []
    if len(active):
        first = previous = int(active[0])
        max_gap = int(0.30 * rate / frame_size)
        for current in map(int, active[1:]):
            if current - previous > max_gap:
                regions.append((first, previous + 1))
                first = current
            previous = current
        regions.append((first, previous + 1))
    result = []
    for first, last in regions:
        begin = first * frame_size / rate
        end = last * frame_size / rate
        if end >= start_seconds and end - begin >= 0.25:
            result.append((begin, end))
    return noise, threshold, result


def band_db(signal, rate, low, high):
    if len(signal) < 32:
        return -120.0
    windowed = signal * np.hanning(len(signal))
    spectrum = np.fft.rfft(windowed)
    frequencies = np.fft.rfftfreq(len(signal), 1.0 / rate)
    selected = (frequencies >= low) & (frequencies < high)
    return db(np.sqrt(np.mean(np.abs(spectrum[selected]) ** 2)) / max(len(signal), 1))


def segment(signal, rate, begin, end):
    return signal[int(begin * rate):min(len(signal), int(end * rate))]


def signal_level(signal):
    return db(np.sqrt(np.mean(signal * signal)))


def print_tdm_report(prefix, raw, raw_rate, regions):
    path = prefix.with_name(prefix.name + "-tdm-slots.wav")
    if not path.exists():
        return
    tdm_rate, tdm = read_wav(path)
    if tdm.shape[1] != 4:
        raise RuntimeError(f"Expected four TDM slots, found {tdm.shape[1]} in {path}")

    print("\nES7210 four-slot summary:")
    print(" slot  overall   noise    peak  active")
    for channel in range(4):
        rms, _ = frame_rms(tdm[:, channel], tdm_rate)
        frame_db = 20.0 * np.log10(np.maximum(rms, 1e-12))
        overall = signal_level(tdm[:, channel])
        noise = float(np.percentile(frame_db, 25))
        peak = db(np.max(np.abs(tdm[:, channel])))
        active = 100.0 * float(np.mean(frame_db >= max(-46.0, noise + 12.0)))
        print(f" ch{channel} {overall:8.1f} {noise:7.1f} {peak:7.1f} {active:6.1f}%")

    count = min(len(raw), len(tdm))
    if raw_rate == tdm_rate and count:
        combined = np.concatenate((raw[:count], tdm[:count]), axis=1)
        correlations = np.corrcoef(combined, rowvar=False)[:raw.shape[1], raw.shape[1]:]
        print("\nZero-lag correlation (raw codec rows -> TDM slot columns):")
        print("             ch0     ch1     ch2     ch3")
        for channel, values in enumerate(correlations):
            formatted = " ".join(f"{value:7.3f}" for value in values)
            print(f" raw-ch{channel} {formatted}")

    if regions:
        print("\nSpeech-region level by TDM slot (dBFS):")
        print(" idx       time       ch0    ch1    ch2    ch3")
        for index, (begin, end) in enumerate(regions, 1):
            levels = [signal_level(segment(tdm[:, channel], tdm_rate, begin, end))
                      for channel in range(4)]
            print(f"{index:4d} {begin:7.2f}-{end:7.2f} " +
                  " ".join(f"{level:6.1f}" for level in levels))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prefix", type=Path)
    parser.add_argument("--last", type=float, default=90.0,
                        help="Only detect speech in the final N seconds")
    parser.add_argument("--threshold", type=float,
                        help="Override the raw microphone frame threshold in dBFS")
    parser.add_argument("--tdm", action="store_true",
                        help="Require and report the four-slot ES7210 diagnostic capture")
    args = parser.parse_args()

    manifest = args.prefix.with_name(args.prefix.name + "-manifest.json")
    if manifest.exists():
        parser.error("This capture uses separate continuous segments with unknown cross-node "
                     "alignment. Align matching speech and exclude gap boundaries before "
                     "comparison; equal WAV times are not aligned sample times. See " + str(manifest))
    print("WARNING: legacy capture has no gap/timing manifest. Same-time comparisons below "
          "are unverified and cannot establish AFE attenuation or distortion.")

    raw_rate, raw = read_wav(codec_capture_path(args.prefix))
    input_rate, afe_input = read_wav(args.prefix.with_name(args.prefix.name + "-afe-input.wav"))
    output_rate, afe_output = read_wav(args.prefix.with_name(args.prefix.name + "-afe-output.wav"))
    duration = min(len(raw) / raw_rate, len(afe_input) / input_rate,
                   len(afe_output) / output_rate)
    start = max(0.0, duration - args.last)
    noise, threshold, regions = speech_regions(
        raw[:, 0], raw_rate, start, args.threshold)

    print(f"duration={duration:.2f}s raw_noise={noise:.1f}dBFS "
          f"speech_threshold={threshold:.1f}dBFS regions={len(regions)}")
    print(" idx       time      raw  afe-in afe-out  out/raw   low mid high (AFE out)")
    for index, (begin, end) in enumerate(regions, 1):
        raw_part = segment(raw[:, 0], raw_rate, begin, end)
        input_part = segment(afe_input[:, 0], input_rate, begin, end)
        output_part = segment(afe_output[:, 0], output_rate, begin, end)
        raw_level = db(np.sqrt(np.mean(raw_part * raw_part)))
        input_level = db(np.sqrt(np.mean(input_part * input_part)))
        output_level = db(np.sqrt(np.mean(output_part * output_part)))
        low = band_db(output_part, output_rate, 80, 300)
        middle = band_db(output_part, output_rate, 300, 3000)
        high = band_db(output_part, output_rate, 3000, 7500)
        print(f"{index:4d} {begin:7.2f}-{end:7.2f} "
              f"{raw_level:6.1f} {input_level:6.1f} {output_level:7.1f} "
              f"{output_level - raw_level:8.1f} {low:5.1f} {middle:5.1f} {high:5.1f}")

    tdm_path = args.prefix.with_name(args.prefix.name + "-tdm-slots.wav")
    if args.tdm and not tdm_path.exists():
        parser.error(f"TDM capture not found: {tdm_path}")
    if args.tdm or tdm_path.exists():
        print_tdm_report(args.prefix, raw, raw_rate, regions)


if __name__ == "__main__":
    main()
