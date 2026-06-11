# probe_mf_aac_encode

Milestone M2.7 — Media Foundation AAC encode probe.

## Purpose

Validates the Windows Media Foundation AAC-LC encode path on this machine.
Captures the default WASAPI render endpoint in loopback mode, encodes the
captured PCM to AAC-LC via MF, and writes a raw ADTS `.aac` file.

## Requirements

- Windows 10 / 11
- Default audio render device must be 48000 Hz, stereo, float-compatible
- No external dependencies (pure Windows API)

## Input format

- WASAPI default render endpoint, loopback mode
- 48000 Hz, 2 channels (sample format negotiated at runtime)
- Input type negotiation: Phase 04 calls `GetInputAvailableType` to enumerate what
  the AAC encoder MFT accepts, then selects the best match in this order:
  1. PCM int16 48 kHz stereo (preferred — lowest conversion cost for the encoder)
  2. PCM int32 48 kHz stereo
  3. Float32 48 kHz stereo
  4. Any PCM or Float type at 48 kHz stereo
  5. Manually constructed PCM int16 48 kHz stereo (fallback if enumeration yields nothing)
- WASAPI loopback delivers Float32; if the encoder requires PCM int16, the encode loop
  performs an in-place conversion using `ConvertFloat32ToPcm16` before submitting each
  capture packet to `IMFTransform::ProcessInput`.

## Output format

- AAC-LC, 48000 Hz, stereo, 192 kbps
- ADTS payload (raw `.aac` file, no container)

## Output path

```
probe_mf_aac_encode_output\wasapi_loopback_aac.aac
```

## Build

```
cmake --preset windows-x64-debug
cmake --build out/build/windows-x64-debug --target probe_mf_aac_encode
```

## Run

```
.\out\build\windows-x64-debug\apps\probes\probe_mf_aac_encode\probe_mf_aac_encode.exe
```

Run from the repository root so that the output directory is created at
`<repo root>\probe_mf_aac_encode_output\`.

## Windows N / ReviOS behavior

On Windows N editions and stripped builds such as ReviOS, `MFTEnumEx` may return
zero AAC encoder transforms even when the system otherwise appears healthy. This
happens because the Media Feature Pack may be absent or the encoder may be
unregistered from the MFT registry.

When `MFTEnumEx` returns zero results, Phase 02 falls back to direct CLSID
activation via `CoCreateInstance(CLSID_AACMFTEncoder)`. If this succeeds, the
probe continues normally; the summary will note that the encoder was instantiated
via the CLSID path rather than enumeration.

If both enumeration and direct CLSID activation fail, AAC Media Foundation encoder
support is unavailable on that system. The probe exits at Phase 02 with a
diagnostic message. Installing the Windows Media Feature Pack may resolve this.

## Limitations

- No resampler: the probe requires exactly 48 kHz stereo loopback input. If the
  default render endpoint mix format does not match (e.g., 44100 Hz or surround),
  the probe fails at Phase 07 with a format-incompatible error.
- Default render endpoint only: the probe always opens the system default render
  endpoint. There is no mechanism to select a specific device.
- WASAPI Float32 input is converted to PCM16 for the AAC encoder when the encoder
  reports PCM int16 as its preferred input type. The conversion is performed
  in-place per captured packet using `ConvertFloat32ToPcm16`.
- Raw ADTS `.aac` output: the output file is a raw ADTS bitstream. It is not
  wrapped in an MP4 or MKV container and will not play in all media players
  without explicit ADTS support.

## Phase sequence

```
01 MFStartup
02 enumerate AAC encoder MFT via MFTEnumEx
03 instantiate AAC encoder MFT
04 negotiate and configure input type: PCM int16 preferred, 48000 Hz, 2 channels
05 configure output type: AAC-LC, 48000 Hz, 2 channels, 192 kbps, ADTS
06 query stream IDs and output buffer requirements
07 open default render endpoint in WASAPI loopback and start
08 send MFT_MESSAGE_NOTIFY_BEGIN_STREAMING
09 send MFT_MESSAGE_NOTIFY_START_OF_STREAM
10 30-second encode loop
11 EOS drain
12 write ADTS .aac file
13 verify output file size > 4096 bytes
```

## Summary output

```
endpoint        : <device friendly name>
input format    : 48000 Hz, 2 ch, float32
captured frames : <N>
encoded bytes   : <N>
elapsed         : <N.N> s
discontinuities : <N>
output path     : probe_mf_aac_encode_output\wasapi_loopback_aac.aac
file size       : <N> bytes
```
