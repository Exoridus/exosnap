#include <gtest/gtest.h>

#include "../auto_record/AutoRecordHarness.h"

using exosnap::auto_record::AutoRecordOptions;
using exosnap::auto_record::HasAutoRecordRequest;
using exosnap::auto_record::HdrMode;
using exosnap::auto_record::ParseAutoRecordOptions;
using exosnap::auto_record::TargetKind;

TEST(AutoRecordHarness, HasRequestDetectsFlag) {
    EXPECT_TRUE(HasAutoRecordRequest({QStringLiteral("exosnap.exe"), QStringLiteral("--auto-record")}));
    EXPECT_FALSE(HasAutoRecordRequest({QStringLiteral("exosnap.exe")}));
}

TEST(AutoRecordHarness, ParsesDefaults) {
    AutoRecordOptions opts;
    QString error;
    ASSERT_TRUE(ParseAutoRecordOptions({QStringLiteral("exosnap.exe"), QStringLiteral("--auto-record")}, &opts, &error))
        << error.toStdString();
    EXPECT_EQ(opts.target, TargetKind::Monitor);
    EXPECT_EQ(opts.duration_seconds, 10);
    EXPECT_EQ(opts.container, QStringLiteral("mkv"));
    EXPECT_FALSE(opts.enable_preview);
    EXPECT_EQ(opts.repeat_cycles, 1);
}

TEST(AutoRecordHarness, ParsesRepeatCycles) {
    AutoRecordOptions opts;
    QString error;
    const QStringList args = {QStringLiteral("exosnap.exe"), QStringLiteral("--auto-record"),
                              QStringLiteral("--repeat-cycles"), QStringLiteral("5")};
    ASSERT_TRUE(ParseAutoRecordOptions(args, &opts, &error)) << error.toStdString();
    EXPECT_EQ(opts.repeat_cycles, 5);
}

TEST(AutoRecordHarness, RejectsNonPositiveRepeatCycles) {
    AutoRecordOptions opts;
    QString error;
    const QStringList args = {QStringLiteral("exosnap.exe"), QStringLiteral("--auto-record"),
                              QStringLiteral("--repeat-cycles"), QStringLiteral("0")};
    EXPECT_FALSE(ParseAutoRecordOptions(args, &opts, &error));
    EXPECT_FALSE(error.isEmpty());
}

TEST(AutoRecordHarness, ParsesFullOptionSet) {
    AutoRecordOptions opts;
    QString error;
    const QStringList args = {
        QStringLiteral("exosnap.exe"),
        QStringLiteral("--auto-record"),
        QStringLiteral("--target"),
        QStringLiteral("window"),
        QStringLiteral("--target-window-title"),
        QStringLiteral("Notepad"),
        QStringLiteral("--audio-rows"),
        QStringLiteral("app,sys"),
        QStringLiteral("--merge-above"),
        QStringLiteral("sys"),
        QStringLiteral("--container"),
        QStringLiteral("mp4"),
        QStringLiteral("--video-codec"),
        QStringLiteral("hevc"),
        QStringLiteral("--chroma"),
        QStringLiteral("444"),
        QStringLiteral("--bit-depth"),
        QStringLiteral("8"),
        QStringLiteral("--hdr"),
        QStringLiteral("native"),
        QStringLiteral("--duration"),
        QStringLiteral("6"),
        QStringLiteral("--capture-frame-at"),
        QStringLiteral("3"),
    };
    ASSERT_TRUE(ParseAutoRecordOptions(args, &opts, &error)) << error.toStdString();
    EXPECT_EQ(opts.target, TargetKind::Window);
    EXPECT_EQ(opts.target_window_title, QStringLiteral("Notepad"));
    EXPECT_EQ(opts.audio_rows, (QStringList{QStringLiteral("app"), QStringLiteral("sys")}));
    EXPECT_EQ(opts.merge_above, QStringLiteral("sys"));
    EXPECT_EQ(opts.container, QStringLiteral("mp4"));
    EXPECT_EQ(opts.video_codec, QStringLiteral("hevc"));
    EXPECT_EQ(opts.chroma, 444);
    EXPECT_EQ(opts.hdr_mode, HdrMode::Native);
    EXPECT_EQ(opts.duration_seconds, 6);
    EXPECT_EQ(opts.capture_frame_at_seconds, 3);
}

// Frame rate was hard-coded to 60 in the harness, which made every rate the
// product offers except 60 unreachable from the command line -- and so
// unverifiable without driving the UI by hand.
TEST(AutoRecordHarness, ParsesFrameRate) {
    AutoRecordOptions opts;
    QString error;
    const QStringList args = {QStringLiteral("exosnap.exe"), QStringLiteral("--auto-record"),
                              QStringLiteral("--frame-rate"), QStringLiteral("120")};
    ASSERT_TRUE(ParseAutoRecordOptions(args, &opts, &error)) << error.toStdString();
    EXPECT_EQ(opts.frame_rate, 120);
}

TEST(AutoRecordHarness, FrameRateDefaultsToSixty) {
    AutoRecordOptions opts;
    QString error;
    const QStringList args = {QStringLiteral("exosnap.exe"), QStringLiteral("--auto-record")};
    ASSERT_TRUE(ParseAutoRecordOptions(args, &opts, &error)) << error.toStdString();
    EXPECT_EQ(opts.frame_rate, 60);
}

TEST(AutoRecordHarness, RejectsFrameRateOutsideWhatTheProductOffers) {
    // The Expert frame-rate field accepts 1-240; anything outside that is a
    // typo, and silently recording at 60 instead would make a verification run
    // claim to prove something it never exercised.
    AutoRecordOptions opts;
    QString error;
    for (const auto& bad : {QStringLiteral("0"), QStringLiteral("-1"), QStringLiteral("241"), QStringLiteral("abc")}) {
        const QStringList args = {QStringLiteral("exosnap.exe"), QStringLiteral("--auto-record"),
                                  QStringLiteral("--frame-rate"), bad};
        EXPECT_FALSE(ParseAutoRecordOptions(args, &opts, &error)) << "accepted " << bad.toStdString();
        EXPECT_FALSE(error.isEmpty());
    }
}

TEST(AutoRecordHarness, ParsesCq) {
    AutoRecordOptions opts;
    QString error;
    const QStringList args = {QStringLiteral("exosnap.exe"), QStringLiteral("--auto-record"), QStringLiteral("--cq"),
                              QStringLiteral("1")};
    ASSERT_TRUE(ParseAutoRecordOptions(args, &opts, &error)) << error.toStdString();
    EXPECT_EQ(opts.cq, 1);
}

TEST(AutoRecordHarness, CqDefaultsToTheBalancedTier) {
    AutoRecordOptions opts;
    QString error;
    const QStringList args = {QStringLiteral("exosnap.exe"), QStringLiteral("--auto-record")};
    ASSERT_TRUE(ParseAutoRecordOptions(args, &opts, &error)) << error.toStdString();
    // AutoRecordHarness.cpp static_asserts this 24 against CanonicalCq(Balanced).
    EXPECT_EQ(opts.cq, 24);
}

TEST(AutoRecordHarness, RejectsCqOutsideTheCanonicalRange) {
    // The canonical CQ range is 1-51 for every codec; the per-codec quantizer
    // conversion happens below this, so a value outside it is a typo rather than
    // an exotic request.
    AutoRecordOptions opts;
    QString error;
    for (const auto& bad : {QStringLiteral("0"), QStringLiteral("-1"), QStringLiteral("52"), QStringLiteral("abc")}) {
        const QStringList args = {QStringLiteral("exosnap.exe"), QStringLiteral("--auto-record"),
                                  QStringLiteral("--cq"), bad};
        EXPECT_FALSE(ParseAutoRecordOptions(args, &opts, &error)) << "accepted " << bad.toStdString();
        EXPECT_FALSE(error.isEmpty());
    }
}

TEST(AutoRecordHarness, ParsesNvencPreset) {
    AutoRecordOptions opts;
    QString error;
    const QStringList args = {QStringLiteral("exosnap.exe"), QStringLiteral("--auto-record"),
                              QStringLiteral("--nvenc-preset"), QStringLiteral("7")};
    ASSERT_TRUE(ParseAutoRecordOptions(args, &opts, &error)) << error.toStdString();
    EXPECT_EQ(opts.nvenc_preset, 7);
}

TEST(AutoRecordHarness, NvencPresetDefaultsToTheShippedP4) {
    AutoRecordOptions opts;
    QString error;
    const QStringList args = {QStringLiteral("exosnap.exe"), QStringLiteral("--auto-record")};
    ASSERT_TRUE(ParseAutoRecordOptions(args, &opts, &error)) << error.toStdString();
    EXPECT_EQ(opts.nvenc_preset, 4);
}

TEST(AutoRecordHarness, RejectsNvencPresetOutsideTheRange) {
    AutoRecordOptions opts;
    QString error;
    for (const auto& bad : {QStringLiteral("0"), QStringLiteral("-1"), QStringLiteral("8"), QStringLiteral("p4")}) {
        const QStringList args = {QStringLiteral("exosnap.exe"), QStringLiteral("--auto-record"),
                                  QStringLiteral("--nvenc-preset"), bad};
        EXPECT_FALSE(ParseAutoRecordOptions(args, &opts, &error)) << "accepted " << bad.toStdString();
        EXPECT_FALSE(error.isEmpty());
    }
}

TEST(AutoRecordHarness, ParsesCaptureFrameInReadyFlag) {
    AutoRecordOptions opts;
    QString error;
    const QStringList args = {QStringLiteral("exosnap.exe"), QStringLiteral("--auto-record"),
                              QStringLiteral("--enable-preview"), QStringLiteral("--capture-frame-in-ready")};
    ASSERT_TRUE(ParseAutoRecordOptions(args, &opts, &error)) << error.toStdString();
    EXPECT_TRUE(opts.capture_frame_in_ready);
}

TEST(AutoRecordHarness, CaptureFrameInReadyDefaultsFalse) {
    AutoRecordOptions opts;
    QString error;
    const QStringList args = {QStringLiteral("exosnap.exe"), QStringLiteral("--auto-record")};
    ASSERT_TRUE(ParseAutoRecordOptions(args, &opts, &error)) << error.toStdString();
    EXPECT_FALSE(opts.capture_frame_in_ready);
}

TEST(AutoRecordHarness, RejectsWindowTargetWithoutTitle) {
    AutoRecordOptions opts;
    QString error;
    const QStringList args = {QStringLiteral("exosnap.exe"), QStringLiteral("--auto-record"),
                              QStringLiteral("--target"), QStringLiteral("window")};
    EXPECT_FALSE(ParseAutoRecordOptions(args, &opts, &error));
    EXPECT_FALSE(error.isEmpty());
}

TEST(AutoRecordHarness, RejectsUnknownValue) {
    AutoRecordOptions opts;
    QString error;
    const QStringList args = {QStringLiteral("exosnap.exe"), QStringLiteral("--auto-record"),
                              QStringLiteral("--container"), QStringLiteral("avi")};
    EXPECT_FALSE(ParseAutoRecordOptions(args, &opts, &error));
}
