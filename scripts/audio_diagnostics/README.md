# PCM capture integrity

Start the receiver before speaking. Use a new output prefix for each capture:

```powershell
python scripts/audio_diagnostics/receive_pcm.py --port 8010 --duration 20 --output captures/test-01
```

The receiver reorders packets and removes identical duplicates. A missing packet
starts a new `-partNNN.wav` file; channels of each part have `-chN.wav` files.
A stream with no internal sequence gap retains its original file naming.
No missing audio is invented, padded, or silently removed by joining across gaps.
The JSON manifest records packet sequence numbers, lengths, host arrival times,
segment durations, and sequence gaps. A part's WAV time begins at zero.

Protocol v1 has variable packet lengths and no sample counter or device timestamp.
Missing durations therefore cannot be recovered exactly. Host arrival timestamps
are only approximate event locators, not audio synchronization. AFE also has
processing latency. Do not compare equal time offsets across different WAVs:
first align matching speech within continuous segments, and exclude segment
boundaries from distortion assessment. Even a gap-free packet run cannot rule
out sample loss before diagnostic packetization or processing artifacts.

The legacy analysis script refuses segmented-manifest captures until alignment
is established; its legacy same-time reports are explicitly marked unverified.
Old WAVs without packet metadata cannot have their missing intervals restored.

To separate diagnostic workload from production audio behavior, a separate
firmware A/B test must disable diagnostic compile-time switches while keeping
AEC/MMR and all acoustic parameters unchanged. Use server-side ASR recordings
for that test. Host-side receiver changes alone do not reduce device workload.
