#include <recorder_core/edit_frame_gpu_converter.h>

#include <d3dcompiler.h>

#include <cstdio>
#include <cstring>

namespace recorder_core {
namespace {

// Full-screen triangle. Verbatim copy of gpu_hdr_tonemap.cpp's vertex shader --
// it carries no tone-map knowledge at all, so both passes share it.
const char* kVertexShaderSrc = R"(
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

// SDR YUV -> BGRA. One variant covers all three DecodedPixelFormat values:
// they differ only in the per-clip matrix/range coefficients (computed on the
// CPU, see YuvConstantsFor) and in whether the chroma planes are half
// resolution. The latter is read off the bound textures rather than branched on
// a constant, so a 4:4:4 plane and a 4:2:0 plane are addressed by the same code.
//
// The planes are R8_UINT / R16_UINT, not UNORM: 10-bit YUV420P10LE samples are
// plain values in [0, 1023] with no P010 <<6 left-justification, so UNORM's
// automatic /65535 would be wrong. Reading raw integers and letting the
// CPU-computed coefficients carry the normalisation matches the CPU reference's
// exact handling (yuv_to_bgra.cpp ConvertFullPlanarYuv420ToBgraScalar).
const char* kYuvPixelShaderSrc = R"(
Texture2D<uint> YPlane : register(t0);
Texture2D<uint> UPlane : register(t1);
Texture2D<uint> VPlane : register(t2);

cbuffer YuvToBgraConstants : register(b0) {
    float c_rv, c_gu, c_gv, c_bu;
    float y_scale;
    float y_off, c_off, pad0;
};

float4 main(float4 position : SV_POSITION, float2 texcoord : TEXCOORD0) : SV_TARGET {
    uint lumaW, lumaH, chromaW, chromaH;
    YPlane.GetDimensions(lumaW, lumaH);
    UPlane.GetDimensions(chromaW, chromaH);
    int2 px = int2(position.xy);
    int2 cpx = (chromaW == lumaW) ? px : (px / 2); // 4:4:4 vs 4:2:0

    float y = float(YPlane.Load(int3(px, 0)));
    float u = float(UPlane.Load(int3(cpx, 0)));
    float v = float(VPlane.Load(int3(cpx, 0)));

    // c_rv/c_gu/c_gv/c_bu already carry the chroma scale AND the /255 that puts
    // the result in [0, 1] -- exactly like ComputeCoefs folds c_scale in once.
    // Applying a separate chroma scale here would apply it twice.
    float yn = (y - y_off) * y_scale;
    float un = u - c_off;
    float vn = v - c_off;
    float r = yn + c_rv * vn;
    float g = yn - c_gu * un - c_gv * vn;
    float b = yn + c_bu * un;
    return float4(saturate(r), saturate(g), saturate(b), 1.0f);
}
)";

// HDR10 (PQ / BT.2020, 10-bit limited range) -> tone-mapped SDR BT.709. A
// verbatim port of the CPU monitoring chain in hdr_preview.h /
// P010PqPixelToMonitorBgr (DequantY10Limited/DequantC10Limited -> YcbcrToPqRgb
// -> PqEotf -> Bt2020ToBt709 -> HdrToneMapChannel -> Bt709Oetf). The CPU
// version is the unit-tested source of truth; keep the two in sync.
const char* kPqPixelShaderSrc = R"(
Texture2D<uint> YPlane : register(t0);
Texture2D<uint> UPlane : register(t1); // Cb
Texture2D<uint> VPlane : register(t2); // Cr

cbuffer PqTonemapConstants : register(b0) {
    float peak_scale; // HdrPeakScale(), >= 1.0
    float pad0, pad1, pad2;
};

static const float kKr2020 = 0.2627f;
static const float kKb2020 = 0.0593f;
static const float kKg2020 = 0.6780f;
static const float kPqM1 = 0.1593017578125f;
static const float kPqM2 = 78.84375f;
static const float kPqC1 = 0.8359375f;
static const float kPqC2 = 18.8515625f;
static const float kPqC3 = 18.6875f;
static const float kPqLinearToScrgb = 125.0f; // 10000.0 / 80.0
static const float3x3 kBt2020ToBt709 = float3x3(
     1.6604910023f, -0.5876411389f, -0.0728498633f,
    -0.1245504746f,  1.1328998971f, -0.0083494226f,
    -0.0181507634f, -0.1005788980f,  1.1187296614f
);

float PqEotf(float signal) {
    float n = saturate(signal);
    float np = pow(n, 1.0f / kPqM2);
    float num = np - kPqC1;
    float den = kPqC2 - kPqC3 * np;
    float base = (num > 0.0f && den > 0.0f) ? (num / den) : 0.0f;
    return pow(base, 1.0f / kPqM1);
}

float HdrToneMapChannel(float x, float peak) {
    const float knee = 0.80f;
    x = max(x, 0.0f);
    if (x <= knee || peak <= knee) {
        return min(x, 1.0f);
    }
    float head = 1.0f - knee;
    float maxExcess = peak - knee;
    float e = x - knee;
    float s = e * (1.0f + e / (maxExcess * maxExcess)) / (1.0f + e);
    return min(knee + head * s, 1.0f);
}

float Bt709Oetf(float l) {
    l = saturate(l);
    return l < 0.018f ? 4.5f * l : 1.099f * pow(l, 0.45f) - 0.099f;
}

float4 main(float4 position : SV_POSITION, float2 texcoord : TEXCOORD0) : SV_TARGET {
    int2 px = int2(position.xy);
    int2 cpx = px / 2; // native HDR10 is always 4:2:0
    float yv = (float(YPlane.Load(int3(px, 0))) - 64.0f) / 876.0f;
    float cbv = (float(UPlane.Load(int3(cpx, 0))) - 512.0f) / 896.0f;
    float crv = (float(VPlane.Load(int3(cpx, 0))) - 512.0f) / 896.0f;

    float rp = yv + crv * (2.0f * (1.0f - kKr2020));
    float bp = yv + cbv * (2.0f * (1.0f - kKb2020));
    float gp = (yv - kKr2020 * rp - kKb2020 * bp) / kKg2020;

    float3 lin2020 = float3(PqEotf(rp), PqEotf(gp), PqEotf(bp));
    float3 lin709 = mul(kBt2020ToBt709, lin2020);

    float3 sdrLinear = float3(
        HdrToneMapChannel(lin709.r * kPqLinearToScrgb, peak_scale),
        HdrToneMapChannel(lin709.g * kPqLinearToScrgb, peak_scale),
        HdrToneMapChannel(lin709.b * kPqLinearToScrgb, peak_scale));

    float3 outRgb = float3(Bt709Oetf(sdrLinear.r), Bt709Oetf(sdrLinear.g), Bt709Oetf(sdrLinear.b));
    return float4(outRgb, 1.0f);
}
)";

// b0 layout of kYuvPixelShaderSrc. 32 bytes (cbuffers are 16-byte aligned).
struct GpuYuvConstants {
    float c_rv;
    float c_gu;
    float c_gv;
    float c_bu;
    float y_scale;
    float y_off;
    float c_off;
    float pad0;
};
static_assert(sizeof(GpuYuvConstants) == 32);

// b0 layout of kPqPixelShaderSrc.
struct GpuPqConstants {
    float peak_scale;
    float pad[3];
};
static_assert(sizeof(GpuPqConstants) == 16);

struct MatrixWeights {
    double kr;
    double kb;
};

// Duplicate of yuv_to_bgra.cpp's WeightsFor (file-local there). Same fallback
// rationale: Unspecified means BT.709, because that is the only matrix this
// engine's encoder ever writes. test_edit_frame_gpu_converter.cpp pins the GPU
// output against the CPU converters, so the two tables cannot drift apart
// unnoticed.
MatrixWeights WeightsFor(MatrixCoefficients matrix) noexcept {
    switch (matrix) {
    case MatrixCoefficients::Bt601:
        return {0.299, 0.114};
    case MatrixCoefficients::Bt2020Ncl:
        return {0.2627, 0.0593};
    case MatrixCoefficients::Bt709:
    case MatrixCoefficients::Unspecified:
    default:
        return {0.2126, 0.0722};
    }
}

// The float mirror of yuv_to_bgra.cpp's ComputeCoefs. Two differences, both
// deliberate:
//   - no 16.16 fixed-point shift (the shader is float already), and
//   - everything is divided by 255 at the end. ComputeCoefs' scales map the
//     normalised signal onto the 0..255 *output byte* domain; the shader wants
//     [0, 1] for its UNORM render target, and 255 is that domain's size
//     whatever the input bit depth. Dividing by the input's max code (1023 for
//     10-bit) instead would double-apply the bit-depth normalisation, which
//     ComputeCoefs already folds into y_scale/c_scale (255/876, 255/896, ...).
GpuYuvConstants YuvConstantsFor(MatrixCoefficients matrix, ColorRange range, DecodedPixelFormat format) noexcept {
    const MatrixWeights w = WeightsFor(matrix);
    const double kg = 1.0 - w.kr - w.kb;
    const double rv = 2.0 * (1.0 - w.kr);
    const double bu = 2.0 * (1.0 - w.kb);
    const double gu = bu * w.kb / kg;
    const double gv = rv * w.kr / kg;

    const bool ten_bit = format == DecodedPixelFormat::Yuv420P10;
    const bool limited = range == ColorRange::Limited;

    double y_scale = 0.0;
    double c_scale = 0.0;
    double y_off = 0.0;
    double c_off = 0.0;
    if (ten_bit) {
        y_scale = limited ? (255.0 / 876.0) : (255.0 / 1023.0);
        c_scale = limited ? (255.0 / 896.0) : (255.0 / 1023.0);
        y_off = limited ? 64.0 : 0.0;
        c_off = 512.0;
    } else {
        y_scale = limited ? (255.0 / 219.0) : 1.0;
        c_scale = limited ? (255.0 / 224.0) : 1.0;
        y_off = limited ? 16.0 : 0.0;
        c_off = 128.0;
    }

    constexpr double kOutputDomain = 255.0;
    GpuYuvConstants c{};
    c.c_rv = static_cast<float>(rv * c_scale / kOutputDomain);
    c.c_gu = static_cast<float>(gu * c_scale / kOutputDomain);
    c.c_gv = static_cast<float>(gv * c_scale / kOutputDomain);
    c.c_bu = static_cast<float>(bu * c_scale / kOutputDomain);
    c.y_scale = static_cast<float>(y_scale / kOutputDomain);
    c.y_off = static_cast<float>(y_off);
    c.c_off = static_cast<float>(c_off);
    c.pad0 = 0.0f;
    return c;
}

void SetHResultError(std::string& err, const char* what, HRESULT hr) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s failed 0x%08lX", what, static_cast<unsigned long>(hr));
    err = buf;
}

bool CompilePixelShader(ID3D11Device* device, const char* src, const char* name, winrt::com_ptr<ID3D11PixelShader>& out,
                        std::string& err) {
    winrt::com_ptr<ID3DBlob> blob;
    winrt::com_ptr<ID3DBlob> error_blob;
    HRESULT hr = D3DCompile(src, std::strlen(src), name, nullptr, nullptr, "main", "ps_5_0",
                            D3DCOMPILE_ENABLE_STRICTNESS, 0, blob.put(), error_blob.put());
    if (FAILED(hr)) {
        SetHResultError(err, name, hr);
        if (error_blob) {
            err += ": ";
            err += static_cast<const char*>(error_blob->GetBufferPointer());
        }
        return false;
    }
    hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, out.put());
    if (FAILED(hr)) {
        SetHResultError(err, "CreatePixelShader(edit frame)", hr);
        return false;
    }
    return true;
}

} // namespace

bool EditFrameGpuConverter::Init(ID3D11Device* device, ID3D11DeviceContext* context, std::string& err) {
    if (device == nullptr || context == nullptr) {
        err = "EditFrameGpuConverter::Init invalid arguments";
        return false;
    }
    device_ = device;
    context_ = context;

    winrt::com_ptr<ID3DBlob> vs_blob;
    winrt::com_ptr<ID3DBlob> error_blob;
    HRESULT hr = D3DCompile(kVertexShaderSrc, std::strlen(kVertexShaderSrc), "edit_frame_vs", nullptr, nullptr, "main",
                            "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, vs_blob.put(), error_blob.put());
    if (FAILED(hr)) {
        SetHResultError(err, "D3DCompile(edit frame vertex shader)", hr);
        return false;
    }
    hr = device_->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr,
                                     vertex_shader_.put());
    if (FAILED(hr)) {
        SetHResultError(err, "CreateVertexShader(edit frame)", hr);
        return false;
    }

    // Both pixel shaders are compiled up front: which one a clip needs is known
    // only per frame, and a first-frame compile hitch is exactly the stutter
    // this render path exists to remove.
    if (!CompilePixelShader(device_, kYuvPixelShaderSrc, "D3DCompile(edit frame yuv pixel shader)", yuv_shader_, err)) {
        return false;
    }
    if (!CompilePixelShader(device_, kPqPixelShaderSrc, "D3DCompile(edit frame pq pixel shader)", pq_shader_, err)) {
        return false;
    }

    D3D11_BUFFER_DESC const_desc{};
    const_desc.Usage = D3D11_USAGE_DEFAULT;
    const_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    const_desc.ByteWidth = sizeof(GpuYuvConstants);
    hr = device_->CreateBuffer(&const_desc, nullptr, yuv_constants_.put());
    if (FAILED(hr)) {
        SetHResultError(err, "CreateBuffer(edit frame yuv constants)", hr);
        return false;
    }
    const_desc.ByteWidth = sizeof(GpuPqConstants);
    hr = device_->CreateBuffer(&const_desc, nullptr, pq_constants_.put());
    if (FAILED(hr)) {
        SetHResultError(err, "CreateBuffer(edit frame pq constants)", hr);
        return false;
    }

    constants_valid_ = false;
    return true;
}

bool EditFrameGpuConverter::UploadPlane(int index, const uint8_t* src, UINT src_stride_bytes, UINT width, UINT height,
                                        DXGI_FORMAT format, std::string& err) {
    PlaneTexture& plane = planes_[index];
    if (plane.texture == nullptr || plane.width != width || plane.height != height || plane.format != format) {
        plane.texture = nullptr;
        plane.srv = nullptr;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT; // written with UpdateSubresource, which honours the source row pitch
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = device_->CreateTexture2D(&desc, nullptr, plane.texture.put());
        if (FAILED(hr)) {
            SetHResultError(err, "CreateTexture2D(edit frame plane)", hr);
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv_desc.Format = format;
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MipLevels = 1;
        hr = device_->CreateShaderResourceView(plane.texture.get(), &srv_desc, plane.srv.put());
        if (FAILED(hr)) {
            SetHResultError(err, "CreateShaderResourceView(edit frame plane)", hr);
            plane.texture = nullptr;
            return false;
        }

        plane.width = width;
        plane.height = height;
        plane.format = format;
    }

    context_->UpdateSubresource(plane.texture.get(), 0, nullptr, src, src_stride_bytes, 0);
    return true;
}

bool EditFrameGpuConverter::Convert(const RawDecodedVideoFrame& frame, ID3D11Texture2D* dst, float hdr_peak_scale,
                                    std::string& err) {
    if (context_ == nullptr || device_ == nullptr || vertex_shader_ == nullptr || dst == nullptr || frame.width == 0 ||
        frame.height == 0 || frame.y_plane == nullptr || frame.u_plane == nullptr || frame.v_plane == nullptr) {
        err = "EditFrameGpuConverter::Convert called before Init or with invalid arguments";
        return false;
    }

    const bool ten_bit = frame.format == DecodedPixelFormat::Yuv420P10;
    // is_pq_source is only meaningful for the 10-bit 4:2:0 layout HDR10 clips
    // decode to (see RawDecodedVideoFrame); anything else takes the SDR path
    // rather than reading its planes as codes they are not.
    const bool pq = frame.is_pq_source && ten_bit;
    const bool subsampled = frame.format != DecodedPixelFormat::Yuv444P8;

    const DXGI_FORMAT plane_format = ten_bit ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R8_UINT;
    const UINT chroma_w = subsampled ? ((frame.width + 1u) / 2u) : frame.width;
    const UINT chroma_h = subsampled ? ((frame.height + 1u) / 2u) : frame.height;

    if (!UploadPlane(0, frame.y_plane, frame.y_stride_bytes, frame.width, frame.height, plane_format, err) ||
        !UploadPlane(1, frame.u_plane, frame.u_stride_bytes, chroma_w, chroma_h, plane_format, err) ||
        !UploadPlane(2, frame.v_plane, frame.v_stride_bytes, chroma_w, chroma_h, plane_format, err)) {
        return false;
    }

    if (!constants_valid_ || frame.matrix != last_matrix_ || frame.range != last_range_ ||
        frame.format != last_format_ || hdr_peak_scale != last_peak_scale_) {
        const GpuYuvConstants yc = YuvConstantsFor(frame.matrix, frame.range, frame.format);
        context_->UpdateSubresource(yuv_constants_.get(), 0, nullptr, &yc, 0, 0);
        GpuPqConstants pc{};
        pc.peak_scale = hdr_peak_scale;
        context_->UpdateSubresource(pq_constants_.get(), 0, nullptr, &pc, 0, 0);
        last_matrix_ = frame.matrix;
        last_range_ = frame.range;
        last_format_ = frame.format;
        last_peak_scale_ = hdr_peak_scale;
        constants_valid_ = true;
    }

    // Created per call rather than cached by texture pointer (HdrToneMapper's
    // rtv_cache_ pattern): dst is typically a swap-chain back buffer, and a
    // retained view would make ResizeBuffers fail. This interface has no
    // invalidation hook, and one CreateRenderTargetView per frame is noise next
    // to the draw it sets up.
    winrt::com_ptr<ID3D11RenderTargetView> rtv;
    const HRESULT hr = device_->CreateRenderTargetView(dst, nullptr, rtv.put());
    if (FAILED(hr)) {
        SetHResultError(err, "CreateRenderTargetView(edit frame dst)", hr);
        return false;
    }

    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(frame.width);
    viewport.Height = static_cast<float>(frame.height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    ID3D11RenderTargetView* rtv_raw = rtv.get();
    ID3D11Buffer* constants = pq ? pq_constants_.get() : yuv_constants_.get();
    ID3D11ShaderResourceView* srvs[3] = {planes_[0].srv.get(), planes_[1].srv.get(), planes_[2].srv.get()};

    context_->OMSetRenderTargets(1, &rtv_raw, nullptr);
    context_->RSSetViewports(1, &viewport);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertex_shader_.get(), nullptr, 0);
    context_->PSSetShader(pq ? pq_shader_.get() : yuv_shader_.get(), nullptr, 0);
    context_->PSSetConstantBuffers(0, 1, &constants);
    context_->PSSetShaderResources(0, 3, srvs);
    context_->Draw(3, 0);

    // Unbind so the plane textures can be re-uploaded and dst can immediately be
    // presented / copied without a read/write hazard.
    ID3D11ShaderResourceView* null_srvs[3] = {nullptr, nullptr, nullptr};
    ID3D11RenderTargetView* null_rtv = nullptr;
    context_->PSSetShaderResources(0, 3, null_srvs);
    context_->OMSetRenderTargets(1, &null_rtv, nullptr);
    return true;
}

} // namespace recorder_core
