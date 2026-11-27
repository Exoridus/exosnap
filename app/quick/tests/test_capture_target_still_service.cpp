// Native WGC still capture for a window with known pixels and a physical display.
// A real desktop session with DWM composition is required.

#include "services/CaptureTargetStillService.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QImage>
#include <QTimer>

#include <windows.h>

#include <winrt/Windows.Graphics.Capture.h>

#include <algorithm>

namespace {

// Kept as components rather than read back out of the COLORREF: the GetxValue
// macros cast a constant expression down to BYTE, which this build treats as a
// truncation error.
constexpr int kColourRed = 0;
constexpr int kColourGreen = 128;
constexpr int kColourBlue = 255;
constexpr COLORREF kWindowColour = RGB(kColourRed, kColourGreen, kColourBlue);
constexpr int kGrabTimeoutMs = 8000;

// A visible top-level window filled with one colour.
//
// WS_EX_NOACTIVATE plus SW_SHOWNOACTIVATE: the window must be on screen for DWM
// to render it, but taking focus from whoever is at the machine is not part of
// running a test.
class ColourWindow {
  public:
    ColourWindow() {
        WNDCLASSEXW cls{};
        cls.cbSize = sizeof(cls);
        cls.lpfnWndProc = DefWindowProcW;
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.hbrBackground = CreateSolidBrush(kWindowColour);
        cls.lpszClassName = L"ExoSnapStillProbeWindow";
        atom_ = RegisterClassExW(&cls);
        brush_ = cls.hbrBackground;
        if (atom_ == 0)
            return;

        window_ = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, reinterpret_cast<LPCWSTR>(atom_),
                                  L"ExoSnap still probe", WS_POPUP, 40, 40, 320, 240, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
        if (window_ == nullptr)
            return;
        ShowWindow(window_, SW_SHOWNOACTIVATE);
        UpdateWindow(window_);
        pump();
    }

    ~ColourWindow() {
        if (window_ != nullptr)
            DestroyWindow(window_);
        if (atom_ != 0)
            UnregisterClassW(reinterpret_cast<LPCWSTR>(atom_), GetModuleHandleW(nullptr));
        if (brush_ != nullptr)
            DeleteObject(brush_);
    }

    ColourWindow(const ColourWindow&) = delete;
    ColourWindow& operator=(const ColourWindow&) = delete;

    [[nodiscard]] HWND handle() const noexcept {
        return window_;
    }

    // The probe window lives on the test thread, so nothing repaints it unless
    // this thread dispatches its messages.
    void pump() {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

  private:
    ATOM atom_ = 0;
    HWND window_ = nullptr;
    HBRUSH brush_ = nullptr;
};

bool wgcAvailable() {
    try {
        return winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported();
    } catch (const winrt::hresult_error&) {
        return false;
    }
}

void EnsureApplication() {
    static char program_name[] = "capture_target_still_tests";
    static char* argv[] = {program_name, nullptr};
    static int argc = 1;
    static const QCoreApplication app(argc, argv);
}

QImage GrabStill(const QString& identity, const exosnap::engine::CaptureTarget& target) {
    exosnap::CaptureTargetStillService service;
    QImage still;
    QEventLoop loop;
    QObject::connect(&service, &exosnap::CaptureTargetStillService::stillReady, &loop,
                     [&](const QString& actual_identity, const QImage& image) {
                         EXPECT_EQ(actual_identity, identity);
                         still = image;
                         loop.quit();
                     });
    QObject::connect(&service, &exosnap::CaptureTargetStillService::stillUnavailable, &loop,
                     [&](const QString& actual_identity) {
                         EXPECT_EQ(actual_identity, identity);
                         ADD_FAILURE() << "The WGC target was reported unavailable";
                         loop.quit();
                     });

    service.setVisibleTargets({{identity, target}});
    service.start();
    QTimer::singleShot(kGrabTimeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
    service.stop();
    return still;
}

} // namespace

TEST(CaptureTargetStillServiceTest, GrabsTheColourOfARealWindow) {
    EnsureApplication();
    if (!wgcAvailable())
        GTEST_SKIP() << "Windows.Graphics.Capture is not available in this session";

    ColourWindow probe;
    ASSERT_NE(probe.handle(), nullptr) << "could not create the probe window";

    // The probe needs message dispatch while the worker waits for the first frame.
    QTimer pump;
    QObject::connect(&pump, &QTimer::timeout, &pump, [&probe]() { probe.pump(); });
    pump.start(10);

    exosnap::engine::CaptureTarget target;
    target.kind = exosnap::engine::CaptureTarget::Kind::Window;
    target.native_id = reinterpret_cast<uintptr_t>(probe.handle());
    target.description = "ExoSnap still probe";
    const QImage still = GrabStill(QStringLiteral("window:probe"), target);
    ASSERT_FALSE(still.isNull()) << "no still arrived within " << kGrabTimeoutMs << " ms";

    const QColor centre = still.pixelColor(still.width() / 2, still.height() / 2);
    // Not an exact compare: the mip chain filters, and a window edge inside the
    // captured surface would bleed into the centre only if the downscale were
    // wrong by a lot. A tolerance of 8 catches that without failing on rounding.
    EXPECT_NEAR(centre.red(), kColourRed, 8);
    EXPECT_NEAR(centre.green(), kColourGreen, 8);
    EXPECT_NEAR(centre.blue(), kColourBlue, 8);
}

TEST(CaptureTargetStillServiceTest, GrabsARealDisplay) {
    EnsureApplication();
    if (!wgcAvailable())
        GTEST_SKIP() << "Windows.Graphics.Capture is not available in this session";

    const HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    ASSERT_NE(monitor, nullptr);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    ASSERT_TRUE(GetMonitorInfoW(monitor, &info));
    const auto width = info.rcMonitor.right - info.rcMonitor.left;
    const auto height = info.rcMonitor.bottom - info.rcMonitor.top;
    ASSERT_GT(width, 0);
    ASSERT_GT(height, 0);

    exosnap::engine::CaptureTarget target;
    target.kind = exosnap::engine::CaptureTarget::Kind::Monitor;
    target.native_id = reinterpret_cast<uintptr_t>(monitor);
    target.description = "ExoSnap display probe";
    const QImage still = GrabStill(QStringLiteral("display:probe"), target);
    ASSERT_FALSE(still.isNull()) << "no still arrived within " << kGrabTimeoutMs << " ms";

    const auto max_edge = exosnap::CaptureTargetStillService::kMaxStillEdge;
    const double scale = std::min(1.0, static_cast<double>(max_edge) / std::max(width, height));
    EXPECT_EQ(still.format(), QImage::Format_RGB32);
    EXPECT_GT(still.width(), 0);
    EXPECT_GT(still.height(), 0);
    EXPECT_LE(std::max(still.width(), still.height()), max_edge);
    // The GPU mip and the final Qt resize round dimensions independently.
    EXPECT_NEAR(still.width(), width * scale, 2.0);
    EXPECT_NEAR(still.height(), height * scale, 2.0);
}
