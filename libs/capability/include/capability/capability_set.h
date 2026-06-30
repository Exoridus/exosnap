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

    std::unordered_map<ComboKey, SupportAnnotation, ComboKeyHash> combo_overrides;

    ResolutionConstraint resolution_constraint;

    SupportAnnotation QueryCombo(Container c, VideoCodec v, AudioCodec a, ChromaSubsampling cs, BitDepth bd) const;

    SupportAnnotation QueryContainer(Container c) const;
    SupportAnnotation QueryVideoCodec(VideoCodec v) const;
    SupportAnnotation QueryAudioCodec(AudioCodec a) const;
    SupportAnnotation QueryChroma(ChromaSubsampling cs) const;
    SupportAnnotation QueryBitDepth(BitDepth bd) const;

    // Query support for a canonical rate-control mode (ADR 0009).
    // Returns Available for CQ/VBR/CBR; NotImplemented for Lossless.
    // This is a static capability declaration (not runtime-probed per-session).
    SupportAnnotation QueryRateControlMode(recorder_core::RateControlMode mode) const;
};

} // namespace exosnap::capability
