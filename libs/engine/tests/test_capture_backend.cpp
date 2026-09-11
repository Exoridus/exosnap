#include <gtest/gtest.h>

#include <exosnap/engine/recorder_session.h>

namespace exosnap::engine {
namespace {

TEST(CaptureBackend, DefaultsMonitorToDxgiOutputDuplication) {
    RecorderConfig config;
    config.target.kind = CaptureTarget::Kind::Monitor;

    EXPECT_EQ(ResolveCaptureBackend(config), EffectiveCaptureBackend::DxgiOutputDuplication);
}

TEST(CaptureBackend, DefaultsWindowToWindowsGraphicsCapture) {
    RecorderConfig config;
    config.target.kind = CaptureTarget::Kind::Window;

    EXPECT_EQ(ResolveCaptureBackend(config), EffectiveCaptureBackend::WindowsGraphicsCapture);
}

TEST(CaptureBackend, WgcOverrideChangesOnlyMonitorSelection) {
    RecorderConfig config;
    config.target.kind = CaptureTarget::Kind::Monitor;
    config.capture_backend = CaptureBackend::WindowsGraphicsCapture;

    EXPECT_EQ(ResolveCaptureBackend(config), EffectiveCaptureBackend::WindowsGraphicsCapture);
}

TEST(CaptureBackend, DiagnosticNameDescribesResolvedBackend) {
    EXPECT_STREQ(CaptureBackendName(EffectiveCaptureBackend::DxgiOutputDuplication), "dxgi_od");
    EXPECT_STREQ(CaptureBackendName(EffectiveCaptureBackend::WindowsGraphicsCapture), "wgc");
}

TEST(CaptureBackend, SessionReportNameKeepsThePublishedHyphenatedSpelling) {
    EXPECT_STREQ(CaptureBackendReportName(EffectiveCaptureBackend::DxgiOutputDuplication), "dxgi-od");
    EXPECT_STREQ(CaptureBackendReportName(EffectiveCaptureBackend::WindowsGraphicsCapture), "wgc");
}

TEST(CaptureBackend, SessionReportNameFollowsTheMonitorWgcOverride) {
    RecorderConfig config;
    config.target.kind = CaptureTarget::Kind::Monitor;
    EXPECT_STREQ(CaptureBackendReportName(ResolveCaptureBackend(config)), "dxgi-od");

    config.capture_backend = CaptureBackend::WindowsGraphicsCapture;
    EXPECT_STREQ(CaptureBackendReportName(ResolveCaptureBackend(config)), "wgc");
}

} // namespace
} // namespace exosnap::engine
