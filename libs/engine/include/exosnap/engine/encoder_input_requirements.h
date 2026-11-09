#pragma once

#include <exosnap/engine/codec_types.h>
#include <exosnap/engine/output_geometry.h>

#include <cstdint>

namespace exosnap::engine {

enum class EncoderInputStorage { Unknown, D3D11Texture2D };
enum class EncoderInputFormat { Unknown, Nv12, P010, Ayuv };
enum class EncoderInputBind : uint32_t { None = 0, ShaderResource = 1, RenderTarget = 2, UnorderedAccess = 4 };

[[nodiscard]] constexpr EncoderInputBind operator|(EncoderInputBind left, EncoderInputBind right) noexcept {
    return static_cast<EncoderInputBind>(static_cast<uint32_t>(left) | static_cast<uint32_t>(right));
}

struct EncoderInputPool {
    uint32_t minimum = 0;
    uint32_t preferred = 0;
    uint32_t maximum = 0;
};

struct EncoderInputRequirements {
    EncoderInputStorage storage = EncoderInputStorage::Unknown;
    EncoderInputFormat format = EncoderInputFormat::Unknown;
    VideoCodec codec = VideoCodec::Av1;
    BitDepth bit_depth = BitDepth::Bit8;
    ChromaSubsampling chroma = ChromaSubsampling::Cs420;
    FrameSize allocation;
    ContentRect visible;
    FrameSize alignment{1, 1};
    EncoderInputBind required_bind = EncoderInputBind::None;
    EncoderInputBind allowed_bind = EncoderInputBind::None;
    // Native resource flags for storage. The current host creates no shared,
    // tiled or protected encoder inputs and rejects nonzero values.
    uint32_t misc_flags = 0;
    uint32_t mip_levels = 1;
    uint32_t array_size = 1;
    uint32_t subresource = 0;
    EncoderInputPool pool;
    // Borrowed native device passed to Open. It and the host-owned textures
    // outlive registration, pending submissions and encoder teardown.
    const void* device = nullptr;
};

// Facts inspected from the actual resource at registration, not inferred from
// the requested codec or trusted as a replacement for native resource queries.
struct EncoderInputSurfaceDesc {
    EncoderInputStorage storage = EncoderInputStorage::Unknown;
    EncoderInputFormat format = EncoderInputFormat::Unknown;
    FrameSize allocation;
    EncoderInputBind bind = EncoderInputBind::None;
    uint32_t misc_flags = 0;
    uint32_t mip_levels = 1;
    uint32_t array_size = 1;
    uint32_t subresource = 0;
    uint32_t sample_count = 1;
    uint32_t sample_quality = 0;
    bool gpu_default_usage = true;
    uint32_t cpu_access_flags = 0;
    const void* device = nullptr;
};

enum class EncoderInputError {
    None,
    UnsupportedStorage,
    UnsupportedFormat,
    ColorMismatch,
    ConfigurationMismatch,
    InvalidGeometry,
    InvalidAlignment,
    InvalidPool,
    WrongDevice,
    IncompatibleBindings,
    UnsupportedResourceLayout,
};

struct EncoderInputValidation {
    EncoderInputError code = EncoderInputError::None;
    const char* message = "";
    [[nodiscard]] explicit operator bool() const noexcept {
        return code == EncoderInputError::None;
    }
};

[[nodiscard]] constexpr bool HasEncoderInputBindings(EncoderInputBind available, EncoderInputBind required) noexcept {
    return (static_cast<uint32_t>(available) & static_cast<uint32_t>(required)) == static_cast<uint32_t>(required);
}

// These limits bound the host's D3D11 allocation path. Backend-specific codec
// limits are still checked by native initialization.
inline constexpr uint32_t kMaxEncoderInputDimension = 16384;
inline constexpr uint32_t kMaxEncoderInputSlots = 128;

[[nodiscard]] inline EncoderInputValidation
ValidateEncoderInputRequirements(const EncoderInputRequirements& input) noexcept {
    if (input.storage != EncoderInputStorage::D3D11Texture2D)
        return {EncoderInputError::UnsupportedStorage, "Encoder input storage is unavailable or unsupported."};
    BitDepth depth;
    ChromaSubsampling chroma;
    switch (input.format) {
    case EncoderInputFormat::Nv12:
        depth = BitDepth::Bit8;
        chroma = ChromaSubsampling::Cs420;
        break;
    case EncoderInputFormat::P010:
        depth = BitDepth::Bit10;
        chroma = ChromaSubsampling::Cs420;
        break;
    case EncoderInputFormat::Ayuv:
        depth = BitDepth::Bit8;
        chroma = ChromaSubsampling::Cs444;
        break;
    default:
        return {EncoderInputError::UnsupportedFormat, "Encoder native input format is unavailable or unsupported."};
    }
    if (input.bit_depth != depth || input.chroma != chroma)
        return {EncoderInputError::ColorMismatch, "Encoder input format does not match its bit depth and chroma."};
    if (input.codec != VideoCodec::H264 && input.codec != VideoCodec::Hevc && input.codec != VideoCodec::Av1)
        return {EncoderInputError::ConfigurationMismatch, "Encoder input declares an unknown codec."};
    if (!input.allocation.IsValid() || input.allocation.width > kMaxEncoderInputDimension ||
        input.allocation.height > kMaxEncoderInputDimension)
        return {EncoderInputError::InvalidGeometry, "Encoder input allocation dimensions are outside D3D11 limits."};
    if (!input.alignment.IsValid() || input.allocation.width % input.alignment.width != 0 ||
        input.allocation.height % input.alignment.height != 0 ||
        (input.chroma == ChromaSubsampling::Cs420 &&
         ((input.allocation.width % 2) != 0 || (input.allocation.height % 2) != 0)))
        return {EncoderInputError::InvalidAlignment, "Encoder input allocation does not satisfy its alignment."};
    // Subtraction after checking the origin avoids overflow in x + width.
    if (input.visible.width == 0 || input.visible.height == 0 || input.visible.x > input.allocation.width ||
        input.visible.y > input.allocation.height || input.visible.width > input.allocation.width - input.visible.x ||
        input.visible.height > input.allocation.height - input.visible.y)
        return {EncoderInputError::InvalidGeometry, "Encoder visible rectangle is outside its allocation."};
    if (input.pool.minimum == 0 || input.pool.minimum > input.pool.preferred ||
        input.pool.preferred > input.pool.maximum || input.pool.maximum > kMaxEncoderInputSlots)
        return {EncoderInputError::InvalidPool, "Encoder input pool bounds are invalid or exceed the host limit."};
    if (!input.device)
        return {EncoderInputError::WrongDevice, "Encoder input has no configured native device."};
    constexpr auto known_bind =
        EncoderInputBind::ShaderResource | EncoderInputBind::RenderTarget | EncoderInputBind::UnorderedAccess;
    if (!HasEncoderInputBindings(known_bind, input.allowed_bind) ||
        !HasEncoderInputBindings(input.allowed_bind, input.required_bind))
        return {EncoderInputError::IncompatibleBindings, "Encoder input binding requirements are incompatible."};
    if (input.mip_levels == 0 || input.array_size == 0 ||
        input.subresource >= static_cast<uint64_t>(input.mip_levels) * input.array_size)
        return {EncoderInputError::UnsupportedResourceLayout, "Encoder input subresource does not exist."};
    return {};
}

// The current converters write one unpadded, complete texture. Reject a valid
// backend layout that needs a different allocator/converter before allocating.
[[nodiscard]] inline EncoderInputValidation ValidateEncoderInputForHost(const EncoderInputRequirements& input,
                                                                        VideoCodec codec, BitDepth depth,
                                                                        ChromaSubsampling chroma, FrameSize requested,
                                                                        int32_t slots, const void* device) noexcept {
    const auto valid = ValidateEncoderInputRequirements(input);
    if (!valid)
        return valid;
    if (input.codec != codec || input.bit_depth != depth || input.chroma != chroma ||
        input.visible.width != requested.width || input.visible.height != requested.height)
        return {EncoderInputError::ConfigurationMismatch, "Encoder input does not match the configured output."};
    if (slots <= 0 || static_cast<uint32_t>(slots) < input.pool.minimum ||
        static_cast<uint32_t>(slots) > input.pool.maximum)
        return {EncoderInputError::InvalidPool, "Encoder slot count is outside its declared pool bounds."};
    if (input.device != device)
        return {EncoderInputError::WrongDevice, "Encoder inputs must use the configured host device."};
    if (input.visible.x != 0 || input.visible.y != 0 || input.allocation.width != input.visible.width ||
        input.allocation.height != input.visible.height)
        return {EncoderInputError::UnsupportedResourceLayout,
                "Host encoder conversion requires an unpadded visible surface."};
    if (input.mip_levels != 1 || input.array_size != 1 || input.subresource != 0 || input.misc_flags != 0)
        return {EncoderInputError::UnsupportedResourceLayout,
                "Host encoder conversion requires a single unshared texture subresource."};
    const auto producer_bind =
        input.format == EncoderInputFormat::Ayuv ? EncoderInputBind::UnorderedAccess : EncoderInputBind::RenderTarget;
    if (!HasEncoderInputBindings(input.required_bind, producer_bind))
        return {EncoderInputError::IncompatibleBindings,
                "Encoder input does not permit the host converter's write access."};
    return {};
}

[[nodiscard]] inline EncoderInputValidation
ValidateEncoderInputSurface(const EncoderInputRequirements& input, const EncoderInputSurfaceDesc& surface) noexcept {
    const auto valid = ValidateEncoderInputRequirements(input);
    if (!valid)
        return valid;
    if (surface.storage != input.storage)
        return {EncoderInputError::UnsupportedStorage, "Encoder input resource uses the wrong native storage."};
    if (surface.format != input.format)
        return {EncoderInputError::UnsupportedFormat, "Encoder input texture has the wrong native format."};
    if (surface.allocation.width != input.allocation.width || surface.allocation.height != input.allocation.height)
        return {EncoderInputError::InvalidGeometry, "Encoder input texture has the wrong allocation dimensions."};
    if (surface.device != input.device)
        return {EncoderInputError::WrongDevice, "Encoder input texture belongs to a different device."};
    if (!HasEncoderInputBindings(surface.bind, input.required_bind) ||
        !HasEncoderInputBindings(input.allowed_bind, surface.bind))
        return {EncoderInputError::IncompatibleBindings, "Encoder input texture has incompatible bind flags."};
    if (surface.mip_levels != input.mip_levels || surface.array_size != input.array_size ||
        surface.subresource != input.subresource || surface.misc_flags != input.misc_flags ||
        surface.sample_count != 1 || surface.sample_quality != 0 || !surface.gpu_default_usage ||
        surface.cpu_access_flags != 0)
        return {EncoderInputError::UnsupportedResourceLayout,
                "Encoder input texture has an incompatible resource layout or access mode."};
    return {};
}

} // namespace exosnap::engine
