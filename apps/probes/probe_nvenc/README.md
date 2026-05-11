# probe_nvenc

## Purpose

A stop-on-first-failure diagnostic probe that validates the NVENC encode stack
on the local machine. Designed to prove each capability layer independently
before integrating NVENC into the production encoder.

All phases use the official NVIDIA preset flow. No rate-control, B-frame,
GOP, CQ, or other encode parameters are overridden beyond what the preset
provides.

## What this probe validates

The probe runs three successive validation sections, each behind its own phase
sequence. A failure in any phase stops the run immediately with the phase
number and NVENC status code.

**H.264 one-frame encode (phases 01–13)**

- The `nvEncodeAPI64.dll` loads and exports `NvEncodeAPICreateInstance`.
- A D3D11 device opens on the NVIDIA hardware adapter (vendor `0x10DE`).
- An NVENC encode session opens with `NV_ENC_DEVICE_TYPE_DIRECTX`.
- `NV_ENC_CODEC_H264_GUID` is supported.
- `NV_ENC_BUFFER_FORMAT_NV12` is supported for H.264.
- Preset config fetches for P4 + `NV_ENC_TUNING_INFO_HIGH_QUALITY`.
- Encoder initializes: 1920×1080, 60 fps, `enablePTD=1`.
- One NV12 input buffer and one bitstream buffer allocate.
- One deterministic gray frame submits and produces a non-empty bitstream.
- EOS flush completes cleanly.

**AV1 one-frame encode (phases 14–23)**

- A fresh encode session opens against the same D3D11 device.
- `NV_ENC_CODEC_AV1_GUID` is supported (Ada Lovelace / RTX 40-series or
  newer required for GeForce; see hardware requirements below).
- `NV_ENC_BUFFER_FORMAT_NV12` is supported for AV1.
- AV1 preset config fetches with `chromaFormatIDC = 1` (YUV420/NV12).
- AV1 encoder initializes with the same dimensions, frame rate, and PTD
  settings as H.264.
- One deterministic gray NV12 frame submits and produces a non-empty AV1
  bitstream.
- EOS flush completes cleanly.

**AV1 300-frame synthetic encode + IVF output (phases 24–28)**

- A third fresh AV1 session opens with the same configuration.
- 300 synthetic NV12 frames submit in sequence. Each frame's luma value is
  `frameIdx % 256`; chroma is fixed at 128. Frames are not byte-identical.
- `NV_ENC_ERR_NEED_MORE_INPUT` is treated as non-fatal; affected frames are
  drained after EOS.
- Cumulative encoded bytes and encode FPS are reported.
- All collected AV1 bitstream packets are written to a well-formed IVF file
  (`probe_nvenc_output\av1_300f.ivf`).
- The output file is verified to exist and to be larger than the 32-byte IVF
  file header.

## What this probe does not validate

- No Windows Graphics Capture (WGC).
- No audio capture or encoding.
- No HEVC encode path.
- No MKV, MP4, or any container beyond the minimal IVF written for AV1.
- No Opus, AAC, or any audio codec.
- No FFmpeg.
- No `recorder_core` integration.
- No WinUI.
- No real capture content; all frames are synthetic.
- No quality verification (no `ffprobe`, no VMAF, no SSIM).
- No sustained H.264 encode (only one H.264 frame).

## Requirements

### Hardware

- NVIDIA GPU with NVENC support.
- For H.264: any NVENC-capable GPU (Maxwell / GTX 900-series or newer).
- For AV1: **Ada Lovelace / RTX 40-series or newer** (GeForce). On older
  hardware, phase 15 (AV1 GUID support) will fail with a clear capability
  message. H.264 phases remain unaffected.

### Software

- Windows 10 or Windows 11.
- NVIDIA display driver installed and supplying `nvEncodeAPI64.dll` at
  runtime. No static link to the driver DLL; it is loaded at run time.
- NVIDIA Video Codec SDK header `nvEncodeAPI.h` v13.0 placed at
  `third_party/nvidia/nvEncodeAPI.h`. Download from
  <https://developer.nvidia.com/video-codec-sdk>.

### CI / headless builders

The `probe_nvenc` target is **optional**. If
`third_party/nvidia/nvEncodeAPI.h` is missing at configure time the target
is silently skipped with a CMake `STATUS` message and the rest of the build
continues.

## Build

```pwsh
cmake --preset windows-x64-debug
cmake --build --preset windows-x64-debug --target probe_nvenc
```

The executable is produced at:

```text
build/windows-x64-debug/apps/probes/probe_nvenc/Debug/probe_nvenc.exe
```

## Run

```pwsh
.\build\windows-x64-debug\apps\probes\probe_nvenc\Debug\probe_nvenc.exe
```

Exit code `0` means all phases passed. Exit code `1` means the first
failing phase stopped the run; the failing phase number, label, and NVENC
status are printed on that line.

## Output artifact

Phase 27 creates the directory `probe_nvenc_output\` (relative to the
current working directory at launch time) and writes:

```text
probe_nvenc_output\av1_300f.ivf
```

This file is an intentional probe artifact and is **not deleted** by the
probe. It is excluded from version control by the top-level `.gitignore`
entry `probe_nvenc_output/`.

Expected size for the 300-frame deterministic synthetic encode: **~13–14 KB**.

## Phase overview

### Section 1 — H.264 one-frame validation

| Phase | Label |
|-------|-------|
| 01 | load nvEncodeAPI64.dll |
| 02 | create NVENC API instance |
| 03 | create D3D11 device |
| 04 | open encode session |
| 05 | H.264 GUID support |
| 06 | NV12 input format support |
| 07 | fetch preset config |
| 08 | H.264 baseline init |
| 09 | create NVENC input/output buffers |
| 10 | fill one NV12 frame |
| 11 | submit one H.264 frame |
| 12 | lock bitstream / verify bytes > 0 |
| 13 | EOS flush and cleanup |

### Section 2 — AV1 one-frame validation

| Phase | Label |
|-------|-------|
| 14 | open AV1 encode session |
| 15 | AV1 GUID support |
| 16 | AV1 NV12 input format |
| 17 | fetch AV1 preset config |
| 18 | AV1 encoder init |
| 19 | create AV1 input/output buffers |
| 20 | fill one NV12 frame |
| 21 | submit one AV1 frame |
| 22 | lock AV1 bitstream / verify bytes > 0 |
| 23 | AV1 EOS flush and cleanup |

### Section 3 — AV1 300-frame synthetic encode + IVF output

| Phase | Label |
|-------|-------|
| 24 | open AV1 sustained session |
| 25 | AV1 300-frame encode loop |
| 26 | AV1 sustained drain and report |
| 27 | write AV1 IVF file |
| 28 | verify IVF file |

## IVF output format

The IVF file written by phase 27 follows the standard IVF container layout.

### File header (32 bytes)

| Offset | Size | Value | Description |
|--------|------|-------|-------------|
| 0 | 4 | `DKIF` | Signature |
| 4 | 2 | `0` | Version |
| 6 | 2 | `32` | Header size |
| 8 | 4 | `AV01` | Codec FourCC |
| 12 | 2 | `1920` | Frame width |
| 14 | 2 | `1080` | Frame height |
| 16 | 4 | `60` | Frame-rate numerator |
| 20 | 4 | `1` | Frame-rate denominator |
| 24 | 4 | N | Frame count (actual packets written, not hardcoded) |
| 28 | 4 | `0` | Reserved |

### Per-frame record header (12 bytes)

| Offset | Size | Value | Description |
|--------|------|-------|-------------|
| 0 | 4 | N | Frame size in bytes (little-endian uint32) |
| 4 | 8 | T | Presentation timestamp (little-endian uint64, from `NV_ENC_LOCK_BITSTREAM::outputTimeStamp`) |

The 12-byte per-frame header avoids the earlier malformed-IVF defect where
only 4 bytes (size only, no timestamp) were written, producing records that
many AV1 decoders silently misparse.

Bitstream bytes follow the frame header immediately and are copied from
`bitstreamBufferPtr` before `nvEncUnlockBitstream` is called.

## Expected successful output

```text
[probe] NVENC baseline bring-up probe (M2.4)
[probe] target: H.264/AV1 one-frame + AV1 300-frame synthetic encode + IVF file output
[probe] === H.264 one-frame validation ===
[phase 01] load nvEncodeAPI64.dll ................. PASS
[phase 02] create NVENC API instance .............. PASS
           client header NVENCAPI_VERSION major=13 minor=0
[phase 03] create D3D11 device .................... PASS
           adapter: NVIDIA GeForce RTX ... (vendor 0x10DE)
[phase 04] open encode session .................... PASS
[phase 05] H.264 GUID support ..................... PASS
[phase 06] NV12 input format support .............. PASS
           chosen input format: NV_ENC_BUFFER_FORMAT_NV12
[phase 07] fetch preset config .................... PASS
           preset: NV_ENC_PRESET_P4_GUID, tuning: NV_ENC_TUNING_INFO_HIGH_QUALITY
[phase 08] H.264 baseline init .................... PASS
           1920x1080 @ 60/1 fps, preset P4, tuning HIGH_QUALITY, enablePTD=1
[phase 09] create NVENC input/output buffers ...... PASS
[phase 10] fill one NV12 frame .................... PASS
[phase 11] submit one H.264 frame ................. PASS
[phase 12] lock bitstream / verify bytes > 0 ...... PASS
[phase 13] EOS flush and cleanup .................. PASS
[probe] === AV1 one-frame validation ===
[phase 14] open AV1 encode session ................ PASS
[phase 15] AV1 GUID support ....................... PASS
[phase 16] AV1 NV12 input format .................. PASS
           chosen input format: NV_ENC_BUFFER_FORMAT_NV12
[phase 17] fetch AV1 preset config ................ PASS
           preset: NV_ENC_PRESET_P4_GUID, tuning: NV_ENC_TUNING_INFO_HIGH_QUALITY
[phase 18] AV1 encoder init ....................... PASS
           1920x1080 @ 60/1 fps, AV1, preset P4, tuning HIGH_QUALITY, enablePTD=1
[phase 19] create AV1 input/output buffers ........ PASS
[phase 20] fill one NV12 frame .................... PASS
[phase 21] submit one AV1 frame ................... PASS
[phase 22] lock AV1 bitstream / verify bytes > 0 .. PASS
           bitstreamSizeInBytes=65, pictureType=3
[phase 23] AV1 EOS flush and cleanup .............. PASS
[probe] === AV1 300-frame synthetic validation ===
[phase 24] open AV1 sustained session ............. PASS
           AV1 sustained session ready: 1920x1080 @ 60/1 fps, preset P4, enablePTD=1
[phase 25] AV1 300-frame encode loop .............. PASS
           [frame 060/300] submitted=12 packets=12 bytes=1188
           [frame 120/300] submitted=60 packets=60 bytes=3956
           [frame 180/300] submitted=108 packets=108 bytes=6764
           [frame 240/300] submitted=156 packets=156 bytes=9042
[phase 26] AV1 sustained drain and report ......... PASS
           frames submitted  : 300
           packets locked    : 300
           NEED_MORE_INPUT   : 248
           total bytes       : 10041
           elapsed           : 0.595 s
           encode fps        : 504.5 fps
[phase 27] write AV1 IVF file .................... PASS
           output: probe_nvenc_output\av1_300f.ivf
           packets written   : 300
           ivf frame count   : 300
           bytes written     : 13673
[phase 28] verify IVF file ....................... PASS
           file: probe_nvenc_output\av1_300f.ivf
           size: 13673 bytes
           expected ~13-14 KB for 300-frame deterministic AV1
[probe] encode probe PASS — H.264 one-frame, AV1 one-frame, AV1 300-frame, IVF file written and verified.
```

## Known limitations

- All input frames are synthetic (uniform-luma NV12, Y = `frameIdx % 256`,
  UV = 128). No real-world capture content is used.
- Phase 26 reports `NEED_MORE_INPUT : 248` of 300 submitted frames. This is
  normal behavior with `enablePTD = 1` and a quality preset that internally
  enables B-frame reordering. The post-EOS drain recovers all pending
  bitstream packets.
- The IVF output is decodable but represents synthetic content with no
  meaningful visual quality.
- No `ffprobe` or decoder round-trip verification. Bitstream correctness is
  inferred from non-empty size and a valid IVF container structure only.
- Sustained H.264 encode and H.264 IVF output are not yet validated.
- AV1 NVENC encode requires Ada Lovelace (RTX 40-series) or newer on
  GeForce. Phase 15 prints a clear capability failure message on older
  hardware; the H.264 section is unaffected.

## Next likely probe steps

1. Sustained H.264 multi-frame encode and H.264 Annex B output.
2. Verify IVF decode round-trip with `ffprobe` or `dav1d`.
3. Rate-control variation experiments (CQ, CBR) within the probe framework.
4. D3D11 texture path via `NvEncRegisterResource` / `NvEncMapInputResource`
   (replacing the CPU input buffer path used here).
5. Controlled preset and B-frame configuration experiments.

## Reference sources

- NVIDIA NVENC Video Encoder API Programming Guide 13.0,
  <https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-video-encoder-api-prog-guide/index.html>
- NVIDIA Video Codec SDK samples,
  <https://github.com/NVIDIA/video-sdk-samples>
- IVF container format: libvpx / libaom reference implementations
