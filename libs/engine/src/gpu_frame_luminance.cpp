#include "gpu_frame_luminance.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace exosnap::engine {
namespace {

// Threadgroup edge. 256 threads is the largest group every feature-level-11
// device guarantees for a 2D layout, and one thread per pixel keeps the shader
// free of an inner loop whose bounds would have to be reasoned about.
constexpr UINT kThreadGroupEdge = 16;

// Result buffer layout, in 32-bit words:
//   0  maximum luminance, IEEE bits          (InterlockedMax)
//   1  minimum luminance, bit complement     (InterlockedMax; see below)
//   2  fixed-point luminance sum             (InterlockedAdd)
//   3  padding, so the histogram starts on a 16-byte boundary
//   4.. histogram counts                     (InterlockedAdd)
constexpr UINT kResultWordMax = 0;
constexpr UINT kResultWordMinComplement = 1;
constexpr UINT kResultWordSum = 2;
constexpr UINT kResultWordHistogram = 4;
constexpr UINT kResultWordCount = kResultWordHistogram + kFrameLuminanceHistogramBins;

// Depth of the readback ring. Three is what keeps a dispatch from ever waiting:
// one copy is in flight, one has landed and is waiting to be taken, and one is
// free for this frame.
constexpr size_t kReadbackSlots = 3;

// Sub-nit resolution of the per-pixel fixed-point luminance sum. The
// threadgroup accumulator holds 256 pixels at this scale, which at the FP16
// ceiling is 2.7e8 -- an order of magnitude under what a 32-bit atomic holds.
constexpr float kPixelSumScale = 16.0f;

// Largest luminance a source pixel can carry: the greatest finite value an FP16
// channel represents. Everything the clamp touches is already far beyond any
// display and beyond the 16-bit ceiling CTA-861.3 codes MaxCLL in.
constexpr float kClampNits = 65504.0f;

// Headroom target for the single 32-bit global luminance accumulator. The
// divisor is sized against this rather than against 2^32 so the accumulator
// cannot reach its ceiling even with every per-group rounding going the same way.
constexpr double kGlobalSumBudget = 4.0e9;

const char* kComputeShaderSrc = R"(
Texture2D<float4> srcTex : register(t0);
RWByteAddressBuffer result : register(u0);

cbuffer LuminanceConstants : register(b0) {
    uint2 gSize;      // analysed region; threads outside contribute nothing
    uint gSumDivisor; // scales each group's sum into the global accumulator
    uint gPqSource;   // 1 when the source is PQ-encoded BT.2020 rather than scRGB
};

static const uint kBins = 64;
static const float kMinLog2 = -7.0;
static const float kLog2Step = 21.0 / 64.0;
static const float kPixelSumScale = 16.0;
static const float kClampNits = 65504.0;
static const float kRefWhiteNits = 80.0;

static const float kPqM1 = 2610.0 / 16384.0;
static const float kPqM2 = 2523.0 / 4096.0 * 128.0;
static const float kPqC1 = 3424.0 / 4096.0;
static const float kPqC2 = 2413.0 / 4096.0 * 32.0;
static const float kPqC3 = 2392.0 / 4096.0 * 32.0;

// ST 2084 EOTF: PQ code (0..1) -> linear light in scRGB units (1.0 == 80 nits).
float PqToScRgb(float n) {
    n = saturate(n);
    float p = pow(n, 1.0 / kPqM2);
    float l = pow(max(p - kPqC1, 0.0) / (kPqC2 - kPqC3 * p), 1.0 / kPqM1);
    return l * 10000.0 / 80.0;
}

float3 Bt2020ToBt709(float3 c) {
    return float3(1.6605 * c.r - 0.5876 * c.g - 0.0728 * c.b,
                  -0.1246 * c.r + 1.1329 * c.g - 0.0083 * c.b,
                  -0.0182 * c.r - 0.1006 * c.g + 1.1187 * c.b);
}

groupshared uint sMax;
groupshared uint sMinComplement;
groupshared uint sSum;
groupshared uint sHistogram[kBins];

[numthreads(16, 16, 1)]
void main(uint3 dtid : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    if (gi == 0) {
        sMax = 0;
        sMinComplement = 0;
        sSum = 0;
    }
    if (gi < kBins) {
        sHistogram[gi] = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    if (dtid.x < gSize.x && dtid.y < gSize.y) {
        float3 c = srcTex.Load(int3(int2(dtid.xy), 0)).rgb;
        if (gPqSource != 0) {
            c = Bt2020ToBt709(float3(PqToScRgb(c.r), PqToScRgb(c.g), PqToScRgb(c.b)));
        }
        // Wide-gamut negatives are clamped before the projection, the same way
        // the tone-map shader clamps them, so the luminance is never negative and
        // the bit reinterpretation below stays monotonic.
        c = max(c, 0.0);
        float lum = min(dot(c, float3(0.2126, 0.7152, 0.0722)) * kRefWhiteNits, kClampNits);

        // InterlockedMin/Max exist for integers only. For a non-negative float
        // the asuint reinterpretation is order-preserving, so a maximum is a
        // maximum of the bit patterns. The minimum is taken as a maximum of the
        // complements instead of an InterlockedMin, so that zero -- the value the
        // buffer is cleared to before every dispatch -- is the correct identity
        // for both reductions and one clear covers the whole buffer.
        uint bits = asuint(lum);
        InterlockedMax(sMax, bits);
        InterlockedMax(sMinComplement, ~bits);
        InterlockedAdd(sSum, (uint)(lum * kPixelSumScale + 0.5));

        uint bin = 0;
        if (lum > 0.0) {
            float t = (log2(lum) - kMinLog2) / kLog2Step;
            bin = t <= 0.0 ? 0 : min((uint)t, kBins - 1);
        }
        InterlockedAdd(sHistogram[bin], 1);
    }
    GroupMemoryBarrierWithGroupSync();

    if (gi == 0) {
        result.InterlockedMax(0, sMax);
        result.InterlockedMax(4, sMinComplement);
        result.InterlockedAdd(8, (sSum + gSumDivisor / 2) / gSumDivisor);
    }
    if (gi < kBins && sHistogram[gi] != 0) {
        result.InterlockedAdd(16 + gi * 4, sHistogram[gi]);
    }
}
)";

struct LuminanceConstants {
    uint32_t size[2];
    uint32_t sum_divisor;
    uint32_t pq_source;
};

void SetHResultError(std::string& err, const char* what, HRESULT hr) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s failed 0x%08lX", what, static_cast<unsigned long>(hr));
    err = buf;
}

float BitsToNits(uint32_t bits) noexcept {
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

} // namespace

bool FrameLuminanceAnalyzer::Init(ID3D11Device* device, ID3D11DeviceContext* context, UINT width, UINT height,
                                  bool pq_source, std::string& err) {
    if (device == nullptr || context == nullptr || width == 0 || height == 0) {
        err = "FrameLuminanceAnalyzer::Init invalid arguments";
        return false;
    }
    Reset();

    device_ = device;
    context_ = context;
    width_ = width;
    height_ = height;
    pq_source_ = pq_source;

    // The worst case this has to survive is a frame in which every pixel sits at
    // the clamp: the global accumulator then receives width * height * clamp *
    // scale fixed-point units, which is several orders of magnitude past a 32-bit
    // integer at 4K. Dividing each group's contribution by this factor brings the
    // total inside the budget for any surface size, and the CPU multiplies it
    // back out when it forms the mean -- so the only cost is a quantisation far
    // below a nit.
    const double worst_case = static_cast<double>(width) * static_cast<double>(height) *
                              static_cast<double>(kClampNits) * static_cast<double>(kPixelSumScale);
    sum_divisor_ = static_cast<uint32_t>((std::max)(1.0, std::ceil(worst_case / kGlobalSumBudget)));

    winrt::com_ptr<ID3DBlob> cs_blob;
    winrt::com_ptr<ID3DBlob> error_blob;
    HRESULT hr = D3DCompile(kComputeShaderSrc, std::strlen(kComputeShaderSrc), "frame_luminance_cs", nullptr, nullptr,
                            "main", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, cs_blob.put(), error_blob.put());
    if (FAILED(hr)) {
        SetHResultError(err, "D3DCompile(frame-luminance compute shader)", hr);
        if (error_blob != nullptr && error_blob->GetBufferPointer() != nullptr) {
            err += ": ";
            err.append(static_cast<const char*>(error_blob->GetBufferPointer()));
        }
        Reset();
        return false;
    }
    hr = device_->CreateComputeShader(cs_blob->GetBufferPointer(), cs_blob->GetBufferSize(), nullptr,
                                      compute_shader_.put());
    if (FAILED(hr)) {
        SetHResultError(err, "CreateComputeShader(frame-luminance)", hr);
        Reset();
        return false;
    }

    LuminanceConstants lc{};
    lc.size[0] = width_;
    lc.size[1] = height_;
    lc.sum_divisor = sum_divisor_;
    lc.pq_source = pq_source_ ? 1u : 0u;
    D3D11_BUFFER_DESC const_desc{};
    const_desc.ByteWidth = sizeof(LuminanceConstants);
    const_desc.Usage = D3D11_USAGE_DEFAULT;
    const_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA const_data{};
    const_data.pSysMem = &lc;
    hr = device_->CreateBuffer(&const_desc, &const_data, constants_.put());
    if (FAILED(hr)) {
        SetHResultError(err, "CreateBuffer(frame-luminance constants)", hr);
        Reset();
        return false;
    }

    D3D11_BUFFER_DESC result_desc{};
    result_desc.ByteWidth = kResultWordCount * sizeof(uint32_t);
    result_desc.Usage = D3D11_USAGE_DEFAULT;
    result_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    result_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    hr = device_->CreateBuffer(&result_desc, nullptr, result_.put());
    if (FAILED(hr)) {
        SetHResultError(err, "CreateBuffer(frame-luminance result)", hr);
        Reset();
        return false;
    }

    D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
    uav_desc.Format = DXGI_FORMAT_R32_TYPELESS;
    uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uav_desc.Buffer.NumElements = kResultWordCount;
    uav_desc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    hr = device_->CreateUnorderedAccessView(result_.get(), &uav_desc, result_uav_.put());
    if (FAILED(hr)) {
        SetHResultError(err, "CreateUnorderedAccessView(frame-luminance result)", hr);
        Reset();
        return false;
    }

    D3D11_BUFFER_DESC staging_desc{};
    staging_desc.ByteWidth = result_desc.ByteWidth;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    D3D11_QUERY_DESC query_desc{};
    query_desc.Query = D3D11_QUERY_EVENT;

    readback_.resize(kReadbackSlots);
    for (ReadbackSlot& slot : readback_) {
        hr = device_->CreateBuffer(&staging_desc, nullptr, slot.staging.put());
        if (FAILED(hr)) {
            SetHResultError(err, "CreateBuffer(frame-luminance staging)", hr);
            Reset();
            return false;
        }
        hr = device_->CreateQuery(&query_desc, slot.fence.put());
        if (FAILED(hr)) {
            SetHResultError(err, "CreateQuery(frame-luminance fence)", hr);
            Reset();
            return false;
        }
    }
    return true;
}

void FrameLuminanceAnalyzer::Reset() noexcept {
    srv_cache_.clear();
    readback_.clear();
    readback_write_ = 0;
    readback_read_ = 0;
    readback_inflight_ = 0;
    result_uav_ = nullptr;
    result_ = nullptr;
    constants_ = nullptr;
    compute_shader_ = nullptr;
    device_ = nullptr;
    context_ = nullptr;
    width_ = 0;
    height_ = 0;
    sum_divisor_ = 1;
}

ID3D11ShaderResourceView* FrameLuminanceAnalyzer::SrvFor(ID3D11Texture2D* tex, std::string& err) {
    auto it = srv_cache_.find(tex);
    if (it != srv_cache_.end()) {
        return it->second.get();
    }
    D3D11_TEXTURE2D_DESC tex_desc{};
    tex->GetDesc(&tex_desc);
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = tex_desc.Format;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;
    winrt::com_ptr<ID3D11ShaderResourceView> srv;
    const HRESULT hr = device_->CreateShaderResourceView(tex, &srv_desc, srv.put());
    if (FAILED(hr)) {
        SetHResultError(err, "CreateShaderResourceView(frame-luminance src)", hr);
        return nullptr;
    }
    ID3D11ShaderResourceView* raw = srv.get();
    srv_cache_.emplace(tex, std::move(srv));
    return raw;
}

bool FrameLuminanceAnalyzer::Dispatch(ID3D11Texture2D* src, std::string& err) {
    if (!Initialised() || src == nullptr) {
        err = "FrameLuminanceAnalyzer::Dispatch called before Init or with a null texture";
        return false;
    }
    if (readback_inflight_ >= readback_.size()) {
        // Nobody is collecting results. Measuring a frame whose numbers have
        // nowhere to land would cost the dispatch for nothing.
        return true;
    }

    ID3D11ShaderResourceView* srv = SrvFor(src, err);
    if (srv == nullptr) {
        return false;
    }

    const UINT zeros[4] = {0, 0, 0, 0};
    ID3D11UnorderedAccessView* uav = ResultUav();
    context_->ClearUnorderedAccessViewUint(uav, zeros);

    ID3D11Buffer* constants = constants_.get();
    context_->CSSetShader(compute_shader_.get(), nullptr, 0);
    context_->CSSetConstantBuffers(0, 1, &constants);
    context_->CSSetShaderResources(0, 1, &srv);
    context_->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    context_->Dispatch((width_ + kThreadGroupEdge - 1) / kThreadGroupEdge,
                       (height_ + kThreadGroupEdge - 1) / kThreadGroupEdge, 1);

    // Unbind before the copy so the source texture can immediately be a render
    // target or a copy destination again, and so the UAV is free for the next
    // clear.
    ID3D11ShaderResourceView* null_srv = nullptr;
    ID3D11UnorderedAccessView* null_uav = nullptr;
    context_->CSSetShaderResources(0, 1, &null_srv);
    context_->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
    context_->CSSetShader(nullptr, nullptr, 0);

    ReadbackSlot& slot = readback_[readback_write_];
    context_->CopyResource(slot.staging.get(), result_.get());
    context_->End(slot.fence.get());
    slot.pending = true;
    readback_write_ = (readback_write_ + 1) % readback_.size();
    ++readback_inflight_;
    return true;
}

bool FrameLuminanceAnalyzer::TryTakeResult(FrameLuminanceStats* out) {
    if (out == nullptr || !Initialised() || readback_inflight_ == 0) {
        return false;
    }
    ReadbackSlot& slot = readback_[readback_read_];
    if (!slot.pending) {
        return false;
    }
    // DONOTFLUSH: asking the driver to flush here would turn a status poll into a
    // submission, which is exactly the stall this ring exists to avoid.
    if (context_->GetData(slot.fence.get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
        return false;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    // DO_NOT_WAIT even though the fence has signalled: a map that can return
    // "still drawing" can never block the capture thread, whatever the driver
    // makes of the query.
    const HRESULT hr = context_->Map(slot.staging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        return false;
    }
    if (FAILED(hr) || mapped.pData == nullptr) {
        // The copy is unrecoverable, but it is one frame's measurement: retire
        // the slot rather than wedging the ring on it.
        slot.pending = false;
        readback_read_ = (readback_read_ + 1) % readback_.size();
        --readback_inflight_;
        return false;
    }

    std::array<uint32_t, kResultWordCount> words{};
    std::memcpy(words.data(), mapped.pData, words.size() * sizeof(uint32_t));
    context_->Unmap(slot.staging.get(), 0);
    slot.pending = false;
    readback_read_ = (readback_read_ + 1) % readback_.size();
    --readback_inflight_;

    FrameLuminanceStats stats;
    stats.max_nits = BitsToNits(words[kResultWordMax]);
    stats.min_nits = BitsToNits(~words[kResultWordMinComplement]);
    const double pixels = static_cast<double>(width_) * static_cast<double>(height_);
    stats.mean_nits = static_cast<float>(static_cast<double>(words[kResultWordSum]) *
                                         static_cast<double>(sum_divisor_) / (kPixelSumScale * pixels));
    for (int bin = 0; bin < kFrameLuminanceHistogramBins; ++bin) {
        stats.histogram[static_cast<size_t>(bin)] = words[kResultWordHistogram + static_cast<size_t>(bin)];
    }
    *out = stats;
    return true;
}

} // namespace exosnap::engine
