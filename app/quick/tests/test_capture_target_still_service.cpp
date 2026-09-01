// The source picker's still pipeline, end to end against a real window.
//
// Everything else about the picker can be tested against seeded rows, but the
// one question those cannot answer is whether a grab produces pixels: the
// visual harness seeds capture targets with invented native ids, so a WGC grab
// there fails by construction and every card shows its placeholder. This test
// creates a window of a known colour and asserts that colour comes back out of
// the service, which exercises the WGC session, the mip downscale and the
// readback in one pass.
//
// Labelled `live`: it needs a real desktop session with DWM composition.

#include "services/CaptureTargetStillService.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QTimer>

#include <windows.h>

#include <winrt/Windows.Graphics.Capture.h>

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

} // namespace

TEST(CaptureTargetStillServiceTest, GrabsTheColourOfARealWindow) {
    // Built here rather than in a custom main: the service delivers its result
    // through a queued connection, which needs an event loop, and this binary
    // holds one test.
    static char program_name[] = "capture_target_still_tests";
    static char* argv[] = {program_name, nullptr};
    static int argc = 1;
    QCoreApplication app(argc, argv);

    if (!wgcAvailable())
        GTEST_SKIP() << "Windows.Graphics.Capture is not available in this session";

    ColourWindow probe;
    ASSERT_NE(probe.handle(), nullptr) << "could not create the probe window";

    exosnap::CaptureTargetStillService service;
    QImage still;
    QString failed_identity;
    QEventLoop loop;
    QObject::connect(&service, &exosnap::CaptureTargetStillService::stillReady, &service,
                     [&still, &loop](const QString&, const QImage& image) {
                         still = image;
                         loop.quit();
                     });
    QObject::connect(&service, &exosnap::CaptureTargetStillService::stillUnavailable, &service,
                     [&failed_identity, &loop](const QString& identity) {
                         failed_identity = identity;
                         loop.quit();
                     });

    exosnap::engine::CaptureTarget target;
    target.kind = exosnap::engine::CaptureTarget::Kind::Window;
    target.native_id = reinterpret_cast<uintptr_t>(probe.handle());
    target.description = "ExoSnap still probe";
    service.setVisibleTargets({{QStringLiteral("window:probe"), target}});
    service.start();

    // The probe's own message pump has to keep running while the service's
    // worker waits for a frame, or the window never paints and the grab times
    // out against a blank surface.
    QTimer pump;
    QObject::connect(&pump, &QTimer::timeout, &pump, [&probe]() { probe.pump(); });
    pump.start(10);

    QTimer::singleShot(kGrabTimeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
    service.stop();

    ASSERT_TRUE(failed_identity.isEmpty()) << "the service reported the probe window as uncapturable";
    ASSERT_FALSE(still.isNull()) << "no still arrived within " << kGrabTimeoutMs << " ms";
    EXPECT_GT(still.width(), 0);
    EXPECT_GT(still.height(), 0);

    const QColor centre = still.pixelColor(still.width() / 2, still.height() / 2);
    // Not an exact compare: the mip chain filters, and a window edge inside the
    // captured surface would bleed into the centre only if the downscale were
    // wrong by a lot. A tolerance of 8 catches that without failing on rounding.
    EXPECT_NEAR(centre.red(), kColourRed, 8);
    EXPECT_NEAR(centre.green(), kColourGreen, 8);
    EXPECT_NEAR(centre.blue(), kColourBlue, 8);
}
