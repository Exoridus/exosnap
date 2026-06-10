# ExoSnap Developer Probes

Standalone C++ programs that exercise individual hardware subsystems
independently of the full ExoSnap GUI. These are **not** product applications.

## Build

Probes are OFF by default. Enable them with:

```powershell
cmake --preset windows-x64-debug -DEXOSNAP_BUILD_PROBES=ON
cmake --build --preset windows-x64-debug
```

Individual probe targets are placed under `build/<preset>/tools/probes/`.

## Probes

| Probe | Purpose | Hardware Required |
|---|---|---|
| probe_wgc_preview | Visual WGC frame capture preview | GPU + display |
| probe_process_loopback | WASAPI process-loopback audio capture | Audio device |
| probe_nvenc | NVENC encoder initialization | NVIDIA GPU |
| probe_wgc_nvenc | WGC capture + NVENC encode pipeline | NVIDIA GPU |
| probe_wgc_nvenc_gpu | WGC + NVENC GPU texture sharing path | NVIDIA GPU |
| probe_mf_aac_encode | Media Foundation AAC encoding | None |

## Usage

Each probe has its own `README.md` with build and run instructions.
Probes are excluded from normal Release builds, install rules, and CI.
