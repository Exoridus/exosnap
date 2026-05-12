# probe_wgc_nvenc_aac_mkv

M2.8 — Live 30-second WGC+NVENC AV1 / WASAPI+MF AAC dual-capture probe muxed into Matroska MKV.

## What it proves

- WGC monitor capture to NVENC AV1 (GPU path, NV12 via VideoProcessorBlt)
- WASAPI loopback to Media Foundation AAC (raw payload type 0)
- WGC `Direct3D11CaptureFrame::SystemRelativeTime`-based video PTS (real capture timing, not CFR packet-index)
- Sample-count-based audio timestamps
- AV1 CodecPrivate derivation by parsing Sequence Header OBU from NVENC bitstream
- AAC CodecPrivate derivation from MF_MT_USER_DATA AudioSpecificConfig bytes
- Matroska MKV mux via libwebm with correct DocType EBML header
- Interleaved packet write via stable_sort on timestamp_ns

## Prerequisites

- NVIDIA Ada Lovelace GPU (RTX 40-series) or newer
- `nvEncodeAPI64.dll` must be present (installed with NVIDIA driver)
- `nvEncodeAPI.h` in `third_party/nvidia/`
- Windows 11, Media Feature Pack installed

## Build

```
cmake --build --preset windows-x64-debug --target probe_wgc_nvenc_aac_mkv
```

## Run

```
probe_wgc_nvenc_aac_mkv              # capture first monitor, 30 seconds
probe_wgc_nvenc_aac_mkv --list      # list available capture targets
probe_wgc_nvenc_aac_mkv <index>     # capture specific target
```

## Video PTS model

Video packet timestamps are derived from `Direct3D11CaptureFrame::SystemRelativeTime`,
which Microsoft defines as the QPC-based compositor render time for the captured frame.

On the first submitted frame the `SystemRelativeTime` value is stored as the video epoch.
Every subsequent frame's PTS is `(SystemRelativeTime - epoch)` expressed in nanoseconds.

Because NVENC may buffer frames internally before returning encoded output (reported as
`NV_ENC_ERR_NEED_MORE_INPUT`), the PTS for each submitted frame is stored in a FIFO
queue before calling `nvEncEncodePicture`. When an encoded output packet becomes
available — either immediately (main loop) or during EOS drain (Phase 15) — the
oldest queued PTS is popped and assigned to that packet.

The MKV container still declares a nominal 60 fps `DefaultDuration` track header, but
the mux timestamps are capture-time based. Video and audio durations should therefore
align within a small skew (typically under 50 ms for a 30-second recording).

## Output

`probe_wgc_nvenc_aac_mkv_output\av1_aac.mkv`
