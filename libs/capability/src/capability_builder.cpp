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
                            SupportAnnotation{SupportLevel::Available, "Validated MP4 H.264+AAC path."});
    caps.containers.emplace(Container::WebM,
                            SupportAnnotation{SupportLevel::Available, "Primary validated WebM container."});

    caps.video_codecs.emplace(VideoCodec::Av1Nvenc,
                              SupportAnnotation{SupportLevel::Available, "Validated NVENC AV1 path."});
    caps.video_codecs.emplace(VideoCodec::HevcNvenc,
                              SupportAnnotation{SupportLevel::NotImplemented,
                                                "HEVC product surface exists but runtime path is not implemented."});
    caps.video_codecs.emplace(VideoCodec::H264Nvenc,
                              SupportAnnotation{SupportLevel::Available, "Validated NVENC H.264 path."});

    caps.audio_codecs.emplace(
        AudioCodec::Opus,
        SupportAnnotation{SupportLevel::Available, "Opus encoder implemented via libopus (static); M4 Phase 3."});
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

        // Lower the dimension-level annotation for all NVENC codecs.
        caps.video_codecs[VideoCodec::Av1Nvenc] = SupportAnnotation{SupportLevel::NotImplemented, nvenc_reason};
        caps.video_codecs[VideoCodec::H264Nvenc] = SupportAnnotation{SupportLevel::NotImplemented, nvenc_reason};

        // Force primary combos to non-selectable via combo_override.
        const ComboKey m32_key{Container::Matroska, VideoCodec::Av1Nvenc, AudioCodec::AacMf, ChromaSubsampling::Cs420,
                               BitDepth::Bit8};
        caps.combo_overrides[m32_key] = SupportAnnotation{SupportLevel::NotImplemented, nvenc_reason};

        const ComboKey mp4_key{Container::Mp4, VideoCodec::H264Nvenc, AudioCodec::AacMf, ChromaSubsampling::Cs420,
                               BitDepth::Bit8};
        caps.combo_overrides[mp4_key] = SupportAnnotation{SupportLevel::NotImplemented, nvenc_reason};
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

        // Force primary AAC combos to non-selectable via combo_override.
        const ComboKey m32_key{Container::Matroska, VideoCodec::Av1Nvenc, AudioCodec::AacMf, ChromaSubsampling::Cs420,
                               BitDepth::Bit8};
        const auto it = caps.combo_overrides.find(m32_key);
        if (it == caps.combo_overrides.end()) {
            caps.combo_overrides[m32_key] = SupportAnnotation{SupportLevel::NotImplemented, aac_reason};
        }

        const ComboKey mp4_key{Container::Mp4, VideoCodec::H264Nvenc, AudioCodec::AacMf, ChromaSubsampling::Cs420,
                               BitDepth::Bit8};
        const auto mp4_it = caps.combo_overrides.find(mp4_key);
        if (mp4_it == caps.combo_overrides.end()) {
            caps.combo_overrides[mp4_key] = SupportAnnotation{SupportLevel::NotImplemented, aac_reason};
        }
    }

    // --- Invariant D: HEVC remains NotImplemented regardless ---
    // The static baseline already sets HevcNvenc to NotImplemented.
    // H.264 is Available in the baseline and is handled by downgrade rules A and B above.

    return caps;
}

CapabilitySet CapabilityBuilder::BuildFromHardwareQuery() {
    const RuntimeCapabilitySnapshot snapshot = QueryRuntimeFacts();
    return BuildEffectiveCapabilities(snapshot);
}

} // namespace exosnap::capability
