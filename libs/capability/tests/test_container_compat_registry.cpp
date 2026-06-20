#include <gtest/gtest.h>

#include <capability/config_types.h>
#include <capability/container_compat_registry.h>

#include <string>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace exosnap::capability {
namespace {

// Convenience: Query and return the level only.
ContainerCompatLevel Level(Container c, VideoCodec v, AudioCodec a) noexcept {
    return ContainerCompatRegistry::Query(c, v, a).level;
}

// ---------------------------------------------------------------------------
// MKV matrix
// ---------------------------------------------------------------------------

TEST(ContainerCompatRegistry, Mkv_Av1_Opus_IsRecommended) {
    EXPECT_EQ(Level(Container::Matroska, VideoCodec::Av1Nvenc, AudioCodec::Opus), ContainerCompatLevel::Recommended);
}

TEST(ContainerCompatRegistry, Mkv_Av1_Aac_IsRecommended) {
    EXPECT_EQ(Level(Container::Matroska, VideoCodec::Av1Nvenc, AudioCodec::AacMf), ContainerCompatLevel::Recommended);
}

TEST(ContainerCompatRegistry, Mkv_Av1_Pcm_IsAllowed) {
    // 0.6.0 Audio v2: uncompressed S16LE PCM (A_PCM/INT_LIT) is Matroska-only and
    // now implemented; MKV + AV1 + PCM is Allowed.
    EXPECT_EQ(Level(Container::Matroska, VideoCodec::Av1Nvenc, AudioCodec::Pcm), ContainerCompatLevel::Allowed);
}

TEST(ContainerCompatRegistry, Mkv_Av1_Flac_IsAllowed) {
    // 0.6.0 Audio v2: lossless FLAC (A_FLAC) is Matroska-only and implemented;
    // MKV + AV1 + FLAC is Allowed.
    EXPECT_EQ(Level(Container::Matroska, VideoCodec::Av1Nvenc, AudioCodec::Flac), ContainerCompatLevel::Allowed);
}

TEST(ContainerCompatRegistry, Mkv_H264_Aac_IsRecommended) {
    EXPECT_EQ(Level(Container::Matroska, VideoCodec::H264Nvenc, AudioCodec::AacMf), ContainerCompatLevel::Recommended);
}

TEST(ContainerCompatRegistry, Mkv_H264_Opus_IsAllowed) {
    // Matroska carries Opus natively; Opus-in-MKV write path is production-validated
    // (AV1+Opus). Only a dedicated player-matrix pass for H.264+Opus is missing —
    // that is the Allowed caveat per ADR 0010.
    EXPECT_EQ(Level(Container::Matroska, VideoCodec::H264Nvenc, AudioCodec::Opus), ContainerCompatLevel::Allowed);
}

TEST(ContainerCompatRegistry, Mkv_H264_Pcm_IsAllowed) {
    // 0.6.0 Audio v2: PCM is implemented and Matroska-only; MKV + H.264 + PCM is Allowed.
    EXPECT_EQ(Level(Container::Matroska, VideoCodec::H264Nvenc, AudioCodec::Pcm), ContainerCompatLevel::Allowed);
}

TEST(ContainerCompatRegistry, Mkv_H264_Flac_IsAllowed) {
    // 0.6.0 Audio v2: FLAC is implemented and Matroska-only; MKV + H.264 + FLAC is Allowed.
    EXPECT_EQ(Level(Container::Matroska, VideoCodec::H264Nvenc, AudioCodec::Flac), ContainerCompatLevel::Allowed);
}

TEST(ContainerCompatRegistry, WebM_Av1_Flac_IsProhibited) {
    // WebM cannot carry FLAC.
    EXPECT_EQ(Level(Container::WebM, VideoCodec::Av1Nvenc, AudioCodec::Flac), ContainerCompatLevel::Prohibited);
}

TEST(ContainerCompatRegistry, Mp4_H264_Flac_IsExperimental) {
    // FLAC-in-MP4 is not specified in this build.
    EXPECT_EQ(Level(Container::Mp4, VideoCodec::H264Nvenc, AudioCodec::Flac), ContainerCompatLevel::Experimental);
}

TEST(ContainerCompatRegistry, Mkv_Hevc_Aac_IsExperimental) {
    EXPECT_EQ(Level(Container::Matroska, VideoCodec::HevcNvenc, AudioCodec::AacMf), ContainerCompatLevel::Experimental);
}

TEST(ContainerCompatRegistry, Mkv_Hevc_Opus_IsExperimental) {
    EXPECT_EQ(Level(Container::Matroska, VideoCodec::HevcNvenc, AudioCodec::Opus), ContainerCompatLevel::Experimental);
}

TEST(ContainerCompatRegistry, Mkv_Hevc_Pcm_IsExperimental) {
    EXPECT_EQ(Level(Container::Matroska, VideoCodec::HevcNvenc, AudioCodec::Pcm), ContainerCompatLevel::Experimental);
}

// ---------------------------------------------------------------------------
// MP4 matrix
// ---------------------------------------------------------------------------

TEST(ContainerCompatRegistry, Mp4_H264_Aac_IsRecommended) {
    EXPECT_EQ(Level(Container::Mp4, VideoCodec::H264Nvenc, AudioCodec::AacMf), ContainerCompatLevel::Recommended);
}

TEST(ContainerCompatRegistry, Mp4_H264_Opus_IsProhibited) {
    // ADR 0010: Opus-in-MP4 is Prohibited for all video codecs.
    EXPECT_EQ(Level(Container::Mp4, VideoCodec::H264Nvenc, AudioCodec::Opus), ContainerCompatLevel::Prohibited);
}

TEST(ContainerCompatRegistry, Mp4_H264_Pcm_IsExperimental) {
    // PCM-in-MP4: sample-entry unspecified; player matrix not on file.
    EXPECT_EQ(Level(Container::Mp4, VideoCodec::H264Nvenc, AudioCodec::Pcm), ContainerCompatLevel::Experimental);
}

TEST(ContainerCompatRegistry, Mp4_Hevc_Aac_IsExperimental) {
    // hvc1/hev1 unresolved; not implemented.
    EXPECT_EQ(Level(Container::Mp4, VideoCodec::HevcNvenc, AudioCodec::AacMf), ContainerCompatLevel::Experimental);
}

TEST(ContainerCompatRegistry, Mp4_Hevc_Opus_IsProhibited) {
    EXPECT_EQ(Level(Container::Mp4, VideoCodec::HevcNvenc, AudioCodec::Opus), ContainerCompatLevel::Prohibited);
}

TEST(ContainerCompatRegistry, Mp4_Hevc_Pcm_IsExperimental) {
    EXPECT_EQ(Level(Container::Mp4, VideoCodec::HevcNvenc, AudioCodec::Pcm), ContainerCompatLevel::Experimental);
}

TEST(ContainerCompatRegistry, Mp4_Av1_Aac_IsExperimental) {
    // AV1-in-MP4 not yet validated.
    EXPECT_EQ(Level(Container::Mp4, VideoCodec::Av1Nvenc, AudioCodec::AacMf), ContainerCompatLevel::Experimental);
}

TEST(ContainerCompatRegistry, Mp4_Av1_Opus_IsProhibited) {
    EXPECT_EQ(Level(Container::Mp4, VideoCodec::Av1Nvenc, AudioCodec::Opus), ContainerCompatLevel::Prohibited);
}

TEST(ContainerCompatRegistry, Mp4_Av1_Pcm_IsExperimental) {
    EXPECT_EQ(Level(Container::Mp4, VideoCodec::Av1Nvenc, AudioCodec::Pcm), ContainerCompatLevel::Experimental);
}

// ---------------------------------------------------------------------------
// WebM matrix
// ---------------------------------------------------------------------------

TEST(ContainerCompatRegistry, WebM_Av1_Opus_IsRecommended) {
    EXPECT_EQ(Level(Container::WebM, VideoCodec::Av1Nvenc, AudioCodec::Opus), ContainerCompatLevel::Recommended);
}

TEST(ContainerCompatRegistry, WebM_Av1_Aac_IsProhibited) {
    EXPECT_EQ(Level(Container::WebM, VideoCodec::Av1Nvenc, AudioCodec::AacMf), ContainerCompatLevel::Prohibited);
}

TEST(ContainerCompatRegistry, WebM_Av1_Pcm_IsProhibited) {
    EXPECT_EQ(Level(Container::WebM, VideoCodec::Av1Nvenc, AudioCodec::Pcm), ContainerCompatLevel::Prohibited);
}

TEST(ContainerCompatRegistry, WebM_H264_Opus_IsProhibited) {
    // ADR 0010: H.264 is prohibited in WebM.
    EXPECT_EQ(Level(Container::WebM, VideoCodec::H264Nvenc, AudioCodec::Opus), ContainerCompatLevel::Prohibited);
}

TEST(ContainerCompatRegistry, WebM_H264_Aac_IsProhibited) {
    EXPECT_EQ(Level(Container::WebM, VideoCodec::H264Nvenc, AudioCodec::AacMf), ContainerCompatLevel::Prohibited);
}

TEST(ContainerCompatRegistry, WebM_H264_Pcm_IsProhibited) {
    EXPECT_EQ(Level(Container::WebM, VideoCodec::H264Nvenc, AudioCodec::Pcm), ContainerCompatLevel::Prohibited);
}

TEST(ContainerCompatRegistry, WebM_Hevc_Opus_IsProhibited) {
    // ADR 0010: HEVC is prohibited in WebM.
    EXPECT_EQ(Level(Container::WebM, VideoCodec::HevcNvenc, AudioCodec::Opus), ContainerCompatLevel::Prohibited);
}

TEST(ContainerCompatRegistry, WebM_Hevc_Aac_IsProhibited) {
    EXPECT_EQ(Level(Container::WebM, VideoCodec::HevcNvenc, AudioCodec::AacMf), ContainerCompatLevel::Prohibited);
}

TEST(ContainerCompatRegistry, WebM_Hevc_Pcm_IsProhibited) {
    EXPECT_EQ(Level(Container::WebM, VideoCodec::HevcNvenc, AudioCodec::Pcm), ContainerCompatLevel::Prohibited);
}

// ---------------------------------------------------------------------------
// IsContainerCompatSelectable / IsContainerCompatProhibited predicates
// ---------------------------------------------------------------------------

TEST(ContainerCompatRegistry, Selectable_Recommended_IsTrue) {
    EXPECT_TRUE(IsContainerCompatSelectable(ContainerCompatLevel::Recommended));
}

TEST(ContainerCompatRegistry, Selectable_Allowed_IsTrue) {
    EXPECT_TRUE(IsContainerCompatSelectable(ContainerCompatLevel::Allowed));
}

TEST(ContainerCompatRegistry, Selectable_Experimental_IsTrue) {
    EXPECT_TRUE(IsContainerCompatSelectable(ContainerCompatLevel::Experimental));
}

TEST(ContainerCompatRegistry, Selectable_Fallback_IsFalse) {
    EXPECT_FALSE(IsContainerCompatSelectable(ContainerCompatLevel::Fallback));
}

TEST(ContainerCompatRegistry, Selectable_Prohibited_IsFalse) {
    EXPECT_FALSE(IsContainerCompatSelectable(ContainerCompatLevel::Prohibited));
}

TEST(ContainerCompatRegistry, Prohibited_OnlyProhibited_IsTrue) {
    EXPECT_TRUE(IsContainerCompatProhibited(ContainerCompatLevel::Prohibited));
    EXPECT_FALSE(IsContainerCompatProhibited(ContainerCompatLevel::Recommended));
    EXPECT_FALSE(IsContainerCompatProhibited(ContainerCompatLevel::Allowed));
    EXPECT_FALSE(IsContainerCompatProhibited(ContainerCompatLevel::Experimental));
    EXPECT_FALSE(IsContainerCompatProhibited(ContainerCompatLevel::Fallback));
}

// ---------------------------------------------------------------------------
// Full 27-entry exhaustive table scan: every combination has a reason string
// and a defined level (never an uninitialised/garbage value)
// ---------------------------------------------------------------------------

TEST(ContainerCompatRegistry, AllCombinations_HaveNonEmptyReason) {
    int count = 0;
    for (const Container c : AllContainers()) {
        for (const VideoCodec v : AllVideoCodecs()) {
            for (const AudioCodec a : AllAudioCodecs()) {
                const ContainerCompatEntry entry = ContainerCompatRegistry::Query(c, v, a);
                EXPECT_FALSE(entry.reason.empty())
                    << "Empty reason for container=" << static_cast<int>(c) << " video=" << static_cast<int>(v)
                    << " audio=" << static_cast<int>(a);
                ++count;
            }
        }
    }
    // 3 containers × 3 video × 4 audio (Opus/AAC/PCM/FLAC).
    EXPECT_EQ(count, 36);
}

TEST(ContainerCompatRegistry, AllCombinations_LevelIsDefinedValue) {
    for (const Container c : AllContainers()) {
        for (const VideoCodec v : AllVideoCodecs()) {
            for (const AudioCodec a : AllAudioCodecs()) {
                const ContainerCompatLevel level = Level(c, v, a);
                EXPECT_TRUE(level == ContainerCompatLevel::Recommended || level == ContainerCompatLevel::Allowed ||
                            level == ContainerCompatLevel::Experimental || level == ContainerCompatLevel::Fallback ||
                            level == ContainerCompatLevel::Prohibited)
                    << "Unexpected level=" << static_cast<int>(level) << " for container=" << static_cast<int>(c)
                    << " video=" << static_cast<int>(v) << " audio=" << static_cast<int>(a);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Preferred codecs
// ---------------------------------------------------------------------------

TEST(ContainerCompatRegistry, PreferredAudio_Mkv_IsOpus) {
    EXPECT_EQ(ContainerCompatRegistry::PreferredAudioCodec(Container::Matroska), AudioCodec::Opus);
}

TEST(ContainerCompatRegistry, PreferredAudio_Mp4_IsAac) {
    EXPECT_EQ(ContainerCompatRegistry::PreferredAudioCodec(Container::Mp4), AudioCodec::AacMf);
}

TEST(ContainerCompatRegistry, PreferredAudio_WebM_IsOpus) {
    EXPECT_EQ(ContainerCompatRegistry::PreferredAudioCodec(Container::WebM), AudioCodec::Opus);
}

TEST(ContainerCompatRegistry, PreferredVideo_Mkv_IsAv1) {
    EXPECT_EQ(ContainerCompatRegistry::PreferredVideoCodec(Container::Matroska), VideoCodec::Av1Nvenc);
}

TEST(ContainerCompatRegistry, PreferredVideo_Mp4_IsH264) {
    EXPECT_EQ(ContainerCompatRegistry::PreferredVideoCodec(Container::Mp4), VideoCodec::H264Nvenc);
}

TEST(ContainerCompatRegistry, PreferredVideo_WebM_IsAv1) {
    EXPECT_EQ(ContainerCompatRegistry::PreferredVideoCodec(Container::WebM), VideoCodec::Av1Nvenc);
}

// ---------------------------------------------------------------------------
// ReconcileCodecs — preset-load reconciliation paths
// ---------------------------------------------------------------------------

TEST(ContainerCompatRegistry, Reconcile_Mkv_Av1_Opus_Unchanged) {
    VideoCodec v = VideoCodec::Av1Nvenc;
    AudioCodec a = AudioCodec::Opus;
    ContainerCompatRegistry::ReconcileCodecs(Container::Matroska, v, a);
    EXPECT_EQ(v, VideoCodec::Av1Nvenc);
    EXPECT_EQ(a, AudioCodec::Opus);
}

TEST(ContainerCompatRegistry, Reconcile_Mkv_Av1_Aac_Unchanged) {
    VideoCodec v = VideoCodec::Av1Nvenc;
    AudioCodec a = AudioCodec::AacMf;
    ContainerCompatRegistry::ReconcileCodecs(Container::Matroska, v, a);
    EXPECT_EQ(v, VideoCodec::Av1Nvenc);
    EXPECT_EQ(a, AudioCodec::AacMf);
}

TEST(ContainerCompatRegistry, Reconcile_Mkv_H264_Aac_Unchanged) {
    VideoCodec v = VideoCodec::H264Nvenc;
    AudioCodec a = AudioCodec::AacMf;
    ContainerCompatRegistry::ReconcileCodecs(Container::Matroska, v, a);
    EXPECT_EQ(v, VideoCodec::H264Nvenc);
    EXPECT_EQ(a, AudioCodec::AacMf);
}

TEST(ContainerCompatRegistry, Reconcile_Mkv_H264_Opus_Unchanged) {
    // MKV + H.264 + Opus is now Allowed (working combo). The reconciler must leave
    // it unchanged — no rewrite to AAC occurs.
    VideoCodec v = VideoCodec::H264Nvenc;
    AudioCodec a = AudioCodec::Opus;
    ContainerCompatRegistry::ReconcileCodecs(Container::Matroska, v, a);
    EXPECT_EQ(v, VideoCodec::H264Nvenc);
    EXPECT_EQ(a, AudioCodec::Opus);
}

TEST(ContainerCompatRegistry, Reconcile_Mkv_H264_Pcm_Unchanged) {
    // 0.6.0 Audio v2: MKV + H.264 + PCM is now Allowed (a working combo), so the
    // reconciler must leave it unchanged — no rewrite to AAC.
    VideoCodec v = VideoCodec::H264Nvenc;
    AudioCodec a = AudioCodec::Pcm;
    ContainerCompatRegistry::ReconcileCodecs(Container::Matroska, v, a);
    EXPECT_EQ(v, VideoCodec::H264Nvenc);
    EXPECT_EQ(a, AudioCodec::Pcm);
}

TEST(ContainerCompatRegistry, Reconcile_Mkv_Av1_Pcm_Unchanged) {
    // MKV + AV1 + PCM is Allowed; reconciler leaves it unchanged.
    VideoCodec v = VideoCodec::Av1Nvenc;
    AudioCodec a = AudioCodec::Pcm;
    ContainerCompatRegistry::ReconcileCodecs(Container::Matroska, v, a);
    EXPECT_EQ(v, VideoCodec::Av1Nvenc);
    EXPECT_EQ(a, AudioCodec::Pcm);
}

TEST(ContainerCompatRegistry, Reconcile_WebM_Av1_Pcm_FixesAudioToOpus) {
    // WebM cannot carry PCM (Prohibited). Reconciler keeps AV1 and swaps audio to
    // the WebM-preferred codec, Opus.
    VideoCodec v = VideoCodec::Av1Nvenc;
    AudioCodec a = AudioCodec::Pcm;
    ContainerCompatRegistry::ReconcileCodecs(Container::WebM, v, a);
    EXPECT_EQ(v, VideoCodec::Av1Nvenc);
    EXPECT_EQ(a, AudioCodec::Opus);
}

TEST(ContainerCompatRegistry, Reconcile_Mp4_H264_Pcm_FixesAudioToAac) {
    // MP4 cannot carry PCM in this build (Experimental). Reconciler keeps H.264
    // and swaps audio to AAC.
    VideoCodec v = VideoCodec::H264Nvenc;
    AudioCodec a = AudioCodec::Pcm;
    ContainerCompatRegistry::ReconcileCodecs(Container::Mp4, v, a);
    EXPECT_EQ(v, VideoCodec::H264Nvenc);
    EXPECT_EQ(a, AudioCodec::AacMf);
}

TEST(ContainerCompatRegistry, Reconcile_Mkv_Av1_Flac_Unchanged) {
    // MKV + AV1 + FLAC is Allowed; reconciler leaves it unchanged.
    VideoCodec v = VideoCodec::Av1Nvenc;
    AudioCodec a = AudioCodec::Flac;
    ContainerCompatRegistry::ReconcileCodecs(Container::Matroska, v, a);
    EXPECT_EQ(v, VideoCodec::Av1Nvenc);
    EXPECT_EQ(a, AudioCodec::Flac);
}

TEST(ContainerCompatRegistry, Reconcile_Mkv_H264_Flac_Unchanged) {
    // MKV + H.264 + FLAC is Allowed; reconciler leaves it unchanged.
    VideoCodec v = VideoCodec::H264Nvenc;
    AudioCodec a = AudioCodec::Flac;
    ContainerCompatRegistry::ReconcileCodecs(Container::Matroska, v, a);
    EXPECT_EQ(v, VideoCodec::H264Nvenc);
    EXPECT_EQ(a, AudioCodec::Flac);
}

TEST(ContainerCompatRegistry, Reconcile_WebM_Av1_Flac_FixesAudioToOpus) {
    // WebM cannot carry FLAC (Prohibited). Reconciler keeps AV1 and swaps audio
    // to the WebM-preferred codec, Opus.
    VideoCodec v = VideoCodec::Av1Nvenc;
    AudioCodec a = AudioCodec::Flac;
    ContainerCompatRegistry::ReconcileCodecs(Container::WebM, v, a);
    EXPECT_EQ(v, VideoCodec::Av1Nvenc);
    EXPECT_EQ(a, AudioCodec::Opus);
}

TEST(ContainerCompatRegistry, Reconcile_Mp4_H264_Flac_FixesAudioToAac) {
    // MP4 cannot carry FLAC in this build (Experimental). Reconciler keeps H.264
    // and swaps audio to AAC.
    VideoCodec v = VideoCodec::H264Nvenc;
    AudioCodec a = AudioCodec::Flac;
    ContainerCompatRegistry::ReconcileCodecs(Container::Mp4, v, a);
    EXPECT_EQ(v, VideoCodec::H264Nvenc);
    EXPECT_EQ(a, AudioCodec::AacMf);
}

TEST(ContainerCompatRegistry, Reconcile_Mkv_Hevc_Aac_FallsToAv1Opus) {
    // HEVC + AAC in MKV is Experimental (not selectable). Reconciler tries
    // audio-only fix first: HEVC + Opus is also Experimental. Then tries full
    // fallback: AV1 + Opus = Recommended.
    VideoCodec v = VideoCodec::HevcNvenc;
    AudioCodec a = AudioCodec::AacMf;
    ContainerCompatRegistry::ReconcileCodecs(Container::Matroska, v, a);
    EXPECT_EQ(v, VideoCodec::Av1Nvenc);
    EXPECT_EQ(a, AudioCodec::Opus);
}

TEST(ContainerCompatRegistry, Reconcile_Mp4_H264_Aac_Unchanged) {
    VideoCodec v = VideoCodec::H264Nvenc;
    AudioCodec a = AudioCodec::AacMf;
    ContainerCompatRegistry::ReconcileCodecs(Container::Mp4, v, a);
    EXPECT_EQ(v, VideoCodec::H264Nvenc);
    EXPECT_EQ(a, AudioCodec::AacMf);
}

TEST(ContainerCompatRegistry, Reconcile_Mp4_Av1_Opus_FixesToH264Aac) {
    // AV1 + Opus in MP4: Opus is Prohibited; Av1+AAC is Experimental (not
    // selectable). Both video+audio replaced with preferred.
    VideoCodec v = VideoCodec::Av1Nvenc;
    AudioCodec a = AudioCodec::Opus;
    ContainerCompatRegistry::ReconcileCodecs(Container::Mp4, v, a);
    EXPECT_EQ(v, VideoCodec::H264Nvenc);
    EXPECT_EQ(a, AudioCodec::AacMf);
}

TEST(ContainerCompatRegistry, Reconcile_Mp4_H264_Opus_FixesAudioToAac) {
    // H.264 + Opus in MP4 is Prohibited; must replace with AAC while keeping H.264.
    VideoCodec v = VideoCodec::H264Nvenc;
    AudioCodec a = AudioCodec::Opus;
    ContainerCompatRegistry::ReconcileCodecs(Container::Mp4, v, a);
    EXPECT_EQ(v, VideoCodec::H264Nvenc);
    EXPECT_EQ(a, AudioCodec::AacMf);
}

TEST(ContainerCompatRegistry, Reconcile_WebM_Av1_Opus_Unchanged) {
    VideoCodec v = VideoCodec::Av1Nvenc;
    AudioCodec a = AudioCodec::Opus;
    ContainerCompatRegistry::ReconcileCodecs(Container::WebM, v, a);
    EXPECT_EQ(v, VideoCodec::Av1Nvenc);
    EXPECT_EQ(a, AudioCodec::Opus);
}

TEST(ContainerCompatRegistry, Reconcile_WebM_H264_Aac_FixesToAv1Opus) {
    // H.264 + AAC in WebM is doubly Prohibited. Full fallback: AV1 + Opus.
    VideoCodec v = VideoCodec::H264Nvenc;
    AudioCodec a = AudioCodec::AacMf;
    ContainerCompatRegistry::ReconcileCodecs(Container::WebM, v, a);
    EXPECT_EQ(v, VideoCodec::Av1Nvenc);
    EXPECT_EQ(a, AudioCodec::Opus);
}

TEST(ContainerCompatRegistry, Reconcile_WebM_Hevc_Opus_FixesToAv1Opus) {
    // HEVC in WebM is Prohibited (codec itself). Even with Opus the video codec
    // is invalid. Full fallback: AV1 + Opus.
    VideoCodec v = VideoCodec::HevcNvenc;
    AudioCodec a = AudioCodec::Opus;
    ContainerCompatRegistry::ReconcileCodecs(Container::WebM, v, a);
    EXPECT_EQ(v, VideoCodec::Av1Nvenc);
    EXPECT_EQ(a, AudioCodec::Opus);
}

// ---------------------------------------------------------------------------
// Reconcile invariant: after ReconcileCodecs the result is always Recommended
// or Allowed (i.e. a working, implemented combination) for every input with
// every container.
// ---------------------------------------------------------------------------

TEST(ContainerCompatRegistry, Reconcile_AllInputs_ResultIsRecommendedOrAllowed) {
    int total = 0;
    for (const Container c : AllContainers()) {
        for (const VideoCodec v_in : AllVideoCodecs()) {
            for (const AudioCodec a_in : AllAudioCodecs()) {
                VideoCodec v = v_in;
                AudioCodec a = a_in;
                ContainerCompatRegistry::ReconcileCodecs(c, v, a);
                const ContainerCompatEntry entry = ContainerCompatRegistry::Query(c, v, a);
                const bool is_working =
                    entry.level == ContainerCompatLevel::Recommended || entry.level == ContainerCompatLevel::Allowed;
                EXPECT_TRUE(is_working) << "Not Recommended/Allowed after reconcile: container=" << static_cast<int>(c)
                                        << " input_video=" << static_cast<int>(v_in)
                                        << " input_audio=" << static_cast<int>(a_in)
                                        << " output_video=" << static_cast<int>(v)
                                        << " output_audio=" << static_cast<int>(a) << " level=" << ToString(entry.level)
                                        << " reason=" << entry.reason;
                ++total;
            }
        }
    }
    // 3 containers × 3 video × 4 audio (Opus/AAC/PCM/FLAC).
    EXPECT_EQ(total, 36);
}

// ---------------------------------------------------------------------------
// ToString
// ---------------------------------------------------------------------------

TEST(ContainerCompatRegistry, ToString_AllLevels_NonEmpty) {
    EXPECT_FALSE(ToString(ContainerCompatLevel::Recommended).empty());
    EXPECT_FALSE(ToString(ContainerCompatLevel::Allowed).empty());
    EXPECT_FALSE(ToString(ContainerCompatLevel::Experimental).empty());
    EXPECT_FALSE(ToString(ContainerCompatLevel::Fallback).empty());
    EXPECT_FALSE(ToString(ContainerCompatLevel::Prohibited).empty());
}

TEST(ContainerCompatRegistry, ToString_Recommended_IsRecommended) {
    EXPECT_EQ(ToString(ContainerCompatLevel::Recommended), "Recommended");
}

TEST(ContainerCompatRegistry, ToString_Prohibited_IsProhibited) {
    EXPECT_EQ(ToString(ContainerCompatLevel::Prohibited), "Prohibited");
}

} // namespace
} // namespace exosnap::capability
