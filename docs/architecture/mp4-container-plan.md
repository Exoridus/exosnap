# MP4 Container Implementation Plan

## Status

MP4 recording is **not implemented**. The `capability::Container::Mp4` enum value and `capability::Container::Matroska` MKV enum value exist as product surface only. All current recording routes through the WebM runtime path.

---

## Why WebM is the current default

WebM (DocType=webm) is written by the libwebm/mkvmuxer library already integrated into the project. The combination AV1 NVENC + libopus + mkvmuxer is fully implemented, end-to-end validated, and produces technically correct multi-track WebM files.

Shipping WebM as the primary container:

- minimises third-party muxer scope for the alpha release
- avoids the Annex-B → AVCC conversion required for H.264 in MP4
- sidesteps the IMFSinkWriter lifecycle complexity
- is compatible with both Chromium-based players and VLC

---

## Why MP4 requires IMFSinkWriter

MP4 is not a format libwebm can produce. The project must not add libavformat/FFmpeg (see project rules). The only compliant option for a Windows-native MP4 muxer without third-party libraries is:

**Media Foundation Sink Writer (`IMFSinkWriter`, `MFCreateSinkWriterFromURL`).**

The Sink Writer handles the fMP4 atom layout, brand negotiation, and track interleaving. It accepts `IMFSample` objects tagged with `MF_MT_SUBTYPE = MFVideoFormat_H264` for video and `MFAudioFormat_AAC` for audio.

Relevant Windows SDK headers:

- `mfreadwrite.h` — `MFCreateSinkWriterFromURL`
- `mfapi.h`, `mferror.h`
- `wmcontainer.h` — `CLSID_MPEG4MediaSink` (alternative lower-level sink)

---

## Why H.264 Annex-B → AVCC conversion is required

NVENC outputs H.264 in **Annex-B** byte-stream format (start codes `00 00 00 01`). The MP4 container requires **AVCC** (length-prefixed NAL units with SPS/PPS in the `avcC` box).

Required steps before feeding samples to IMFSinkWriter:

1. Extract the SPS and PPS NAL units from the first IDR frame or from NVENC codec private data.
2. Build the `avcC` configuration record (version, profile, level, length-size, SPS list, PPS list).
3. Set this record as the `MF_MT_MPEG_SEQUENCE_HEADER` attribute on the video media type.
4. Strip Annex-B start codes from each sample and replace them with 4-byte big-endian NAL lengths.

This conversion is a small utility (~100 lines) that can be unit-tested independently with canned NAL buffers.

---

## Why AAC is already ready for MP4

The existing `MfAacEncoder` produces raw AAC-LC ADTS or raw AAC payload — both accepted by IMFSinkWriter via `MFAudioFormat_AAC`. No additional work is needed for the audio path except confirming the correct media type attributes (`MF_MT_AUDIO_SAMPLES_PER_SECOND`, `MF_MT_AUDIO_NUM_CHANNELS`, `MF_MT_AUDIO_AVG_BYTES_PER_SECOND`).

Opus is **not** valid for MP4 in the ExoSnap product matrix (no spec-compliant box type in common ISOBMFF, no Windows player support). When the user selects MP4, the audio codec must be AAC.

---

## Why AV1-in-MP4 is deferred

AV1 in MP4 requires:

- the `av01` sample entry box defined in AOM's ISOBMFF binding spec
- the `AV1CodecConfigurationRecord` (av1C box) carrying the Sequence OBU
- IMFSinkWriter may not support the AV1 ISOBMFF binding natively on all Windows versions

This is non-trivial to validate and adds no concrete user value over WebM + AV1. AV1-in-MP4 is deferred until after H.264 + AAC in MP4 is fully shipped.

---

## Implementation boundary points

The following locations in the source are where MP4 work must begin:

| Location | What to add |
|---|---|
| `libs/recorder_core/include/recorder_core/codec_types.h` | No change needed; Container::Matroska already exists for MKV; MP4 is a separate runtime path |
| `libs/recorder_core/src/recorder_session.cpp` — `Validate()` | Add MP4 container acceptance and validate MP4 + H264 + AAC combination |
| `libs/recorder_core/src/mux_thread.cpp` | Add dispatch: Container::WebM → WebmMuxer; Container::Mp4 → Mp4MuxThread |
| `libs/recorder_core/src/` (new) | `mp4_mux_thread.cpp` using `IMFSinkWriter` |
| `libs/recorder_core/src/nvenc_encoder.cpp` | Add H.264 GUID/profile/preset branch |
| `libs/recorder_core/src/` (new) | `annexb_to_avcc.cpp` — NAL conversion utility |
| `libs/capability/src/capability_set.cpp` | Mark MP4 + H264 + AAC as Available only when runtime probe confirms H264 works |
| `libs/capability/src/capability_builder.cpp` | Add downgrade rule for MP4 + H264 if NVENC H264 is not confirmed |
| `libs/capability/src/translation.cpp` | Add MP4 + H264 + AAC translation path |
| `apps/exosnap/pages/OutputPage.*` | Enable MP4 + H264 + AAC in UI only when capability returns Available |

---

## Acceptance gate

MP4 must not be enabled in the UI until **all** of the following are true:

1. A real `.mp4` file is produced (not WebM with a renamed extension).
2. `ffprobe -v error -show_format -show_streams output.mp4` reports:
   - `format_name=mov,mp4,...`
   - `codec_name=h264` for the video stream
   - `codec_name=aac` for the audio stream
3. The file is playable in Windows Media Player and VLC.
4. WebM recording remains green (no regression).
5. CTest passes all tests including MP4 validation and WebM regression tests.
