# ADR 0038: Media Foundation delay-load + webcam capability gate

## Status

Accepted — implemented in the 0.9.0 wave (strand S4).

## Context

ExoSnap's live capture/encode pipeline is Media Foundation (MF)-free: the MF AAC
encoder was superseded by FDK-AAC (the live audio path uses `FdkAacEncoder`), and
video uses NVENC. The **only** runtime MF consumer in `exosnap.exe` is the webcam
overlay (`WebcamService`, via `IMFSourceReader`), plus two async startup capability
probes (`ProbeMfWebcam`, `ProbeMfAac`).

Windows N/KN editions ship without the Media Feature Pack, so `mfplat.dll`, `mf.dll`,
and `mfreadwrite.dll` are absent. Because these DLLs were statically imported, the
loader resolved them at process start — on a Windows-N host without the Media Feature
Pack, `exosnap.exe` would fail to launch (or the capability probe would fault) even
though the entire core recording path needs no MF.

## Decision

1. **Delay-load the MF DLLs.** `/DELAYLOAD:mfplat.dll`, `/DELAYLOAD:mf.dll`,
   `/DELAYLOAD:mfreadwrite.dll` on the `exosnap` target (MSVC only) + `delayimp.lib`.
   The DLLs are bound on first use, not at startup. `mfuuid.lib` is a static GUID lib
   (no backing DLL) and is not delay-loaded.
2. **Probe before any MF entry point.** `WebcamService::IsMfPresent()` is a cached,
   `noexcept`, thread-safe `LoadLibraryW(L"mfplat.dll")` probe. `kernel32` is not
   delay-loaded, so the probe itself is safe on a host where MF is missing — it never
   reaches the delay-load failure hook (`__delayLoadHelper2` → `VcppException`). All
   MF call sites (`EnumerateDevices`, `EnumerateFormats`, `ThreadMain`) and the
   capability probes are guarded by it.
3. **Gate the webcam capability.** `ProbeMfWebcam` feeds `CapabilitySet.mf_webcam_available`.
   When false, the webcam UI (Record page + Settings → Webcam panel) is disabled with a
   notice that references the Media Feature Pack, and a "Webcam (MF)" row appears in
   Diagnostics. The rest of the app runs normally.

## Consequences

- ExoSnap launches and records on Windows N/KN without the Media Feature Pack; only the
  webcam overlay is unavailable, clearly surfaced rather than crashing.
- MF DLLs are not loaded for users who never open the webcam UI (minor startup win).
- The `recorder_core` test binaries that exercise the legacy `MfAacEncoder`
  (`test_mf_aac_encoder`) still link MF normally and require MF present — they are not
  shipped, so there is no runtime impact, but they would fail on a true Windows-N host.
- The legacy `MfAacEncoder` is still compiled into `recorder_core` though the live
  pipeline uses FDK-AAC; harmless under delay-load (never invoked). A future cleanup
  could delete it and retire the `mf_aac` capability.
- Possible UX follow-up: the gate disables the UI and reports it; it does not offer to
  install the Media Feature Pack or deep-link to Windows settings.
