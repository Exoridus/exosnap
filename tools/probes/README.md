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

| Probe | Purpose | Hardware Required | Invocation |
|---|---|---|---|
| probe_wgc_preview | Visual WGC frame capture preview | GPU + display | `probe_wgc_preview.exe` |
| probe_process_loopback | WASAPI process-loopback audio capture | Audio device | `probe_process_loopback.exe <PID>` |
| probe_nvenc | NVENC encoder initialization test | NVIDIA GPU | `probe_nvenc.exe` |
| probe_wgc_nvenc | WGC capture + NVENC encode pipeline (system memory path) | NVIDIA GPU | `probe_wgc_nvenc.exe` |
| probe_wgc_nvenc_gpu | WGC + NVENC GPU texture sharing path (D3D11 interop) | NVIDIA GPU | `probe_wgc_nvenc_gpu.exe` |
| probe_mf_aac_encode | Media Foundation AAC encoding (legacy/transitional) | None | `probe_mf_aac_encode.exe` |

## probe_mf_aac_encode — Legacy/Transitional Note

Media Foundation AAC encoding is a transitional path. It is not the preferred
future encoder architecture for ExoSnap (which uses FDK-AAC for better
portability and control). This probe remains only for current compatibility
validation and debugging. Remove it when the Media Foundation AAC path is
retired from the production pipeline.

## Maintenance Value

- **probe_wgc_preview:** Validates WGC frame capture works. Useful when
  debugging capture failures or checking frame format compatibility.
- **probe_process_loopback:** Validates WASAPI process loopback audio capture
  against a specific PID. Critical for debugging app-audio isolation issues.
- **probe_nvenc:** Tests NVENC encoder init standalone (no capture dependency).
  First-stop diagnostic for NVENC API or driver problems.
- **probe_wgc_nvenc:** End-to-end system-memory capture+encode pipeline.
  Validates the most common recording path used in production.
- **probe_wgc_nvenc_gpu:** Tests the GPU-texture-sharing NVENC path. This is
  a distinct code path from probe_wgc_nvenc (D3D11 texture interop vs system
  memory copy). Validate this if GPU compositor or texture-sharing issues arise.
- **probe_mf_aac_encode:** Validates Media Foundation AAC encoder
  initialization and basic encoding. See retirement note above.

## Relationship to Production Architecture

Probes exercise the same low-level Windows APIs (WGC, WASAPI, NVENC, MF) that
the production recording engine uses, but in isolation. They share no code
with the production pipeline — each probe has its own self-contained
implementation. When a production integration test fails, the corresponding
probe helps isolate whether the issue is in the API layer or the integration.

## Retirement Conditions

A probe should be removed when:
- The subsystem it tests has a production-level self-test or diagnostic
- The underlying API is no longer used by ExoSnap
- The probe duplicates capabilities available in `recorder_core` tests

## Usage

Each probe has its own `README.md` with detailed build and run instructions.
Probes are excluded from normal Release builds, install rules, and CI.
