#include <capability/capability_builder.h>
#include <capability/runtime_snapshot.h>

#include <string>

namespace exosnap::capability {

CapabilitySet CapabilityBuilder::BuildStaticValidatedBaseline() {
    CapabilitySet caps;
    caps.gpu_adapter_name = "validated-baseline-static";
    caps.nvenc_dll_present = true;
    caps.mf_aac_available = true;
    caps.mf_webcam_available = true; // S4: baseline assumes MF present

    caps.containers.emplace(Container::Matroska,
                            SupportAnnotation{SupportLevel::Available, "Primary validated container."});
    caps.containers.emplace(Container::Mp4,
                            SupportAnnotation{SupportLevel::Available, "Validated MP4 H.264+AAC path."});
    caps.containers.emplace(Container::WebM,
                            SupportAnnotation{SupportLevel::Available, "Primary validated WebM container."});

    caps.video_codecs.emplace(VideoCodec::Av1Nvenc,
                              SupportAnnotation{SupportLevel::Available, "Validated NVENC AV1 path."});
    caps.video_codecs.emplace(VideoCodec::HevcNvenc,
                              SupportAnnotation{SupportLevel::ValidUnvalidated,
                                                "HEVC NVENC + Matroska V_MPEGH/ISO/HEVC implemented in 0.7.0; "
                                                "not yet validated on recording hardware."});
    caps.video_codecs.emplace(VideoCodec::H264Nvenc,
                              SupportAnnotation{SupportLevel::Available, "Validated NVENC H.264 path."});

    caps.audio_codecs.emplace(
        AudioCodec::Opus,
        SupportAnnotation{SupportLevel::Available, "Opus encoder implemented via libopus (static); M4 Phase 3."});
    caps.audio_codecs.emplace(AudioCodec::Aac,
                              SupportAnnotation{SupportLevel::Available, "Validated AAC-LC Media Foundation path."});
    caps.audio_codecs.emplace(
        AudioCodec::Pcm, SupportAnnotation{SupportLevel::Available,
                                           "Uncompressed S16LE PCM (A_PCM/INT/LIT); Matroska-only (0.6.0 Audio v2)."});
    caps.audio_codecs.emplace(AudioCodec::Flac,
                              SupportAnnotation{SupportLevel::Available,
                                                "Lossless FLAC (A_FLAC) via libFLAC; Matroska-only (0.6.0 Audio v2)."});

    caps.chroma_modes.emplace(ChromaSubsampling::Cs420,
                              SupportAnnotation{SupportLevel::Available, "Validated chroma mode."});
    caps.chroma_modes.emplace(ChromaSubsampling::Cs422,
                              SupportAnnotation{SupportLevel::NotImplemented, "4:2:2 path is not implemented."});
    // 4:4:4 is a real 8-bit H.264/HEVC expert path (AYUV input, NVENC High 4:4:4 /
    // HEVC FREXT). This dimension-level annotation only states the path EXISTS
    // (mirroring the 10-bit Bit10 entry); the per-codec truth (H.264/HEVC only,
    // never AV1, never 10-bit) lives in chroma444 + the static matrix.
    caps.chroma_modes.emplace(
        ChromaSubsampling::Cs444,
        SupportAnnotation{SupportLevel::ValidUnvalidated,
                          "4:4:4 (8-bit H.264/HEVC) implemented; not yet validated on recording hardware."});

    // Per-codec 4:4:4 (YUV444, 8-bit) capability. H.264 (High 4:4:4 Predictive) and
    // HEVC (Range Extensions) carry 4:4:4; AV1 NVENC is 4:2:0 Main only. Baseline is
    // ValidUnvalidated for H.264/HEVC (implemented, not hardware-validated) and
    // NotImplemented for AV1; a real per-GPU probe downgrades H.264/HEVC when the
    // specific GPU cannot do it (ApplyNvencYuv444Support).
    caps.chroma444.emplace(
        VideoCodec::H264Nvenc,
        SupportAnnotation{SupportLevel::ValidUnvalidated,
                          "H.264 High 4:4:4 Predictive (8-bit); not yet validated on recording hardware."});
    caps.chroma444.emplace(
        VideoCodec::HevcNvenc,
        SupportAnnotation{SupportLevel::ValidUnvalidated,
                          "HEVC Range Extensions 4:4:4 (8-bit); not yet validated on recording hardware."});
    caps.chroma444.emplace(
        VideoCodec::Av1Nvenc,
        SupportAnnotation{SupportLevel::NotImplemented, "AV1 NVENC is 4:2:0 only; use H.264 or HEVC for 4:4:4."});

    for (VideoCodec codec : AllVideoCodecs()) {
        caps.bframe_capability.emplace(
            codec, BFrameCapability{SupportAnnotation{SupportLevel::NotImplemented,
                                                      "B-frame support not yet probed on this hardware."},
                                    0, 0});
        caps.lookahead.emplace(codec, SupportAnnotation{SupportLevel::NotImplemented,
                                                        "Lookahead support not yet probed on this hardware."});
        caps.temporal_aq.emplace(codec, SupportAnnotation{SupportLevel::NotImplemented,
                                                          "Temporal-AQ support not yet probed on this hardware."});
    }

    caps.bit_depths.emplace(BitDepth::Bit8, SupportAnnotation{SupportLevel::Available, "Validated bit depth."});
    caps.bit_depths.emplace(BitDepth::Bit10,
                            SupportAnnotation{SupportLevel::ValidUnvalidated,
                                              "10-bit HEVC/AV1 via NVENC Main10/P010 implemented in 0.7.0 "
                                              "(SDR BT.709); not yet validated on recording hardware. "
                                              "Requires HEVC or AV1; H.264 is 8-bit only."});

    // Explicit per-codec HDR10-native (10-bit / P010) capability.
    // HEVC (Main10) and AV1 (Main 10-bit) carry a native PQ/BT.2020 HDR10 signal;
    // H.264 has no 10-bit NVENC path, so it cannot. This is a codec-format fact,
    // NOT a per-GPU probe: like the Bit10 annotation above it is inferred from
    // "NVENC can HEVC/AV1", not from an NV_ENC_CAPS_SUPPORT_10BIT_ENCODE query —
    // a real probe is deferred; the inference (NVENC HEVC/AV1 implies
    // Main10/10-bit) is the documented risk. Kept independent of the
    // NVENC-absence downgrade below: encode availability is a separate concern
    // owned by video_codecs / the codec-availability blocker.
    caps.hdr10_native.emplace(
        VideoCodec::Av1Nvenc,
        SupportAnnotation{SupportLevel::Available, "AV1 carries native HDR10 (10-bit/P010, PQ/BT.2020)."});
    caps.hdr10_native.emplace(
        VideoCodec::HevcNvenc,
        SupportAnnotation{SupportLevel::Available, "HEVC Main10 carries native HDR10 (10-bit/P010, PQ/BT.2020)."});
    caps.hdr10_native.emplace(
        VideoCodec::H264Nvenc,
        SupportAnnotation{SupportLevel::NotImplemented,
                          "H.264 has no 10-bit/HDR10 path (8-bit only). Use HEVC or AV1 for HDR10."});

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
    caps.mf_webcam_available = snapshot.mf_webcam.available; // S4: gate webcam UI

    // --- Downgrade rule A: missing NVENC blocks AV1 path ---
    // NVENC is required when the DLL is not present or API version is not valid.
    if (!snapshot.nvidia.nvenc_dll_present || !snapshot.nvidia.nvenc_api_version_valid) {
        const std::string nvenc_reason =
            "NVIDIA NVENC is not available on this system. "
            "Install a supported NVIDIA driver or switch to a non-NVENC recording profile.";

        // Lower the dimension-level annotation for all NVENC codecs.
        caps.video_codecs[VideoCodec::Av1Nvenc] = SupportAnnotation{SupportLevel::NotImplemented, nvenc_reason};
        caps.video_codecs[VideoCodec::H264Nvenc] = SupportAnnotation{SupportLevel::NotImplemented, nvenc_reason};
        caps.video_codecs[VideoCodec::HevcNvenc] = SupportAnnotation{SupportLevel::NotImplemented, nvenc_reason};

        // 10-bit requires NVENC (HEVC Main10 / AV1 10-bit). Without NVENC it cannot be
        // produced at all, so downgrade it to NotImplemented mirroring the HEVC codec.
        caps.bit_depths[BitDepth::Bit10] = SupportAnnotation{SupportLevel::NotImplemented, nvenc_reason};

        // Force primary combos to non-selectable via combo_override.
        const ComboKey mkv_av1_key{Container::Matroska, VideoCodec::Av1Nvenc, AudioCodec::Aac, ChromaSubsampling::Cs420,
                                   BitDepth::Bit8};
        caps.combo_overrides[mkv_av1_key] = SupportAnnotation{SupportLevel::NotImplemented, nvenc_reason};

        const ComboKey mkv_h264_key{Container::Matroska, VideoCodec::H264Nvenc, AudioCodec::Aac,
                                    ChromaSubsampling::Cs420, BitDepth::Bit8};
        caps.combo_overrides[mkv_h264_key] = SupportAnnotation{SupportLevel::NotImplemented, nvenc_reason};

        const ComboKey mp4_key{Container::Mp4, VideoCodec::H264Nvenc, AudioCodec::Aac, ChromaSubsampling::Cs420,
                               BitDepth::Bit8};
        caps.combo_overrides[mp4_key] = SupportAnnotation{SupportLevel::NotImplemented, nvenc_reason};
    }

    // --- Downgrade rule B: missing AAC blocks AAC path ---
    if (!snapshot.mf_aac.available()) {
        const std::string aac_reason =
            "AAC audio encoding (Media Foundation) is not available on this system. "
            "Switch to an Opus recording profile or ensure Media Foundation components are installed.";

        // Lower the dimension-level annotation for Aac.
        caps.audio_codecs[AudioCodec::Aac] = SupportAnnotation{SupportLevel::NotImplemented, aac_reason};

        // Force primary AAC combos to non-selectable via combo_override.
        const ComboKey mkv_av1_key{Container::Matroska, VideoCodec::Av1Nvenc, AudioCodec::Aac, ChromaSubsampling::Cs420,
                                   BitDepth::Bit8};
        if (caps.combo_overrides.find(mkv_av1_key) == caps.combo_overrides.end()) {
            caps.combo_overrides.try_emplace(mkv_av1_key, SupportAnnotation{SupportLevel::NotImplemented, aac_reason});
        }

        const ComboKey mkv_h264_key{Container::Matroska, VideoCodec::H264Nvenc, AudioCodec::Aac,
                                    ChromaSubsampling::Cs420, BitDepth::Bit8};
        if (caps.combo_overrides.find(mkv_h264_key) == caps.combo_overrides.end()) {
            caps.combo_overrides.try_emplace(mkv_h264_key, SupportAnnotation{SupportLevel::NotImplemented, aac_reason});
        }

        const ComboKey mp4_key{Container::Mp4, VideoCodec::H264Nvenc, AudioCodec::Aac, ChromaSubsampling::Cs420,
                               BitDepth::Bit8};
        if (caps.combo_overrides.find(mp4_key) == caps.combo_overrides.end()) {
            caps.combo_overrides.try_emplace(mp4_key, SupportAnnotation{SupportLevel::NotImplemented, aac_reason});
        }
    }

    // --- HEVC (0.7.0) ---
    // The static baseline sets HevcNvenc to ValidUnvalidated (implemented engine path,
    // not yet validated on recording hardware). Downgrade rule A above lowers it to
    // NotImplemented when NVENC is absent, mirroring AV1/H.264. A live GPU smoke test is
    // required before promoting HEVC to Available.
    // H.264 is Available in the baseline and is handled by downgrade rules A and B above.

    // --- Per-GPU NVENC codec-GUID refinement (truthful detection) ---
    // Runs AFTER the DLL-presence gate: when NVENC is absent, rule A already set every
    // codec to NotImplemented and nvenc_codec_probed is false, so this is a no-op. When a
    // real session probe ran, it downgrades codecs this specific GPU generation cannot
    // encode (e.g. AV1 on pre-Ada hardware) that the static baseline optimistically
    // advertised.
    ApplyNvencCodecSupport(caps, snapshot.nvidia);
    ApplyNvencYuv444Support(caps, snapshot.nvidia);
    ApplyNvencAdvancedEncodeSupport(caps, snapshot.nvidia);

    return caps;
}

void ApplyNvencCodecSupport(CapabilitySet& caps, const NvidiaRuntimeFacts& facts) {
    // Only a real, successful per-GPU probe is authoritative. Without it the static
    // baseline (or the NVENC DLL-presence gate) stands — never regress to "no codecs".
    if (!facts.nvenc_codec_probed) {
        return;
    }

    auto downgrade_if_unsupported = [&caps](VideoCodec codec, bool supported, const std::string& reason) {
        if (supported) {
            return; // GPU encodes this codec — keep the baseline annotation.
        }
        // Preserve a stricter existing annotation (e.g. NotImplemented from the DLL gate)
        // rather than overwriting it; only downgrade when the codec is still selectable.
        const auto it = caps.video_codecs.find(codec);
        if (it == caps.video_codecs.end() || IsSelectable(it->second.level)) {
            caps.video_codecs[codec] = SupportAnnotation{SupportLevel::NotImplemented, reason};
        }
    };

    downgrade_if_unsupported(VideoCodec::Av1Nvenc, facts.nvenc_av1,
                             "This GPU does not support AV1 NVENC encoding "
                             "(NVIDIA Ada Lovelace / RTX 40 series or newer is required).");
    downgrade_if_unsupported(VideoCodec::HevcNvenc, facts.nvenc_hevc,
                             "This GPU does not support HEVC (H.265) NVENC encoding.");
    downgrade_if_unsupported(VideoCodec::H264Nvenc, facts.nvenc_h264,
                             "This GPU does not support H.264 NVENC encoding.");
}

void ApplyNvencYuv444Support(CapabilitySet& caps, const NvidiaRuntimeFacts& facts) {
    // Without an authoritative probe, keep the ValidUnvalidated baseline (AV1 stays
    // NotImplemented via the baseline map). Never regress headless CI to "no 4:4:4".
    if (!facts.nvenc_codec_probed) {
        return;
    }

    auto downgrade_if_unsupported = [&caps](VideoCodec codec, bool supported, const std::string& reason) {
        if (supported) {
            return; // GPU encodes 4:4:4 for this codec — keep the baseline annotation.
        }
        const auto it = caps.chroma444.find(codec);
        if (it == caps.chroma444.end() || IsSelectable(it->second.level)) {
            caps.chroma444[codec] = SupportAnnotation{SupportLevel::NotImplemented, reason};
        }
    };

    downgrade_if_unsupported(VideoCodec::H264Nvenc, facts.nvenc_yuv444_h264,
                             "This GPU does not support H.264 4:4:4 (YUV444) NVENC encoding.");
    downgrade_if_unsupported(VideoCodec::HevcNvenc, facts.nvenc_yuv444_hevc,
                             "This GPU does not support HEVC 4:4:4 (YUV444) NVENC encoding.");
    // AV1 is not probed for 4:4:4 — it has no NVENC 4:4:4 path and stays
    // NotImplemented from the baseline.
}

void ApplyNvencAdvancedEncodeSupport(CapabilitySet& caps, const NvidiaRuntimeFacts& facts) {
    if (!facts.nvenc_codec_probed) {
        return; // fail-closed baseline stands — no real probe ran
    }
    auto apply = [&caps](VideoCodec codec, bool advertised, const NvencAdvancedEncodeFacts& adv) {
        if (!advertised) {
            return; // codec not advertised at all -> leave NotImplemented/0 baseline
        }
        caps.bframe_capability[codec] = BFrameCapability{
            SupportAnnotation{adv.max_bframes > 0 ? SupportLevel::ValidUnvalidated : SupportLevel::NotImplemented,
                              adv.max_bframes > 0
                                  ? "Probed on this GPU/driver; not yet validated by ExoSnap's own encode path."
                                  : "GPU/driver reports 0 max B-frames for this codec."},
            adv.max_bframes, adv.bframe_ref_mode};
        caps.lookahead[codec] = SupportAnnotation{
            adv.lookahead ? SupportLevel::ValidUnvalidated : SupportLevel::NotImplemented,
            adv.lookahead ? "Probed on this GPU/driver; not yet validated by ExoSnap's own encode path."
                          : "GPU/driver does not report lookahead support for this codec."};
        caps.temporal_aq[codec] = SupportAnnotation{
            adv.temporal_aq ? SupportLevel::ValidUnvalidated : SupportLevel::NotImplemented,
            adv.temporal_aq ? "Probed on this GPU/driver; not yet validated by ExoSnap's own encode path."
                            : "GPU/driver does not report Temporal-AQ support for this codec."};
    };
    apply(VideoCodec::H264Nvenc, facts.nvenc_h264, facts.nvenc_adv_h264);
    apply(VideoCodec::HevcNvenc, facts.nvenc_hevc, facts.nvenc_adv_hevc);
    apply(VideoCodec::Av1Nvenc, facts.nvenc_av1, facts.nvenc_adv_av1);
}

CapabilitySet CapabilityBuilder::BuildFromHardwareQuery() {
    const RuntimeCapabilitySnapshot snapshot = QueryRuntimeFacts();
    CapabilitySet caps = BuildEffectiveCapabilities(snapshot);
    // Only a set that just came from a real, completed hardware probe is
    // authoritative enough to gate a recording-start decision.
    caps.probed = true;
    return caps;
}

} // namespace exosnap::capability
