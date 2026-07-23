#include <gtest/gtest.h>

extern "C" {
#include <libavcodec/avcodec.h>
}

// Proves the vendored FFmpeg build (exosnap-ffmpeg-build) actually has the
// libx264/libx265 encoders compiled in. This is a check on the *build
// artifact's capability*, not a test of ExoSnap production code -- there is
// no X264VideoEncoder/HEVC equivalent wired up yet (ADR 0007 covers x264;
// wiring either into VideoEncoderFactory/IVideoEncoder is separate, future
// work). Unlike FfmpegAacEncoderTest's graceful-skip pattern (which
// tolerates whatever FFmpeg build happens to be pinned), these assert hard:
// a null result here means the vendored build is wrong, not that the test
// should degrade.
TEST(FfmpegBuildCapabilitiesTest, LibX264EncoderIsRegistered) {
    const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    ASSERT_NE(codec, nullptr) << "libx264 encoder not found in the vendored FFmpeg build -- "
                                 "was --enable-libx264/--enable-encoder=libx264 set?";
}

TEST(FfmpegBuildCapabilitiesTest, LibX265EncoderIsRegistered) {
    const AVCodec* codec = avcodec_find_encoder_by_name("libx265");
    ASSERT_NE(codec, nullptr) << "libx265 encoder not found in the vendored FFmpeg build -- "
                                 "was --enable-libx265/--enable-encoder=libx265 set?";
}
