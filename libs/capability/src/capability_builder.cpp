#include <capability/capability_builder.h>
#include <capability/runtime_snapshot.h>

#include <string>

namespace exosnap::capability {

CapabilitySet CapabilityBuilder::BuildStaticValidatedBaseline() {
    CapabilitySet caps;
    caps.gpu_adapter_name = "validated-baseline-static";
    caps.nvenc_dll_present = true;
    caps.mf_aac_available = true;

    caps.containers.emplace(Container::Matroska,
                            SupportAnnotation{SupportLevel::Available, "Primary validated container."});
    caps.containers.emplace(Container::Mp4,
                            SupportAnnotation{SupportLevel::NotImplemented,
                                              "MP4 product surface exists but runtime path is not implemented."});
    caps.containers.emplace(Container::WebM,
                            SupportAnnotation{SupportLevel::NotImplemented,
                                              "WebM product surface exists but runtime path is not implemented."});

    caps.video_codecs.emplace(VideoCodec::Av1Nvenc,
                              SupportAnnotation{SupportLevel::Available, "Validated NVENC AV1 path."});
    caps.video_codecs.emplace(VideoCodec::HevcNvenc,
                              SupportAnnotation{SupportLevel::NotImplemented,
                                                "HEVC product surface exists but runtime path is not implemented."});
    caps.video_codecs.emplace(VideoCodec::H264Nvenc,
                              SupportAnnotation{SupportLevel::NotImplemented,
                                                "H.264 product surface exists but runtime path is not implemented."});

    caps.audio_codecs.emplace(AudioCodec::Opus,
                              SupportAnnotation{SupportLevel::NotImplemented,
                                                "Opus product surface exists but runtime path is not implemented."});
    caps.audio_codecs.emplace(AudioCodec::AacMf,
                              SupportAnnotation{SupportLevel::Available, "Validated AAC-LC Media Foundation path."});
    caps.audio_codecs.emplace(AudioCodec::Pcm,
                              SupportAnnotation{SupportLevel::NotImplemented, "PCM path is not implemented."});

    caps.chroma_modes.emplace(ChromaSubsampling::Cs420,
                              SupportAnnotation{SupportLevel::Available, "Validated chroma mode."});
    caps.chroma_modes.emplace(ChromaSubsampling::Cs422,
                              SupportAnnotation{SupportLevel::NotImplemented, "4:2:2 path is not implemented."});
    caps.chroma_modes.emplace(ChromaSubsampling::Cs444,
                              SupportAnnotation{SupportLevel::NotImplemented, "4:4:4 path is not implemented."});

    caps.bit_depths.emplace(BitDepth::Bit8, SupportAnnotation{SupportLevel::Available, "Validated bit depth."});
    caps.bit_depths.emplace(BitDepth::Bit10,
                            SupportAnnotation{SupportLevel::NotImplemented, "10-bit path is not implemented."});

    caps.resolution_constraint.max_width = 0;
    caps.resolution_constraint.max_height = 0;
    caps.resolution_constraint.must_be_even = true;

    return caps;
}

CapabilitySet CapabilityBuilder::BuildEffectiveCapabilities(const RuntimeCapabilitySnapshot& snapshot) {
    CapabilitySet caps = BuildStaticValidatedBaseline();
    caps.runtime = snapshot;

    // Propagate best-effort metadata from snapshot into legacy fields.
    caps.gpu_adapter_name = snapshot.nvidia.adapter_name;
    caps.nvenc_dll_present = snapshot.nvidia.nvenc_dll_present;
    caps.mf_aac_available = snapshot.mf_aac.available();

    // --- Downgrade rule A: missing NVENC blocks AV1 path ---
    // NVENC is required when the DLL is not present or API version is not valid.
    if (!snapshot.nvidia.nvenc_dll_present || !snapshot.nvidia.nvenc_api_version_valid) {
        const std::string nvenc_reason = "NVENC unavailable: " + (snapshot.nvidia.failure_detail.empty()
                                                                      ? "DLL not present or API version not confirmed"
                                                                      : snapshot.nvidia.failure_detail);

        // Lower the dimension-level annotation for AV1/NVENC.
        caps.video_codecs[VideoCodec::Av1Nvenc] = SupportAnnotation{SupportLevel::NotImplemented, nvenc_reason};

        // Force the primary M3.2 combo to non-selectable via combo_override.
        const ComboKey m32_key{Container::Matroska, VideoCodec::Av1Nvenc, AudioCodec::AacMf, ChromaSubsampling::Cs420,
                               BitDepth::Bit8};
        caps.combo_overrides[m32_key] = SupportAnnotation{SupportLevel::NotImplemented, nvenc_reason};
    }

    // --- Downgrade rule B: missing AAC blocks AAC path ---
    if (!snapshot.mf_aac.available()) {
        const std::string aac_reason =
            "Media Foundation AAC unavailable: " +
            (snapshot.mf_aac.failure_detail.empty()
                 ? "MFTEnumEx found no encoders and direct CLSID_AACMFTEncoder instantiation failed"
                 : snapshot.mf_aac.failure_detail);

        // Lower the dimension-level annotation for AacMf.
        caps.audio_codecs[AudioCodec::AacMf] = SupportAnnotation{SupportLevel::NotImplemented, aac_reason};

        // Force the primary M3.2 combo to non-selectable via combo_override.
        // If NVENC was already lowered, this upserts with the AAC reason to cover both axes.
        const ComboKey m32_key{Container::Matroska, VideoCodec::Av1Nvenc, AudioCodec::AacMf, ChromaSubsampling::Cs420,
                               BitDepth::Bit8};
        // Preserve a more-severe existing override; otherwise apply AAC reason.
        const auto it = caps.combo_overrides.find(m32_key);
        if (it == caps.combo_overrides.end()) {
            caps.combo_overrides[m32_key] = SupportAnnotation{SupportLevel::NotImplemented, aac_reason};
        }
        // If an NVENC override already exists, both are NotImplemented — no change needed.
    }

    // --- Invariant C & D: H.264 and HEVC remain NotImplemented regardless ---
    // The static baseline already sets these to NotImplemented.
    // We explicitly enforce by never promoting them here.
    // (No action needed; this invariant is upheld by not touching these entries.)

    return caps;
}

CapabilitySet CapabilityBuilder::BuildFromHardwareQuery() {
    const RuntimeCapabilitySnapshot snapshot = QueryRuntimeFacts();
    return BuildEffectiveCapabilities(snapshot);
}

} // namespace exosnap::capability
