# Audio Service Architecture

`AudioService` owns one input engine and keeps codec I/O, Opus encoding/decoding,
and network queues independent from the chip-specific speech pipeline.

## Input engines

The old `AudioProcessor + WakeWord` combinations have been replaced by a single
`AudioEngine` interface. `AudioInputTask` reads PCM once and feeds exactly one
engine.

| Target | Engine | Wake word | Uplink processing |
| --- | --- | --- | --- |
| ESP32-S3 / ESP32-P4 / ESP32-S31 | `AfeAudioEngine` | WakeNet inside AFE, or MultiNet fed from AFE output | FD AEC + VAD when audio processing is enabled |
| ESP32 / ESP32-C3 / ESP32-C5 / ESP32-C6 | `LiteAudioEngine` | Standalone WakeNet when configured | Raw mono PCM |

`AfeAudioEngine` owns a single FD AFE instance. WakeNet and voice uplink share
that instance, so enabling both no longer creates two AFE pipelines. For custom
MultiNet wake words, AFE fetch output is passed to `CustomWakeWord`; MultiNet is
not created on the smaller targets.

The MMR (two microphones plus reference) path selects `AFE_TYPE_FD` and
`AFE_MODE_LOW_COST`, retaining the ESP-SR profile's AEC/SE defaults.
The current MMR A/B test explicitly sets `AEC_NLP_LEVEL_NORMAL`, replacing
the previously observed `AGGR` default; no other acoustic settings change.
Other input layouts use the existing VC/HIGH_PERF profile with
`AEC_MODE_VOIP_HIGH_PERF` and `AEC_NLP_LEVEL_NORMAL` overrides.
Standalone NS remains disabled; AGC is enabled for the current A/B experiment.
ESP-SR is pinned to `2.5.5` to evaluate its newer BSS implementation while
keeping the explicit MMR, AEC NLP, microphone gain, VAD and AGC settings intact.
The library's profile defaults can still change across versions; verify the
startup configuration and pipeline before comparing physical audio tests.
In the integrated AFE, `afe_config_check()` prioritizes dual-mic SE/BSS over
NS when the algorithms conflict. Disabling SE falls back to the first mic;
setting `ns_init=true` alone does not add a second denoising stage after BSS.
The 2.5.5 upgrade was compiled for `holomate-waveshare-s3-lcd-1.3` on
ESP-IDF 6.0.2, with 43 host regression tests passing. Wake/ASR quality,
the deployed model assets, interruption and reconnect still require a physical
device test; a successful build does not establish acoustic improvement.
Do not infer current MMR NLP strength from older single-microphone tuning or
documentation.

At initialization, `AFE init config` logs AEC mode, NLP level, filter length,
and AEC/SE/NS/AGC flags from the configuration object after creation. These
are configuration values, not later runtime enable/disable state; the existing
`print_pipeline` output describes the created graph. No acoustic settings are
changed by this logging.

Before starting its fetch task, the engine verifies that the AFE reports
16 kHz, the codec's interleaved channel count, and positive feed/fetch frame
lengths. A mismatch destroys the instance and fails initialization. Codec
capture may still run at 24 kHz: `AudioService` resamples all input channels
before feeding AFE. These checks and logs run only during initialization,
independently of the disabled PCM-dump and periodic-diagnostic features.

When wake-word audio upload is enabled, the most recent two seconds of PCM are
stored in a single 64 KB PSRAM ring buffer. WakeNet and MultiNet share the same
cache implementation, and the encoder reads it one Opus frame at a time. This
avoids the previous per-chunk internal-SRAM allocations and temporary PCM
concatenation buffer.

## Input data flow

### Board input layouts

Built-in boards default to one microphone: `M` without a playback reference, or
`MR` when the codec supplies a playback reference. `R` must contain playback
reference samples, not a second microphone. The channel declaration must match
the interleaved PCM returned by the codec.

Multiple-microphone input remains available for developer experiments; the AFE
still derives its format from the codec's microphone and reference counts.
For Box Lite, the final `BoxAudioCodecLite` constructor parameter is
`microphone_channels`, defaulting to `1`. Pass `2` explicitly to test `MMR`
with reference enabled, or `MM` without it. `EnableInput()` and `Read()` use this
count for capture and interleaving. On ESP32-S31-Korvo-1, developers can change
the board's `AUDIO_INPUT_CHANNELS` from `1` to `2` to test `MM`; its codec has no
playback reference. Neither experimental layout is enabled in default builds.

Single-microphone defaults do not imply that ESP-SR FD lacks dual-microphone
support, or guarantee hardware operation. Validate capture, playback, wake/VAD,
interruption, reconnect, and applicable AEC modes on the affected board before
releasing a change. Box Lite's software playback reference also needs timing
and concurrency validation.

```mermaid
flowchart LR
    Mic[Microphone] --> Codec[AudioCodec]
    Codec --> Input[AudioInputTask]
    Input --> Engine[One AudioEngine]
    Engine --> Wake[Wake-word event]
    Engine --> PCM[16 kHz mono PCM]
    PCM --> EncodeQueue[audio_encode_queue_]
    EncodeQueue --> Opus[OpusCodecTask]
    Opus --> SendQueue[audio_send_queue_]
    SendQueue --> App[Application / network]
```

Wake-word detection and voice processing are independent runtime states on the
same engine. On AFE targets, AEC stays active while wake-word detection is active
so playback reference remains available for wake-up during device playback. It
also stays active during voice processing when device AEC is requested.

## Output data flow

```mermaid
flowchart LR
    App[Application / network] --> DecodeQueue[audio_decode_queue_]
    DecodeQueue --> Opus[OpusCodecTask]
    Opus --> PlaybackQueue[audio_playback_queue_]
    PlaybackQueue --> Output[AudioOutputTask]
    Output --> Codec[AudioCodec]
    Codec --> Speaker[Speaker]
```

## Tasks and power management

- `AudioInputTask` reads codec input and feeds the selected engine.
- `AudioOutputTask` drains decoded PCM to the codec output.
- `OpusCodecTask` encodes uplink PCM and decodes downlink packets.
- `AfeAudioEngine` has its own AFE fetch task on S3/P4/S31.

The audio power timer still enables and disables codec ADC/DAC channels based on
activity; the engine refactor does not change that policy.

## Queue and allocation policy

The service queues use compile-time fixed-capacity storage. Their existing drop,
wait, and playback-drain policies remain unchanged, but queue growth no longer
allocates deque nodes. `AudioTask` values are stored directly, and Opus encoding
writes into the packet payload instead of allocating a second temporary output
buffer. Packet and PCM buffer pools should only be added after target-side heap
and latency measurements justify their additional ownership complexity.
