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
