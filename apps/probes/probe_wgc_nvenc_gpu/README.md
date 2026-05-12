# probe_wgc_nvenc_gpu

Validates the full GPU-path pipeline:

```
WGC BGRA D3D11 texture
  -> ID3D11VideoProcessor (GPU BGRA->NV12)
  -> NVENC D3D11 resource registration (nvEncRegisterResource)
  -> nvEncMapInputResource
  -> AV1 encode
  -> IVF output
```

No CPU readback. No staging textures. No `D3D11_MAP_READ`. No CPU BGRA->NV12 conversion.

## Requirements

- Windows 10 1903+ (WGC)
- NVIDIA Ada Lovelace (RTX 40-series) or newer for AV1 hardware encode
- `nvEncodeAPI64.dll` must be present (`third_party/nvidia/nvEncodeAPI.h` required at build time)

## Usage

```
probe_wgc_nvenc_gpu              # capture first monitor found
probe_wgc_nvenc_gpu --list       # list available targets
probe_wgc_nvenc_gpu <index>      # capture target at 0-based index
```

## Output

`probe_wgc_nvenc_gpu_output\wgc_gpu_av1.ivf`

## Phases

| Phase | Description |
|-------|-------------|
| 01 | Init D3D11 device with `BGRA_SUPPORT \| VIDEO_SUPPORT`; QI `ID3D11VideoDevice`, `ID3D11VideoContext` |
| 02 | Create WGC capture item (monitor or window) |
| 03 | Validate and round source dimensions to even |
| 04 | Load `nvEncodeAPI64.dll`; call `NvEncodeAPICreateInstance` |
| 05 | Open NVENC session with `NV_ENC_DEVICE_TYPE_DIRECTX` |
| 06 | Confirm `NV_ENC_CODEC_AV1_GUID` and `NV_ENC_BUFFER_FORMAT_NV12` |
| 07 | Fetch AV1 preset config (P4, HIGH_QUALITY); set `chromaFormatIDC=1` |
| 08 | `nvEncInitializeEncoder` with `enablePTD=1` |
| 09 | `nvEncCreateBitstreamBuffer` only — no CPU input buffer |
| 10 | Create NV12 D3D11 texture (`D3D11_USAGE_DEFAULT`, `BIND_RENDER_TARGET`, `CPUAccessFlags=0`) |
| 11 | Create `ID3D11VideoProcessorEnumerator`, `ID3D11VideoProcessor`, NV12 output view |
| 12 | `nvEncRegisterResource` — NV12 texture as `NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX` |
| 13 | Start WGC frame pool + `StartCapture` |
| 14 | Wait for first frame (5 s timeout); validate BGRA8 format and dimensions |
| 15 | Capture/convert/map/encode loop (300 frames or 5 s) |
| 16 | EOS flush + drain |
| 17 | `nvEncUnregisterResource` (before `nvEncDestroyEncoder`) |
| 18 | Write IVF file to `probe_wgc_nvenc_gpu_output\wgc_gpu_av1.ivf` |
| 19 | Verify IVF file size: `32 + sum(12 + packet.size())` |
