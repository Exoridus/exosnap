#include <capability/container_compat_registry.h>

namespace exosnap::capability {

// ---------------------------------------------------------------------------
// ToString
// ---------------------------------------------------------------------------

std::string_view ToString(ContainerCompatLevel level) noexcept {
    switch (level) {
    case ContainerCompatLevel::Recommended:
        return "Recommended";
    case ContainerCompatLevel::Allowed:
        return "Allowed";
    case ContainerCompatLevel::Experimental:
        return "Experimental";
    case ContainerCompatLevel::Fallback:
        return "Fallback";
    case ContainerCompatLevel::Prohibited:
        return "Prohibited";
    }
    return "Prohibited";
}

// ---------------------------------------------------------------------------
// ContainerCompatRegistry::Query — exhaustive table
//
// This table is the SINGLE SOURCE OF TRUTH for container × video × audio
// compatibility.  Every combination of the three enum dimensions is listed
// explicitly; there are no implicit fall-throughs.
//
// ADR 0010 authoritative matrix:
//
//   MKV  | AV1          | Opus  → Recommended  (primary validated path)
//   MKV  | AV1          | AAC   → Recommended  (validated M3.2 path)
//   MKV  | AV1          | PCM   → Allowed      (uncompressed S16LE A_PCM/INT_LIT;
//                                               Matroska-only, 0.6.0 Audio v2)
//   MKV  | AV1          | FLAC  → Allowed      (lossless A_FLAC via libFLAC;
//                                               Matroska-only, 0.6.0 Audio v2)
//   MKV  | HEVC         | Opus  → Allowed      (0.7.0: HEVC NVENC + Matroska V_MPEGH/ISO/HEVC;
//                                               hvcC codec-private + length-prefixed samples)
//   MKV  | HEVC         | AAC   → Allowed      (0.7.0: HEVC NVENC + Matroska V_MPEGH/ISO/HEVC)
//   MKV  | HEVC         | PCM   → Allowed      (0.7.0: HEVC NVENC + Matroska V_MPEGH/ISO/HEVC)
//   MKV  | HEVC         | FLAC  → Allowed      (0.7.0: HEVC NVENC + Matroska V_MPEGH/ISO/HEVC)
//   MKV  | H.264        | AAC   → Recommended  (validated MKV H.264+AAC)
//   MKV  | H.264        | Opus  → Allowed      (Matroska carries Opus natively;
//                                               Opus-in-MKV write path is
//                                               production-validated via AV1+Opus;
//                                               dedicated player-matrix pass for
//                                               H.264+Opus not yet on file)
//   MKV  | H.264        | PCM   → Allowed      (uncompressed S16LE A_PCM/INT_LIT;
//                                               Matroska-only, 0.6.0 Audio v2)
//   MKV  | H.264        | FLAC  → Allowed      (lossless A_FLAC via libFLAC;
//                                               Matroska-only, 0.6.0 Audio v2)
//
//   MP4  | H.264        | AAC   → Recommended  (primary validated MP4 path,
//                                               delivered via remux-on-stop ADR 0014)
//   MP4  | H.264        | Opus  → Prohibited   (ADR 0010: Opus-in-MP4 is Prohibited)
//   MP4  | H.264        | PCM   → Experimental (libavformat emits the ipcm sample entry
//                                               (ISO/IEC 23003-5) for pcm_s16le/s24le/
//                                               s32le in MP4; ipcm has limited player
//                                               support — Windows Films & TV, QuickTime,
//                                               and many NLEs do not play it; deferred
//                                               until a broadly-compatible sample-entry
//                                               mapping is validated; ADR 0030)
//   MP4  | H.264        | FLAC  → Experimental (FLAC-in-MP4 not specified here)
//   MP4  | HEVC         | AAC   → Allowed      (0.7.0: HEVC via transient MKV, remuxed to MP4 with
//                                               the 'hvc1' sample-entry FourCC; Apple/NLE-compatible)
//   MP4  | HEVC         | Opus  → Prohibited   (Opus-in-MP4 is Prohibited)
//   MP4  | HEVC         | PCM   → Experimental (MP4 audio is AAC-only)
//   MP4  | HEVC         | FLAC  → Experimental (MP4 audio is AAC-only)
//   MP4  | AV1          | AAC   → Experimental (deferred; AV1-in-MP4 not validated)
//   MP4  | AV1          | Opus  → Prohibited   (Opus-in-MP4 is Prohibited)
//   MP4  | AV1          | PCM   → Experimental (not implemented)
//   MP4  | AV1          | FLAC  → Experimental (not implemented)
//
//   WebM | AV1          | Opus  → Recommended  (primary validated WebM path)
//   WebM | AV1          | AAC   → Prohibited   (WebM does not carry AAC)
//   WebM | AV1          | PCM   → Prohibited   (WebM does not carry PCM)
//   WebM | AV1          | FLAC  → Prohibited   (WebM does not carry FLAC)
//   WebM | HEVC         | Opus  → Prohibited   (WebM does not carry H.264/HEVC)
//   WebM | HEVC         | AAC   → Prohibited
//   WebM | HEVC         | PCM   → Prohibited
//   WebM | H.264        | Opus  → Prohibited   (WebM does not carry H.264/HEVC)
//   WebM | H.264        | AAC   → Prohibited
//   WebM | H.264        | PCM   → Prohibited
//
// Notes:
//   - MKV + H.264 + Opus is Allowed in this registry.  Matroska carries Opus
//     natively; the Opus-in-MKV write path is production-validated (AV1+Opus);
//     only a dedicated player-matrix pass for this exact pairing is missing.
//     ReconcileCodecs() now leaves H.264+Opus presets unchanged (Allowed is a
//     working combo); no rewrite to AAC occurs any more.
//   - MKV + HEVC entries are promoted to Allowed in 0.7.0 (hvcC codec-private +
//     length-prefixed sample conversion implemented; V_MPEGH/ISO/HEVC codec ID).
//   - MP4 + HEVC + AAC is promoted to Allowed in 0.7.0: the engine records HEVC to a
//     transient MKV (hvcC) and the remuxer stream-copies it to MP4 with the 'hvc1'
//     FourCC (out-of-band parameter sets) for Apple/QuickTime/NLE compatibility
//     (ADR 0014). MP4 + HEVC + PCM/FLAC stay Experimental — MP4 audio is AAC-only.
// ---------------------------------------------------------------------------

ContainerCompatEntry ContainerCompatRegistry::Query(Container container, VideoCodec video, AudioCodec audio) noexcept {
    // --- MKV ---
    if (container == Container::Matroska) {
        if (video == VideoCodec::Av1Nvenc) {
            if (audio == AudioCodec::Opus)
                return {ContainerCompatLevel::Recommended, "Primary validated MKV path: AV1 NVENC + Opus."};
            if (audio == AudioCodec::AacMf)
                return {ContainerCompatLevel::Recommended, "Validated MKV path: AV1 NVENC + AAC (M3.2)."};
            if (audio == AudioCodec::Pcm)
                return {ContainerCompatLevel::Allowed,
                        "MKV + AV1 + PCM: uncompressed 16-bit signed little-endian PCM (A_PCM/INT_LIT). "
                        "Large files; lossless audio. Matroska-only (0.6.0 Audio v2)."};
            if (audio == AudioCodec::Flac)
                return {ContainerCompatLevel::Allowed,
                        "MKV + AV1 + FLAC: lossless FLAC (A_FLAC) via libFLAC. "
                        "Smaller than PCM, still lossless. Matroska-only (0.6.0 Audio v2)."};
        }
        if (video == VideoCodec::H264Nvenc) {
            if (audio == AudioCodec::AacMf)
                return {ContainerCompatLevel::Recommended, "Validated MKV path: H.264 NVENC + AAC."};
            if (audio == AudioCodec::Opus)
                return {ContainerCompatLevel::Allowed,
                        "MKV + H.264 + Opus: Matroska carries Opus natively and the Opus-in-MKV write path "
                        "is production-validated (AV1+Opus). A dedicated player-matrix pass for this exact "
                        "pairing is not yet on file (ADR 0010 Allowed caveat)."};
            if (audio == AudioCodec::Pcm)
                return {ContainerCompatLevel::Allowed,
                        "MKV + H.264 + PCM: uncompressed 16-bit signed little-endian PCM (A_PCM/INT_LIT). "
                        "Large files; lossless audio. Matroska-only (0.6.0 Audio v2)."};
            if (audio == AudioCodec::Flac)
                return {ContainerCompatLevel::Allowed,
                        "MKV + H.264 + FLAC: lossless FLAC (A_FLAC) via libFLAC. "
                        "Smaller than PCM, still lossless. Matroska-only (0.6.0 Audio v2)."};
        }
        if (video == VideoCodec::HevcNvenc) {
            if (audio == AudioCodec::AacMf)
                return {ContainerCompatLevel::Allowed,
                        "MKV + HEVC + AAC: HEVC NVENC with Matroska V_MPEGH/ISO/HEVC (hvcC codec-private, "
                        "length-prefixed samples). Implemented in 0.7.0."};
            if (audio == AudioCodec::Opus)
                return {ContainerCompatLevel::Allowed,
                        "MKV + HEVC + Opus: HEVC NVENC with Matroska V_MPEGH/ISO/HEVC (hvcC codec-private, "
                        "length-prefixed samples). Implemented in 0.7.0."};
            if (audio == AudioCodec::Pcm)
                return {ContainerCompatLevel::Allowed,
                        "MKV + HEVC + PCM: HEVC NVENC with Matroska V_MPEGH/ISO/HEVC (hvcC codec-private, "
                        "length-prefixed samples) + uncompressed PCM (A_PCM/INT_LIT). Implemented in 0.7.0."};
            if (audio == AudioCodec::Flac)
                return {ContainerCompatLevel::Allowed,
                        "MKV + HEVC + FLAC: HEVC NVENC with Matroska V_MPEGH/ISO/HEVC (hvcC codec-private, "
                        "length-prefixed samples) + lossless FLAC (A_FLAC). Implemented in 0.7.0."};
        }
        return {ContainerCompatLevel::Prohibited, "Unknown MKV combination."};
    }

    // --- MP4 ---
    if (container == Container::Mp4) {
        // Opus-in-MP4 is Prohibited for all video codecs (ADR 0010 + ADR 0014).
        if (audio == AudioCodec::Opus)
            return {ContainerCompatLevel::Prohibited, "Opus audio is not supported in MP4. "
                                                      "Select AAC for MP4 recordings (ADR 0010)."};

        if (video == VideoCodec::H264Nvenc) {
            if (audio == AudioCodec::AacMf)
                return {ContainerCompatLevel::Recommended,
                        "Primary validated MP4 path: H.264 NVENC + AAC via remux-on-stop (ADR 0014)."};
            if (audio == AudioCodec::Pcm)
                return {ContainerCompatLevel::Experimental,
                        "MP4 + H.264 + PCM: libavformat writes an ipcm (ISO/IEC 23003-5) sample entry "
                        "for pcm_s16le/pcm_s24le/pcm_s32le in MP4 (confirmed via ffprobe "
                        "codec_tag_string=ipcm). ipcm has limited player support — Windows Films & TV, "
                        "QuickTime, and many NLEs do not play it. Deferred until a broadly-compatible "
                        "sample-entry mapping (e.g. sowt/in24) is validated; use MKV for PCM (ADR 0030)."};
            if (audio == AudioCodec::Flac)
                return {ContainerCompatLevel::Experimental,
                        "MP4 + H.264 + FLAC: FLAC-in-MP4 not specified in this build (use MKV for FLAC)."};
        }
        if (video == VideoCodec::HevcNvenc) {
            if (audio == AudioCodec::AacMf)
                return {ContainerCompatLevel::Allowed,
                        "MP4 + HEVC + AAC: HEVC recorded to a transient MKV and remuxed to MP4 with the "
                        "'hvc1' sample-entry FourCC (parameter sets out-of-band in hvcC) for Apple/QuickTime/"
                        "NLE compatibility. Implemented in 0.7.0 (ADR 0010/0014)."};
            if (audio == AudioCodec::Pcm)
                return {ContainerCompatLevel::Experimental,
                        "MP4 + HEVC + PCM: not implemented (MP4 audio is AAC-only)."};
            if (audio == AudioCodec::Flac)
                return {ContainerCompatLevel::Experimental,
                        "MP4 + HEVC + FLAC: not implemented (MP4 audio is AAC-only)."};
        }
        if (video == VideoCodec::Av1Nvenc) {
            if (audio == AudioCodec::AacMf)
                return {ContainerCompatLevel::Experimental,
                        "MP4 + AV1 + AAC: deferred; AV1-in-MP4 container support not yet validated."};
            if (audio == AudioCodec::Pcm)
                return {ContainerCompatLevel::Experimental, "MP4 + AV1 + PCM: not implemented."};
            if (audio == AudioCodec::Flac)
                return {ContainerCompatLevel::Experimental, "MP4 + AV1 + FLAC: not implemented."};
        }
        return {ContainerCompatLevel::Prohibited, "Unknown MP4 combination."};
    }

    // --- WebM ---
    if (container == Container::WebM) {
        // H.264 and HEVC are unconditionally Prohibited in WebM (ADR 0010).
        if (video == VideoCodec::H264Nvenc || video == VideoCodec::HevcNvenc)
            return {ContainerCompatLevel::Prohibited, "WebM supports only AV1 in ExoSnap's product matrix. "
                                                      "H.264 and HEVC are prohibited in WebM (ADR 0010)."};

        if (video == VideoCodec::Av1Nvenc) {
            if (audio == AudioCodec::Opus)
                return {ContainerCompatLevel::Recommended, "Primary validated WebM path: AV1 NVENC + Opus."};
            if (audio == AudioCodec::AacMf)
                return {ContainerCompatLevel::Prohibited,
                        "WebM does not support AAC. Use Opus for WebM recordings (ADR 0010)."};
            if (audio == AudioCodec::Pcm)
                return {ContainerCompatLevel::Prohibited, "WebM does not support PCM. Use Opus for WebM recordings."};
            if (audio == AudioCodec::Flac)
                return {ContainerCompatLevel::Prohibited, "WebM does not support FLAC. Use Opus for WebM recordings."};
        }
        return {ContainerCompatLevel::Prohibited, "Unknown WebM combination."};
    }

    return {ContainerCompatLevel::Prohibited, "Unknown container."};
}

// ---------------------------------------------------------------------------
// ContainerCompatRegistry::PreferredAudioCodec
// ---------------------------------------------------------------------------

AudioCodec ContainerCompatRegistry::PreferredAudioCodec(Container container) noexcept {
    switch (container) {
    case Container::Matroska:
        return AudioCodec::Opus;
    case Container::Mp4:
        return AudioCodec::AacMf;
    case Container::WebM:
        return AudioCodec::Opus;
    }
    return AudioCodec::AacMf;
}

// ---------------------------------------------------------------------------
// ContainerCompatRegistry::PreferredVideoCodec
// ---------------------------------------------------------------------------

VideoCodec ContainerCompatRegistry::PreferredVideoCodec(Container container) noexcept {
    switch (container) {
    case Container::Matroska:
        return VideoCodec::Av1Nvenc;
    case Container::Mp4:
        return VideoCodec::H264Nvenc;
    case Container::WebM:
        return VideoCodec::Av1Nvenc;
    }
    return VideoCodec::H264Nvenc;
}

// ---------------------------------------------------------------------------
// ContainerCompatRegistry::ReconcileCodecs
// ---------------------------------------------------------------------------

void ContainerCompatRegistry::ReconcileCodecs(Container container, VideoCodec& video, AudioCodec& audio) noexcept {
    // Reconciliation fixes presets/configs so they use a combination that is
    // actually implemented and working today.  Only Recommended and Allowed
    // entries qualify — Experimental entries map to NotImplemented in the
    // CapabilitySet and are not user-selectable, so they must be fixed just
    // like Prohibited ones.
    auto IsWorkingCombo = [](ContainerCompatLevel level) noexcept -> bool {
        return level == ContainerCompatLevel::Recommended || level == ContainerCompatLevel::Allowed;
    };

    // Step 1: check current combination.
    const ContainerCompatEntry current = Query(container, video, audio);
    if (IsWorkingCombo(current.level)) {
        return; // already a working combination; leave unchanged
    }

    // Step 2: try each audio codec in turn while keeping video.
    // Try all three codecs so we find the best working audio for this video,
    // preferring Opus → AAC → PCM for MKV/WebM, and AAC → Opus → PCM for MP4.
    // This preserves the video codec whenever possible (e.g. MKV + H264 + Opus
    // → MKV + H264 + AAC rather than falling all the way to AV1 + Opus).
    const std::array<AudioCodec, 4> audio_candidates = {AudioCodec::AacMf, AudioCodec::Opus, AudioCodec::Pcm,
                                                        AudioCodec::Flac};
    for (const AudioCodec candidate_audio : audio_candidates) {
        if (candidate_audio == audio) {
            continue; // already tried (and failed at step 1)
        }
        const ContainerCompatEntry audio_fixed = Query(container, video, candidate_audio);
        if (IsWorkingCombo(audio_fixed.level)) {
            audio = candidate_audio;
            return;
        }
    }

    // Step 3: try fixing both video and audio to the preferred codecs for the
    // container.  This is the guaranteed fallback.
    // WebM → AV1 + Opus; MKV → AV1 + Opus; MP4 → H264 + AAC.
    const VideoCodec preferred_video = PreferredVideoCodec(container);
    const AudioCodec preferred_audio = PreferredAudioCodec(container);
    video = preferred_video;
    audio = preferred_audio;
}

} // namespace exosnap::capability
