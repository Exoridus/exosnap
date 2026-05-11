# probe_process_loopback

## Purpose

Prove that the application can:
- Enumerate visible top-level windows and select a target process by PID.
- Open two independent WASAPI Process Loopback capture streams against the same target process tree:
  - `INCLUDE` target process tree (future APP source)
  - `EXCLUDE` target process tree (future SYS source)
- Read live audio packets from both streams.
- Compute signal metrics (RMS, peak, silence, discontinuities) over a bounded run.
- Prove that INCLUDE and EXCLUDE are disjoint: audio rendered by the target process should appear in INCLUDE but not EXCLUDE, and vice versa.

This is an isolated capability probe, not production audio-engine code.

## Build

```pwsh
cmake --preset windows-x64-debug
cmake --build --preset windows-x64-debug --target probe_process_loopback
```

The executable is produced at:
`build/windows-x64-debug/apps/probes/probe_process_loopback/<Configuration>/probe_process_loopback.exe`

## Run

```pwsh
.\build\windows-x64-debug\apps\probes\probe_process_loopback\Debug\probe_process_loopback.exe
```

On startup the probe:
1. Prints that it requires Windows Process Loopback support.
2. Enumerates visible top-level windows with non-empty titles.
3. Prompts for a target number or `q` to quit.
4. Prints the selected target window title and PID.
5. Creates two process-loopback capture clients for the same target PID:
   - `include_target_tree` (INCLUDE mode)
   - `exclude_target_tree` (EXCLUDE mode)
6. Prints both capture formats.
7. Runs for 30 seconds, printing per-second metrics for both streams.
8. Press `q` to stop early.

## What the probe proves

- Process Loopback can be activated via `ActivateAudioInterfaceAsync` with `AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK`.
- Two independent streams can be created for the same target PID with opposite modes.
- INCLUDE captures audio rendered by the target process tree.
- EXCLUDE captures audio rendered by everything EXCEPT the target process tree.
- This probe is intended to validate the APP/SYS process-loopback model:
  - APP = INCLUDE selected target process tree
  - SYS = EXCLUDE same selected target process tree

## OS/API requirements

- Windows 10 Build 20348+ or Windows 11
- `ActivateAudioInterfaceAsync` from `mmdevapi.dll` with `AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK`
- `audioclientactivationparams.h` from Windows SDK 10.0.26100+
- Virtual process-loopback device: `VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK` (`L"VAD\\Process_Loopback"`)
- Completion handler must be agile (aggregate Free-Threaded Marshaler via `CoCreateFreeThreadedMarshaler`)
- Probe capture format is hard-coded PCM 44100 Hz / 2ch / 16-bit; `GetMixFormat()` is not called on the process-loopback virtual device

## Manual validation: INCLUDE vs EXCLUDE disjointness

### Setup

1. Open a **target** app capable of playing audio (e.g. VLC, a browser tab with a YouTube video, or Windows Media Player). Note: the app must render audio through the Windows audio stack.
2. Prepare a separate **non-target** app capable of playing different audio (e.g. a different browser, a different media player, or a system sound event).

### Validation steps

1. **Run the probe** and select the target app from the window list.
2. **Step A — Only target audio plays:**
   - Play unique audio in the target app (e.g. a specific music track).
   - Ensure no other apps are playing audio.
   - Observe the per-second metrics:
     - `include_target_tree` should show non-silent signal (RMS > 0, peak > 0.005, silence < 100%).
     - `exclude_target_tree` should be silent or near-silent (silence close to 100%, low RMS/peak).
3. **Step B — Only non-target audio plays:**
   - Stop the target app audio.
   - Play different audio in a non-target app (e.g. a different browser tab, different media player).
   - Observe the per-second metrics:
     - `include_target_tree` should be silent or near-silent.
     - `exclude_target_tree` should show non-silent signal.
4. **Step C — Both play:**
   - Play audio in both target and non-target apps simultaneously (if possible).
   - Observe that both streams show non-silent signal simultaneously.
5. **Step D — Stop both and continue:**
   - Stop all audio playback.
   - Observe both streams approach 100% silence.
6. **Stop the probe** by pressing `q` or letting it run to completion.
7. **Review the final summary** for both streams and confirm the non-silent observed flags match expectations.

### Expected disjointness result

| Scenario                     | include_target_tree | exclude_target_tree |
|------------------------------|---------------------|---------------------|
| Only target audio            | non-silent          | silent/near-silent  |
| Only non-target audio        | silent/near-silent  | non-silent          |
| Both play                    | non-silent          | non-silent          |
| Neither plays                | silent              | silent              |

## Metrics

### Per-second console metrics (one line per stream)

```
[metrics] stream=include_target_tree pid=1234 rate=48000 channels=2 packets=100 frames=48000 rms=0.080 peak=0.400 silence=0.0% discontinuities=0
[metrics] stream=exclude_target_tree pid=1234 rate=48000 channels=2 packets=100 frames=48000 rms=0.020 peak=0.120 silence=70.0% discontinuities=0
```

| Field           | Meaning                                                |
|-----------------|--------------------------------------------------------|
| stream          | `include_target_tree` or `exclude_target_tree`         |
| pid             | target process ID                                      |
| rate            | sample rate (Hz)                                       |
| channels        | channel count                                          |
| packets         | number of audio packets read during the interval       |
| frames          | total audio frames during the interval                 |
| rms             | root-mean-square level over the interval               |
| peak            | maximum absolute sample value over the interval        |
| silence         | % of frames with peak amplitude <= 0.005 (~-46 dBFS) or marked silent by WASAPI |
| discontinuities | number of data-discontinuity flags during the interval |

If `frames == 0`, silence is printed as `n/a`.

### Run summary (on exit)

```
=== PROCESS LOOPBACK PROBE SUMMARY ===
  duration (s)        : 30.0
  target process loss : no

--- include_target_tree ---
  target PID         : 1234
  sample rate        : 48000
  channels           : 2
  total packets      : ...
  total frames       : ...
  avg RMS            : ...
  max peak           : ...
  silence            : ... %
  discontinuities    : ...
  non-silent observed: yes

--- exclude_target_tree ---
  target PID         : 1234
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

- This probe proves Process Loopback capability only and does not yet implement the production audio engine.
- No encoding, muxing, file output, or diagnostic-framework code.
- No microphone capture (that is a separate WASAPI endpoint probe).
- Silence detection uses a fixed amplitude threshold of 0.005 (~-46 dBFS). Frames with peak amplitude below this threshold, or packets explicitly flagged silent by WASAPI, are counted as silent. This is a simple probe-level metric, not production noise gating.
- The probe uses a hard-coded capture format of PCM 44100 Hz / 2ch / 16-bit. `GetMixFormat()` is not called because the process-loopback virtual device returns `E_NOTIMPL`.
- Process exit monitoring uses `OpenProcess(SYNCHRONIZE)`; if the process is already dead or permission is denied, exit monitoring is skipped with a warning.
- Process loopback requires Windows 10 Build 20348+ or Windows 11.
- If the target process uses child processes for audio, the API captures/excludes based on the entire process tree. This is expected behavior per the API definition.
- Shared-mode only; event-driven buffering is not used; packets are polled periodically.
- The probe uses `VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK`, not a physical audio endpoint.
- The completion handler aggregates the Free-Threaded Marshaler for agile marshaling.
