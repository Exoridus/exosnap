# probe_wgc_nvenc

## Purpose

End-to-end integration probe that captures real desktop content via Windows
Graphics Capture, converts frames to NV12 on the CPU, encodes them as AV1
through NVENC, and writes a valid IVF file. This is the first probe that
exercises the full WGC → encode → container pipeline with real captured
frames rather than synthetic data.

## What this probe validates

**Setup (phases 01–10)**
- D3D11 device created with `D3D11_CREATE_DEVICE_BGRA_SUPPORT` (required by
  WGC; compatible with NVENC).
- WGC capture item created for a user-selected monitor or window;
  source dimensions read from `item.Size()`.
- Source dimensions rounded down to even (required for NV12 4:2:0 chroma
  subsampling); gate requires `encodeWidth ≥ 2` and `encodeHeight ≥ 2`.
- NVENC DLL loaded at runtime; `NvEncodeAPICreateInstance` called.
- NVENC AV1 encode session opened against the shared D3D11 device.
- `NV_ENC_CODEC_AV1_GUID` and `NV_ENC_BUFFER_FORMAT_NV12` verified as
  supported by the driver.
- AV1 preset config fetched with `NV_ENC_PRESET_P4_GUID` +
  `NV_ENC_TUNING_INFO_HIGH_QUALITY`; `chromaFormatIDC = 1` set for YUV420.
- AV1 encoder initialized at actual WGC source dimensions, 60 fps,
  `enablePTD = 1`.
- NVENC CPU input buffer + bitstream buffer allocated.
- Staging texture created (`D3D11_USAGE_STAGING`, `D3D11_CPU_ACCESS_READ`,
  `DXGI_FORMAT_B8G8R8A8_UNORM`) at encode dimensions.

**Capture start (phases 11–12)**
- WGC frame pool started with 3 buffers, `B8G8R8A8UIntNormalized` format.
- First WGC frame received within 5-second timeout; format
  (`DXGI_FORMAT_B8G8R8A8_UNORM`) and dimensions validated.

**Capture-encode loop (phase 13)**
- WGC frames polled in each iteration; all available frames drained and the
  latest taken for encoding (earlier frames counted as dropped/skipped).
- Each WGC frame texture validated for format and dimensions before copy.
- GPU texture copied to staging; staging mapped for CPU access.
- NVENC input buffer locked; BGRA→NV12 conversion written into it.
- Both resources released (unlock + unmap) on all code paths.
- Frame submitted to `nvEncEncodePicture`.
- `NV_ENC_ERR_NEED_MORE_INPUT` handled as non-fatal.
- Bitstream packets captured (bytes + `outputTimeStamp`) for IVF output.
- Loop terminates on first of: 300 frames submitted, 5 seconds elapsed, or
  source loss. Phase passes if at least one frame was submitted without a
  fatal NVENC error.
- Progress printed every 60 captured frames.

**Flush and output (phases 14–16)**
- EOS flush; conservative post-EOS drain of any buffered packets.
- IVF file written to `probe_wgc_nvenc_output\wgc_av1.ivf`. IVF frame
  count is the actual number of collected packets, not hardcoded.
- IVF file size verified against the exact expected byte count:
  `32 + Σ(12 + bitstreamSizeInBytes)`. Size mismatch fails the phase.

## What this probe does not validate

- No GPU resource registration (`NvEncRegisterResource` /
  `NvEncMapInputResource`) — CPU conversion path only.
- No WGC preview window or SwapChain.
- No audio capture or encoding.
- No MKV, MP4, or any container beyond minimal IVF.
- No Opus, AAC.
- No FFmpeg.
- No `recorder_core` integration.
- No WinUI.
- No frame scaling or cropping (source must match encode dimensions).
- No color space accuracy (BT.601 coefficients used; see known limitations).
- No `ffprobe` / decoder round-trip verification.

## BGRA→NV12 conversion

`ConvertBgraToNv12` is a static CPU function. It uses BT.601 limited-range
integer approximations. Color accuracy is not a goal for this probe; the
conversion is correct enough to produce a decodable AV1 bitstream.

Chroma is downsampled by averaging each 2×2 block. The UV plane byte order
is `[U/Cb, V/Cr]` per sample, matching the NV12 specification.

For production use, BT.709 coefficients would be appropriate for 1080p and
higher. The README will be updated when that path is implemented.

## IVF output format

Identical to `probe_nvenc`. See that probe's README for the byte-level
layout. The key correctness point: each frame record uses a **12-byte**
header (4-byte LE size + 8-byte LE timestamp), avoiding the old 4-byte-only
malformed-IVF defect.

Timestamps come from `NV_ENC_LOCK_BITSTREAM::outputTimeStamp` (presentation
order, handles B-frame reordering). IVF timebase is 60/1 (1/60 s per tick).

## Source-loss behavior

If the WGC source is closed mid-capture, the loop stops gracefully. EOS
flush runs on whatever was captured, and the IVF is written with the actual
number of packets collected. Exit code is `0`; source loss is reported in
the summary.

## Requirements

### Hardware

- NVIDIA GPU with NVENC + AV1 support: **Ada Lovelace / RTX 40-series or
  newer**. Phase 06 fails clearly on older hardware with a capability
  message.
- Any monitor or WGC-capturable window.

### Software

- Windows 10 / 11.
- NVIDIA display driver (supplies `nvEncodeAPI64.dll` at runtime).
- NVIDIA Video Codec SDK header `nvEncodeAPI.h` v13.0 at
  `third_party/nvidia/nvEncodeAPI.h` (probe is skipped at configure time if
  missing).

## Build

```pwsh
cmake --preset windows-x64-debug
cmake --build --preset windows-x64-debug --target probe_wgc_nvenc
```

Executable:
```text
build/windows-x64-debug/apps/probes/probe_wgc_nvenc/Debug/probe_wgc_nvenc.exe
```

## Run

```pwsh
.\build\windows-x64-debug\apps\probes\probe_wgc_nvenc\Debug\probe_wgc_nvenc.exe
```

The probe lists available capture targets and prompts for a selection. Enter
the index number, then wait for up to 5 seconds or 300 frames. Exit code
`0` means all phases passed. Exit code `1` means the first failing phase
stopped the run.

## Output artifact

```text
probe_wgc_nvenc_output\wgc_av1.ivf
```

Written relative to the working directory at launch time. The directory is
`.gitignore`d (`probe_wgc_nvenc_output/`). The file is not deleted by the
probe; re-running overwrites it.

## Known limitations

- BT.601 color space rather than BT.709; hue is slightly off on wide-gamut
  displays but the bitstream is decodable.
- Source dimensions are used as-is (minus even-rounding). If WGC delivers a
  different resolution mid-run the probe will fail the per-frame dimension
  gate.
- IVF timestamps represent frame submission order, not wall-clock time.
- Dropped/skipped frames are counted but not further diagnosed.

## Next likely steps

1. `NvEncRegisterResource` / `NvEncMapInputResource` GPU path (eliminates
   CPU copy and conversion).
2. BT.709 color conversion.
3. `ffprobe` / dav1d decode verification.
4. Variable source resolution handling.
