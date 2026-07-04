#pragma once

#include "config_types.h"
#include "runtime_snapshot.h"
#include "support_level.h"

#include <recorder_core/codec_types.h>

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

struct CapabilitySet {
    std::string gpu_adapter_name;
    bool nvenc_dll_present = false;
    bool mf_aac_available = false;
    bool mf_webcam_available = false; // S4: true when mfplat.dll is present (webcam subsystem usable)

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

    // Query support for a canonical rate-control mode (ADR 0009).
    // Returns Available for CQ/VBR/CBR; NotImplemented for Lossless.
    // This is a static capability declaration (not runtime-probed per-session).
    SupportAnnotation QueryRateControlMode(recorder_core::RateControlMode mode) const;
};

} // namespace exosnap::capability
