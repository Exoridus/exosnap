#pragma once

#include "config_types.h"
#include "runtime_snapshot.h"
#include "support_level.h"

#include <exosnap/engine/codec_types.h>

#include <cstddef>
#include <string>
#include <unordered_map>

namespace exosnap::capability {

struct ComboKey {
    Container c;
    VideoCodec v;
    AudioCodec a;
    ChromaSubsampling cs;
    BitDepth bd;

    bool operator==(const ComboKey&) const noexcept = default;
};

struct ComboKeyHash {
    size_t operator()(const ComboKey& key) const noexcept;
};

// Per-codec NVENC B-frame capability: whether/how many B-frames the GPU and
// driver support for this codec, plus the B-frame-reference-mode capability
// bit (0/1/2, SDK semantics). Fail-closed: NotImplemented/0 baseline, upgraded
// only when a real per-GPU probe confirms support — see D1 of
// encoder-quality-features-spec.md (never an architecture/name heuristic).
struct BFrameCapability {
    SupportAnnotation annotation;
    int max_bframes = 0;
    int bframe_ref_mode = 0;
};

struct CapabilitySet {
    std::string gpu_adapter_name;
    bool nvenc_dll_present = false;
    bool mf_webcam_available = false; // S4: true when mfplat.dll is present (webcam subsystem usable)

    // True only when this set was produced by a real, just-completed hardware
    // probe (CapabilityBuilder::BuildFromHardwareQuery). False for the static
    // baseline and for a set rebuilt from a disk-cache warm-start snapshot
    // (CapabilityBuilder::BuildEffectiveCapabilities called directly on a
    // cached RuntimeCapabilitySnapshot). A recording-start decision must never
    // be authorized from a set with probed == false — see
    // RecordingCoordinator::OnCapabilitiesReady, which refuses one.
    bool probed = false;

    RuntimeCapabilitySnapshot runtime;

    std::unordered_map<Container, SupportAnnotation> containers;
    std::unordered_map<VideoCodec, SupportAnnotation> video_codecs;
    std::unordered_map<AudioCodec, SupportAnnotation> audio_codecs;
    std::unordered_map<ChromaSubsampling, SupportAnnotation> chroma_modes;
    std::unordered_map<BitDepth, SupportAnnotation> bit_depths;

    // Explicit per-codec HDR10-native (10-bit / P010, PQ/BT.2020) capability.
    // HEVC and AV1 carry HDR10; H.264 does not. This is a codec-format fact,
    // so gate against it instead of comparing codec names (the H.264+HDR10
    // pre-flight blocker, and a future expert HDR control).
    std::unordered_map<VideoCodec, SupportAnnotation> hdr10_native;

    // Explicit per-codec 4:4:4 (YUV444, 8-bit) encode capability. NVENC supports
    // 4:4:4 for H.264 (High 4:4:4 Predictive) and HEVC (Range Extensions), but
    // NOT AV1 (NVENC AV1 is 4:2:0 Main only). The static baseline advertises
    // H.264/HEVC as ValidUnvalidated and AV1 as NotImplemented; a real per-GPU
    // NVENC probe (NV_ENC_CAPS_SUPPORT_YUV444_ENCODE) downgrades H.264/HEVC when
    // the specific GPU cannot do it. Only consulted for the Cs444 chroma mode.
    std::unordered_map<VideoCodec, SupportAnnotation> chroma444;

    // Per-codec NVENC advanced-encode capabilities (B-frames, Lookahead,
    // Temporal-AQ). Fail-closed baseline (NotImplemented/0 for every codec);
    // a real probe upgrades exactly the codecs the GPU advertised support for.
    // Not part of QueryCombo — these are Expert-setting capabilities, not
    // container/codec/audio/chroma/bitdepth combo dimensions.
    std::unordered_map<VideoCodec, BFrameCapability> bframe_capability;
    std::unordered_map<VideoCodec, SupportAnnotation> lookahead;
    std::unordered_map<VideoCodec, SupportAnnotation> temporal_aq;

    std::unordered_map<ComboKey, SupportAnnotation, ComboKeyHash> combo_overrides;

    ResolutionConstraint resolution_constraint;

    SupportAnnotation QueryCombo(Container c, VideoCodec v, AudioCodec a, ChromaSubsampling cs, BitDepth bd) const;

    SupportAnnotation QueryContainer(Container c) const;
    SupportAnnotation QueryVideoCodec(VideoCodec v) const;
    SupportAnnotation QueryAudioCodec(AudioCodec a) const;
    SupportAnnotation QueryChroma(ChromaSubsampling cs) const;
    SupportAnnotation QueryBitDepth(BitDepth bd) const;

    // Whether `v` can carry a native HDR10 (10-bit/P010) signal. Selectable for
    // HEVC/AV1, NotImplemented for H.264.
    SupportAnnotation QueryHdr10Native(VideoCodec v) const;

    // Whether `v` can encode 8-bit 4:4:4 (YUV444). Selectable for H.264/HEVC when
    // the GPU supports it; NotImplemented for AV1. Consulted by QueryCombo for the
    // Cs444 chroma mode and by the expert chroma UI gate.
    SupportAnnotation QueryChroma444(VideoCodec v) const;

    // NVENC B-frame capability for `v`: max B-frames the GPU/driver reports
    // plus the B-ref-mode capability bit. NotImplemented/0 when unprobed or
    // when this GPU reported zero max B-frames for the codec (e.g. AV1 on
    // many current-generation NVENC parts).
    BFrameCapability QueryBFrames(VideoCodec v) const;

    // NVENC Lookahead / Temporal-AQ capability for `v`. NotImplemented when
    // unprobed or when the GPU/driver does not report the feature.
    SupportAnnotation QueryLookahead(VideoCodec v) const;
    SupportAnnotation QueryTemporalAq(VideoCodec v) const;

    // Query support for a canonical rate-control mode (ADR 0009).
    // Returns Available for CQ/VBR/CBR; NotImplemented for Lossless.
    // This is a static capability declaration (not runtime-probed per-session).
    SupportAnnotation QueryRateControlMode(exosnap::engine::RateControlMode mode) const;
};

} // namespace exosnap::capability
