# probe_wasapi_endpoint

## Purpose

Prove that the application can:
- Open the current Windows default render endpoint in WASAPI loopback mode.
- Open the current Windows default capture endpoint in normal WASAPI capture mode.
- Read live audio packets from both streams.
- Compute signal metrics (RMS, peak, silence, discontinuities) over a bounded run.

This is an isolated capability probe, not production audio-engine code.

## Build

```pwsh
cmake --preset windows-x64-debug
cmake --build --preset windows-x64-debug --target probe_wasapi_endpoint
```

The executable is produced at:
`build/windows-x64-debug/apps/probes/probe_wasapi_endpoint/<Configuration>/probe_wasapi_endpoint.exe`

## Run

```pwsh
.\build\windows-x64-debug\apps\probes\probe_wasapi_endpoint\Debug\probe_wasapi_endpoint.exe
```

The probe runs for 30 seconds by default. Press `q` to stop early.

For best validation results:
- Play system audio (e.g. music or a video) so the output loopback stream receives non-silent signal.
- Speak or generate input near the microphone so the input capture stream receives non-silent signal.

## What the probe proves

- MMDevice API can enumerate and open the default render and capture endpoints.
- WASAPI loopback capture works for the output endpoint.
- WASAPI normal capture works for the input endpoint.
- Audio packet data can be read and basic metrics computed.
- Both streams can run concurrently.

**This probe does NOT yet prove Process Loopback for APP/SYS isolation.** That is in scope for a later probe.

## Expected manual validation

1. Start system audio playback (music, video, etc.).
2. Run the probe.
3. Confirm both endpoint names and negotiated formats are printed.
4. Confirm 1-second metric lines appear for both `output_loopback` and `input_capture`.
5. Confirm the output loopback shows non-zero RMS and non-100% silence while audio plays.
6. Speak into the microphone and confirm the input capture shows non-zero RMS.
7. Leave the microphone silent briefly and confirm silence percentage rises.
8. Press `q` to stop early, or let the probe run the full 30 seconds.
9. Confirm the final summary is printed for both streams.

## Output fields

### 1-second console metrics (one line per stream)

```
[metrics] stream=output_loopback rate=48000 channels=2 packets=100 frames=48000 rms=0.123 peak=0.456 silence=0.0% discontinuities=0
[metrics] stream=input_capture  rate=48000 channels=2 packets=100 frames=48000 rms=0.012 peak=0.081 silence=35.0% discontinuities=0
```

| Field           | Meaning                                                |
|-----------------|--------------------------------------------------------|
| stream          | `output_loopback` or `input_capture`                   |
| rate            | sample rate (Hz)                                       |
| channels        | channel count                                          |
| packets         | number of audio packets read during the interval       |
| frames          | total audio frames during the interval                 |
| rms             | root-mean-square level over the interval               |
| peak            | maximum absolute sample value over the interval        |
| silence         | % of frames with peak amplitude ≤ 0.005 (~-46 dBFS) or marked silent by WASAPI |
| discontinuities | number of data-discontinuity flags during the interval |

### Run summary (on exit)

```
=== WASAPI ENDPOINT PROBE SUMMARY ===
  duration (s)        : 30.0

--- output_loopback ---
  device             : <friendly name>
  sample rate        : 48000
  channels           : 2
  total packets      : ...
  total frames       : ...
  avg RMS            : ...
  max peak           : ...
  silence            : ... %
  discontinuities    : ...
  non-silent observed: yes

--- input_capture ---
  device             : <friendly name>
  sample rate        : 48000
  channels           : 2
  total packets      : ...
  total frames       : ...
  avg RMS            : ...
  max peak           : ...
  silence            : ... %
  discontinuities    : ...
  non-silent observed: yes
```

## Known limitations

- This probe uses endpoint loopback and endpoint capture only. Process Loopback (APP/SYS isolation) is not demonstrated.
- No encoding, muxing, file output, or diagnostic-framework code.
- Silence detection uses a fixed amplitude threshold of 0.005 (~-46 dBFS). Frames with peak amplitude below this threshold, or packets explicitly flagged silent by WASAPI, are counted as silent. This is a simple probe-level metric, not production noise gating.
- Only float32 and int16 sample formats are supported.
- Shared-mode only; exclusive-mode WASAPI is not tested.
- Event-driven buffering is not used; packets are polled periodically.
