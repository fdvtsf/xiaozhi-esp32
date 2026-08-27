# Multi-tap audio diagnostics

The optional audio diagnostics framework exports synchronized PCM taps and
audio-state events over UDP. It is intended for short, controlled acoustic
debug sessions on a trusted LAN.

## Zero-cost disabled build

`CONFIG_AUDIO_DIAGNOSTICS` defaults to `n`. In that configuration:

- `audio/diagnostics/audio_diagnostics.cc` is not compiled;
- all capture hooks are removed by the preprocessor;
- no diagnostic object, task, queue, PSRAM buffer, socket, branch, or packet is
  present at runtime.

The legacy `CONFIG_USE_AUDIO_DEBUGGER` option is independent and should also be
disabled when measuring a production build.

## Enable a diagnostic build

Open menuconfig and enable:

```text
XiaoZhi Assistant -> Enable multi-tap audio diagnostics
```

Set `Audio diagnostics UDP target` to the receiving computer's LAN IPv4 address
and port, for example `192.168.1.20:8000`.

The enabled build allocates 16 fixed 1200-byte payload slots in PSRAM by
default. Audio tasks only copy into an immediately available slot. If all slots
are busy, diagnostic data is dropped instead of blocking the audio pipeline.
UDP transmission runs in a separate priority-1 task.

## Receive a session

On the computer, allow the selected UDP port through the local firewall and run:

```powershell
python scripts/audio_diagnostics_receiver.py --port 8000
```

Stop with Ctrl+C so WAV headers and the summary are finalized. Each device boot
creates a session directory containing:

```text
codec_input.wav   # ES7210 data before resampling; HoloMate is mic + reference
afe_output.wav    # AFE fetch output at 16 kHz mono
uplink_pcm.wav    # PCM immediately before Opus encoding
playback_pcm.wav  # PCM submitted to the speaker codec
events.jsonl      # AEC, VAD, WakeNet, and voice-processing transitions
summary.json      # Receiver-observed packet loss
```

The files can be imported into Audacity and aligned by their first event or
timestamp. `codec_input.wav` is interleaved; on HoloMate its two channels are
the physical microphone and playback-reference inputs supplied to AFE.

## Interpretation

- Clipping/noise already present in `codec_input.wav` points to analog gain,
  microphone, power, or mechanical noise.
- A healthy reference channel but assistant speech remaining in
  `afe_output.wav` points to AEC alignment, gain matching, nonlinear speaker
  distortion, or excessive acoustic coupling.
- A clean `afe_output.wav` with incorrect VAD events points to VAD policy.
- A clean `uplink_pcm.wav` with incorrect recognition points downstream to
  encoding, transport, or server ASR.

Do not leave diagnostics enabled in production: captured audio may contain
private speech and the enabled build intentionally consumes PSRAM, CPU, and LAN
bandwidth.
