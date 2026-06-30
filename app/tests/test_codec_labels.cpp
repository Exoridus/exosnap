#include <gtest/gtest.h>

#include <QString>

#include "ui/CodecLabels.h"

// capability:: lives in exosnap::capability (nested); recorder_core:: is global.
// Sit in namespace exosnap so the unqualified capability:: resolves like it does
// in ConfigPage/RecordPage.
namespace exosnap {
namespace {

using exosnap::ui::audioCodecLabel;
using exosnap::ui::containerLabel;
using exosnap::ui::frameRateLabel;
using exosnap::ui::videoCodecLabel;

// Locks the user-visible naming canon (feedback_codec_naming_canon) and, just as
// importantly, that the two enum-family overloads agree byte-for-byte — the
// WEBM-vs-WebM divergence between ConfigPage and RecordPage is what motivated
// folding both into one header.

TEST(CodecLabelsTest, ContainerCanonCasingAgreesAcrossEnumFamilies) {
    EXPECT_EQ(containerLabel(capability::Container::Matroska), QStringLiteral("MKV"));
    EXPECT_EQ(containerLabel(capability::Container::Mp4), QStringLiteral("MP4"));
    EXPECT_EQ(containerLabel(capability::Container::WebM), QStringLiteral("WebM"));

    EXPECT_EQ(containerLabel(recorder_core::Container::Matroska), containerLabel(capability::Container::Matroska));
    EXPECT_EQ(containerLabel(recorder_core::Container::Mp4), containerLabel(capability::Container::Mp4));
    EXPECT_EQ(containerLabel(recorder_core::Container::WebM), containerLabel(capability::Container::WebM));
    EXPECT_EQ(containerLabel(recorder_core::Container::WebM), QStringLiteral("WebM"));
}

TEST(CodecLabelsTest, VideoCodecCanonCasing) {
    EXPECT_EQ(videoCodecLabel(capability::VideoCodec::H264Nvenc), QStringLiteral("H.264"));
    EXPECT_EQ(videoCodecLabel(capability::VideoCodec::HevcNvenc), QStringLiteral("HEVC"));
    EXPECT_EQ(videoCodecLabel(capability::VideoCodec::Av1Nvenc), QStringLiteral("AV1"));

    EXPECT_EQ(videoCodecLabel(recorder_core::VideoCodec::H264Nvenc), QStringLiteral("H.264"));
    EXPECT_EQ(videoCodecLabel(recorder_core::VideoCodec::HevcNvenc), QStringLiteral("HEVC"));
    EXPECT_EQ(videoCodecLabel(recorder_core::VideoCodec::Av1Nvenc), QStringLiteral("AV1"));
}

TEST(CodecLabelsTest, AudioCodecCanonCasingNeverUppercaseOpus) {
    EXPECT_EQ(audioCodecLabel(capability::AudioCodec::Opus), QStringLiteral("Opus"));
    EXPECT_EQ(audioCodecLabel(capability::AudioCodec::AacMf), QStringLiteral("AAC"));
    EXPECT_EQ(audioCodecLabel(capability::AudioCodec::Pcm), QStringLiteral("PCM"));
    EXPECT_EQ(audioCodecLabel(capability::AudioCodec::Flac), QStringLiteral("FLAC"));

    EXPECT_EQ(audioCodecLabel(recorder_core::AudioCodec::Opus), QStringLiteral("Opus"));
    EXPECT_EQ(audioCodecLabel(recorder_core::AudioCodec::AacMf), QStringLiteral("AAC"));
    EXPECT_EQ(audioCodecLabel(recorder_core::AudioCodec::Pcm), QStringLiteral("PCM"));
    EXPECT_EQ(audioCodecLabel(recorder_core::AudioCodec::Flac), QStringLiteral("FLAC"));
}

TEST(CodecLabelsTest, FrameRateLabelFormats) {
    EXPECT_EQ(frameRateLabel(60, 1), QStringLiteral("60 fps"));
    EXPECT_EQ(frameRateLabel(144, 1), QStringLiteral("144 fps"));
    EXPECT_EQ(frameRateLabel(0, 0), QStringLiteral("60 fps")); // guard: 0 denom -> default
    EXPECT_EQ(frameRateLabel(30000, 1001), QStringLiteral("30000/1001 fps"));
}

} // namespace
} // namespace exosnap
