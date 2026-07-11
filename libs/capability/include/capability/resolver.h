#pragma once

#include "capability_set.h"
#include "user_config.h"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace exosnap::capability {

struct Adjustment {
    std::string field;
    std::string from;
    std::string to;
    std::string reason;
};

struct Warning {
    std::string code;
    std::string message;
};

struct InvalidReason {
    std::string field;
    std::string message;
};

struct ResolveResult {
    bool succeeded = false;
    UserRecorderConfig resolved_config;
    std::vector<Adjustment> adjustments;
    std::vector<Warning> warnings;
    std::vector<InvalidReason> invalidity;
};

class RequestedChange {
  public:
    struct ResolutionValue {
        uint32_t width = 0;
        uint32_t height = 0;
    };

    using Value = std::variant<Container, VideoCodec, AudioCodec, ChromaSubsampling, BitDepth, ResolutionValue>;

    static RequestedChange ForContainer(Container value);
    static RequestedChange ForVideoCodec(VideoCodec value);
    static RequestedChange ForAudioCodec(AudioCodec value);
    static RequestedChange ForChroma(ChromaSubsampling value);
    static RequestedChange ForBitDepth(BitDepth value);
    static RequestedChange ForResolution(uint32_t width, uint32_t height);

    const Value& value() const noexcept;

  private:
    explicit RequestedChange(Value value);

    Value value_;
};

class SettingsResolver {
  public:
    explicit SettingsResolver(const CapabilitySet& caps);

    ResolveResult ResolveChange(const UserRecorderConfig& current, const RequestedChange& change) const;

    ResolveResult ValidateConfig(const UserRecorderConfig& config) const;

  private:
    const CapabilitySet& caps_;
};

// ---------------------------------------------------------------------------
// Static output-format reconciliation
// ---------------------------------------------------------------------------
//
// The resolver owns every reconciliation rule that does not need a probed
// CapabilitySet: container × codec compatibility (ADR 0010), the 10-bit
// bit-depth demotion (ADR 0032), the 4:4:4 chroma snap, and the MP4 CFR
// timing constraint. App-layer callers (settings intake, preset sanitizing)
// call ReconcileOutputFormat and copy the answer — they must not re-implement
// any of these rules. The capability-gated fallbacks (SettingsResolver above)
// then validate the already-reconciled config against the probed hardware.

// 10-bit (HEVC Main10 / AV1 10-bit P010) is valid only for HEVC and AV1,
// never H.264 — the same static rule translation.cpp enforces.
[[nodiscard]] constexpr bool CodecSupports10Bit(VideoCodec codec) noexcept {
    return codec == VideoCodec::HevcNvenc || codec == VideoCodec::Av1Nvenc;
}

// 4:4:4 is an 8-bit H.264/HEVC-only expert path (AV1 NVENC is 4:2:0 only).
[[nodiscard]] constexpr bool CodecSupportsChroma444(VideoCodec codec) noexcept {
    return codec == VideoCodec::H264Nvenc || codec == VideoCodec::HevcNvenc;
}

// The raw wish as the user stored it, before any rule is applied.
struct OutputFormatRequest {
    Container container = Container::Matroska;
    VideoCodec video_codec = VideoCodec::Av1Nvenc;
    AudioCodec audio_codec = AudioCodec::Opus;
    BitDepth bit_depth = BitDepth::Bit8;
    ChromaSubsampling chroma = ChromaSubsampling::Cs420;
    // Constant-frame-rate timing. Not part of UserRecorderConfig (it lives in
    // the video settings), but the MP4 mux path is fixed-rate, so the timing
    // constraint is a container rule and belongs here.
    bool cfr = true;
};

struct OutputFormatReconciliation {
    // The fully reconciled format; every field is safe to hand to validation.
    OutputFormatRequest resolved;
    // Which rules fired, so a caller can surface the decision (log/notice)
    // without re-deriving it.
    bool codecs_adjusted = false;   // container forced the video and/or audio codec
    bool bit_depth_demoted = false; // 10-bit fell back to 8-bit
    bool chroma_snapped = false;    // 4:4:4 fell back to 4:2:0
    bool cfr_forced = false;        // MP4 forced VFR timing to CFR
};

// Applies the static rules in canonical order: (1) container × codec
// compatibility via the ADR 0010 registry, (2) bit-depth demotion — after the
// container rule so a container-forced H.264 also demotes a stored 10-bit
// selection, (3) chroma snap — after the bit-depth rule so an 8-bit-demoted
// selection can still keep 4:4:4, (4) MP4 forces CFR. Pure and idempotent.
[[nodiscard]] OutputFormatReconciliation ReconcileOutputFormat(OutputFormatRequest request) noexcept;

} // namespace exosnap::capability
