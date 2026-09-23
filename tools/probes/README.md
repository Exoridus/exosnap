# Developer probes

Small executables isolate a hardware/API question without running the complete application. Some call production engine code; others deliberately implement a standalone API experiment. Identify which one a result proves before treating it as product evidence.

## Build and run

Most probes are opt-in:

```powershell
cmake --preset windows-x64-debug -DEXOSNAP_BUILD_PROBES=ON
cmake --build build/windows-x64-debug --config Debug --target <probe target>
```

Multi-config outputs normally sit under `build/windows-x64-debug/tools/probes/<target>/Debug/`. The actual target directory/build result is authoritative. Release-gate instruments such as the stall/fullscreen test windows can be built unconditionally; do not assume every probe uses the same gate. Probes are not installed in the user package.

Run visible/audio-producing probes only with desktop coordination, and run them from a scratch directory when they create files. Hardware requirements are established by their capability checks, not by an assumed GPU model alone.

| Probe | Question |
|---|---|
| [probe_wgc_preview](probe_wgc_preview/README.md) | Can WGC capture the selected monitor/window and display BGRA frames? |
| [probe_process_loopback](probe_process_loopback/README.md) | Can process-loopback WASAPI capture the selected process? |
| [probe_nvenc](probe_nvenc/README.md) | Can a standalone NVENC session encode its synthetic input? |
| [probe_wgc_nvenc](probe_wgc_nvenc/README.md) | Does standalone WGC + CPU conversion + NVENC produce an AV1 bitstream? |
| [probe_wgc_nvenc_gpu](probe_wgc_nvenc_gpu/README.md) | Does WGC + GPU conversion/resource registration + NVENC work? |
| [probe_mf_aac_encode](probe_mf_aac_encode/README.md) | Does the independent Media Foundation AAC API experiment work? Not the product AAC encoder. |
| `probe_gpup_nvenc`, `probe_idd_duplication` | Is NVENC/duplication usable in the declared guest and output configuration? |
| `probe_encode_file`, `probe_edit_playback` | Exercise real encoder or editor code with controlled media |
| `probe_luminance_cost` | GPU query cost of luminance analysis and tone-mapping |

## Interpreting evidence

An init-only PASS is not a decoded-file round trip. A standalone CPU/BT.601 experiment does not validate the product's GPU BT.709/HDR compositor. A probe reporting frames says nothing about all output containers, audio routing, recovery or frontend lifecycle unless it actually executes those paths.

Media Foundation AAC is not a production recording dependency; current AAC uses FFmpeg's native encoder. Keep its probe only as an explicitly scoped API diagnostic, not as a transitional product path or a blocker for normal AAC recording.

For duplication experiments, `probe_idd_duplication` offers `--frames`, `--duration-ms`, `--acquire-timeout-ms`, `--nonblocking-acquire`, `--resource-reset-before-release` and `--hold-ms` forms described by its source. Record the exact invocation; changing acquire/release timing changes the measured behavior. The [guest guide](../../docs/dev/release-verify-vm.md) explains its verification role.

Prefer an existing probe for a specific hardware question. Remove redundant instruments deliberately when their subject is covered elsewhere. Do not copy run histories or experiment plans into these READMEs. Current production invariants belong in [architecture](../../docs/architecture/overview.md).
