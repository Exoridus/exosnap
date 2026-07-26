// Unit tests for HumanizeEngineDetail — the pure engine-detail humanizer used by
// RecordingErrorPanel to strip raw enum tokens out of on-screen error detail.

#include <gtest/gtest.h>

#include <QString>

#include "ui/dialogs/RecordingErrorDetailText.h"

namespace exosnap::ui::dialogs {
namespace {

TEST(RecordingErrorDetailText, MapsQualifiedContainerAndVideoTokens) {
    const QString in = QStringLiteral(
        "Container::Matroska requires VideoCodec::Av1Nvenc, VideoCodec::H264Nvenc, or VideoCodec::HevcNvenc");
    const QString out = HumanizeEngineDetail(in);
    EXPECT_EQ(out, QStringLiteral("MKV requires AV1, H.264, or HEVC"));
}

TEST(RecordingErrorDetailText, MapsBareContainerName) {
    EXPECT_EQ(HumanizeEngineDetail(QStringLiteral("Failed to build hvcC from HEVC VPS/SPS/PPS for Matroska")),
              QStringLiteral("Failed to build hvcC from HEVC VPS/SPS/PPS for MKV"));
}

TEST(RecordingErrorDetailText, MapsAudioTokens) {
    EXPECT_EQ(HumanizeEngineDetail(QStringLiteral("AudioCodec::Aac not allowed with Container::WebM")),
              QStringLiteral("AAC not allowed with WebM"));
    EXPECT_EQ(HumanizeEngineDetail(QStringLiteral("AudioCodec::Pcm and AudioCodec::Flac are Matroska-only")),
              QStringLiteral("PCM and FLAC are MKV-only"));
}

TEST(RecordingErrorDetailText, LeavesAlreadyCanonicalLabelsUntouched) {
    // "Opus" and "WebM" are already canon; "HEVC" is not a token here.
    EXPECT_EQ(HumanizeEngineDetail(QStringLiteral("Opus is valid for WebM and MKV-style muxing")),
              QStringLiteral("Opus is valid for WebM and MKV-style muxing"));
}

TEST(RecordingErrorDetailText, UnknownStringPassesThroughUnchanged) {
    const QString in = QStringLiteral("Disk write stalled for 3200 ms; output device may be full");
    EXPECT_EQ(HumanizeEngineDetail(in), in);
}

TEST(RecordingErrorDetailText, DoesNotRewriteTokenInsideLargerIdentifier) {
    // "Mp4Sink" must not become "MP4Sink"; word boundaries protect it.
    const QString in = QStringLiteral("Mp4Sink initialization returned E_FAIL");
    EXPECT_EQ(HumanizeEngineDetail(in), in);
}

TEST(RecordingErrorDetailText, EmptyStringIsEmpty) {
    EXPECT_EQ(HumanizeEngineDetail(QString()), QString());
}

} // namespace
} // namespace exosnap::ui::dialogs
