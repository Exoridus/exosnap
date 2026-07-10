#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

// ---------------------------------------------------------------------------
// The single overlay shader used by every consumer that draws a webcam PiP or a
// cursor sprite over a captured frame:
//   * the recording compositor (recorder_core::GpuCompositor),
//   * the DXGI live preview (app/services/DxgiPreviewRenderer).
//
// Both compile the same HLSL and fill the same constant buffer, so the chroma
// key, the mirror and the opacity a user sees in the preview are the exact ones
// the encoder writes.  Do NOT re-derive the key mathematics in UI code or add a
// second pixel shader — extend this one.
//
// D3D-free and header-only on purpose: the constant layout is plain floats, so
// the shaders can be unit-tested against a WARP device without dragging the
// compositor's device plumbing along.
// ---------------------------------------------------------------------------

namespace recorder_core {

struct ChromaKeyParams {
    bool enabled = false;
    uint8_t r = 0;
    uint8_t g = 255;
    uint8_t b = 0;
    float tolerance = 0.40f;
    float softness = 0.15f;
    float spill_reduction = 0.30f;
};

// The `mode` the pixel shader branches on (params.y). Named here so callers stop
// spelling the magic floats.
enum class OverlayMode : int {
    Cursor = 0, // preserve the sprite's own alpha (cursor bitmaps carry a mask)
    Chroma = 1, // key the sprite against ChromaKeyParams
    Opaque = 2, // force alpha to 1 (webcam with chroma disabled, background blit)
};

[[nodiscard]] inline OverlayMode SelectOverlayMode(bool chroma_enabled, bool force_opaque) noexcept {
    if (chroma_enabled) {
        return OverlayMode::Chroma;
    }
    return force_opaque ? OverlayMode::Opaque : OverlayMode::Cursor;
}

// Mirrors `cbuffer DrawConstants` in kOverlayPixelShaderSrc, register b0.
// Three float4 rows; HLSL constant buffers round up to 16-byte rows, so the
// static_asserts below are the contract between C++ and the shader.
struct OverlayPixelConstants {
    float key_color[4]; // r, g, b (0-1) + tolerance
    // x=mirror, y=mode(0=cursor/1=chroma/2=opaque), z=spillReduction, w=softness
    float params[4];
    // x=hdrLinear (0/1), y=refWhiteScale, z=opacity, w=reserved
    float params2[4];
};

static_assert(sizeof(OverlayPixelConstants) == 48, "DrawConstants is three float4 rows");
static_assert(sizeof(OverlayPixelConstants) % 16 == 0, "D3D11 constant buffers require 16-byte rows");
static_assert(offsetof(OverlayPixelConstants, params) == 16, "params must start at row 1");
static_assert(offsetof(OverlayPixelConstants, params2) == 32, "params2 must start at row 2");

// ref_white_scale: linear-light scale applied to sRGB-decoded sprites on the FP16
// native-HDR path (overlay reference white / scRGB reference white). Ignored when
// hdr_linear is false; pass 1.0f there.
// opacity: uniform overlay opacity, clamped to [0,1]; non-finite falls back to 1.
[[nodiscard]] inline OverlayPixelConstants MakeOverlayPixelConstants(const ChromaKeyParams& chroma, bool mirror,
                                                                     bool force_opaque, float opacity, bool hdr_linear,
                                                                     float ref_white_scale) noexcept {
    OverlayPixelConstants pc{};
    pc.key_color[0] = static_cast<float>(chroma.r) / 255.0f;
    pc.key_color[1] = static_cast<float>(chroma.g) / 255.0f;
    pc.key_color[2] = static_cast<float>(chroma.b) / 255.0f;
    pc.key_color[3] = chroma.tolerance;

    pc.params[0] = mirror ? 1.0f : 0.0f;
    pc.params[1] = static_cast<float>(static_cast<int>(SelectOverlayMode(chroma.enabled, force_opaque)));
    pc.params[2] = chroma.spill_reduction;
    pc.params[3] = chroma.softness;

    const float safe_opacity = std::isfinite(static_cast<double>(opacity)) ? std::clamp(opacity, 0.0f, 1.0f) : 1.0f;
    pc.params2[0] = hdr_linear ? 1.0f : 0.0f;
    pc.params2[1] = ref_white_scale;
    pc.params2[2] = safe_opacity;
    return pc;
}

// Fullscreen triangle; the viewport selects the destination rectangle.
inline constexpr char kOverlayVertexShaderSrc[] = R"(
struct VS_OUTPUT {
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
};

VS_OUTPUT main(uint id : SV_VertexID) {
    VS_OUTPUT output;
    output.texcoord = float2((id << 1) & 2, id & 2);
    output.position = float4(output.texcoord * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}
)";

// Chroma-distance algorithm: YCbCr (BT.601) chroma-only distance.
// Separating luminance from chroma makes the key robust to lighting variation.
//
// Spill reduction: reduces key-color chrominance contamination in partially
// keyed edge pixels. Weight is proportional to (1 - alpha) so only edges
// are corrected; fully opaque non-keyed regions are unaffected.
//
// The mirror is a texcoord flip, not a CPU pixel copy — sampling the source
// backwards costs nothing and keeps preview and encoder bit-identical.
inline constexpr char kOverlayPixelShaderSrc[] = R"(
Texture2D frameTex : register(t0);
SamplerState frameSamp : register(s0);

cbuffer DrawConstants : register(b0) {
    float4 keyColor; // r, g, b, tolerance
    float4 params;   // x=mirror, y=mode(0=cursor/1=chroma/2=opaque), z=spillReduction, w=softness
    float4 params2;  // x=hdrLinear, y=refWhiteScale, z=opacity, w=reserved
};

// BT.601 RGB->CbCr. Output range [0,1] with neutral at 0.5.
float2 RgbToCbCr(float3 c) {
    float cb = -0.169f * c.r - 0.331f * c.g + 0.500f * c.b + 0.5f;
    float cr =  0.500f * c.r - 0.419f * c.g - 0.081f * c.b + 0.5f;
    return float2(cb, cr);
}

// sRGB electro-optical transfer (gamma decode) -> linear light, component-wise.
float3 SrgbToLinear(float3 c) {
    float3 lo = c / 12.92f;
    float3 hi = pow(max((c + 0.055f) / 1.055f, 0.0f), 2.4f);
    return lerp(hi, lo, step(c, 0.04045f));
}

float4 main(float4 position : SV_POSITION, float2 texcoord : TEXCOORD0) : SV_TARGET {
    float2 uv = texcoord;
    if (params.x > 0.5f)
        uv.x = 1.0f - uv.x;

    float4 color = frameTex.Sample(frameSamp, uv);
    const float mode = params.y;

    if (mode > 1.5f) {
        // mode 2: opaque sprite, no chroma key
        color.a = 1.0f;
    } else if (mode > 0.5f) {
        // mode 1: chroma key via YCbCr chroma distance
        const float2 cbcr_sample = RgbToCbCr(color.rgb);
        const float2 cbcr_key    = RgbToCbCr(keyColor.rgb);
        const float  dist        = distance(cbcr_sample, cbcr_key);

        const float tol       = keyColor.a;
        const float soft      = max(params.w, 0.001f);
        const float softTotal = tol + soft;

        if (dist <= tol) {
            color.a = 0.0f;
        } else if (dist >= softTotal) {
            color.a = 1.0f;
        } else {
            color.a = (dist - tol) / (softTotal - tol);
        }

        // Spill reduction: suppress key chrominance on partially-keyed edges.
        // Compute key luminance and its chroma direction, then remove the
        // projection of the sample onto that direction, weighted by (1-alpha).
        const float spill = params.z;
        if (spill > 0.001f && color.a > 0.001f) {
            const float key_lum      = dot(keyColor.rgb, float3(0.2126f, 0.7152f, 0.0722f));
            const float3 key_chroma  = keyColor.rgb - float3(key_lum, key_lum, key_lum);
            const float  key_lensq   = dot(key_chroma, key_chroma);
            if (key_lensq > 0.001f) {
                const float  clum        = dot(color.rgb, float3(0.2126f, 0.7152f, 0.0722f));
                const float3 col_chroma  = color.rgb - float3(clum, clum, clum);
                const float  proj        = dot(col_chroma, key_chroma);
                if (proj > 0.0f) {
                    const float strength = spill * (1.0f - color.a);
                    color.rgb -= key_chroma * (proj / key_lensq) * strength;
                    color.rgb  = saturate(color.rgb);
                }
            }
        }
    }
    // else mode 0: cursor — preserve source alpha unchanged

    // HDR-linear output: the background is linear scRGB (1.0 = 80 nits). Overlay
    // sprites are sRGB-encoded, so decode to linear and scale to the HDR overlay
    // reference white before the (linear-light) alpha blend, so soft chroma-keyed
    // edges — which carry partial alpha — composite correctly. The overall overlay
    // opacity is a caller-supplied uniform blend factor applied to alpha.
    if (params2.x > 0.5f) {
        color.rgb = SrgbToLinear(color.rgb) * params2.y;
    }
    color.a *= params2.z;

    return color;
}
)";

} // namespace recorder_core
