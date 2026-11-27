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
#include "services/WgcSourceProducer.h"

#include "../../diagnostics/AppLog.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <semaphore>
#include <string>
#include <utility>

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
    explicit ColourWindow(COLORREF colour = kWindowColour, int x = 40) {
        const auto class_name = L"ExoSnapStillProbeWindow" + std::to_wstring(x);
        WNDCLASSEXW cls{};
        cls.cbSize = sizeof(cls);
        cls.lpfnWndProc = DefWindowProcW;
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.hbrBackground = CreateSolidBrush(colour);
        cls.lpszClassName = class_name.c_str();
        atom_ = RegisterClassExW(&cls);
        brush_ = cls.hbrBackground;
        if (atom_ == 0)
            return;

        window_ = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, reinterpret_cast<LPCWSTR>(atom_),
                                  L"ExoSnap still probe", WS_POPUP, x, 40, 320, 240, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
        if (window_ == nullptr)
            return;
        ShowWindow(window_, SW_SHOWNOACTIVATE);
        UpdateWindow(window_);
        pump();
    }

    ~ColourWindow() {
        close();
        if (brush_ != nullptr)
            DeleteObject(brush_);
    }

    void close() {
        if (window_ != nullptr) {
            DestroyWindow(window_);
            window_ = nullptr;
        }
        if (atom_ != 0) {
            UnregisterClassW(reinterpret_cast<LPCWSTR>(atom_), GetModuleHandleW(nullptr));
            atom_ = 0;
        }
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

void ensureApplication() {
    static char program_name[] = "capture_target_still_tests";
    static char* argv[] = {program_name, nullptr};
    static int argc = 1;
    static const QCoreApplication app(argc, argv);
}

auto captureItemsCreated() {
    const auto log = exosnap::diagnostics::AppLog::history();
    return std::count_if(log.begin(), log.end(), [](const auto& entry) {
        return entry.category == QStringLiteral("wgc-producer") &&
               entry.message == QStringLiteral("capture item created");
    });
}

exosnap::CaptureTargetStillService::Request probeRequest(QString identity, HWND handle) {
    return {std::move(identity),
            {exosnap::engine::CaptureTarget::Kind::Window, reinterpret_cast<uintptr_t>(handle), "ExoSnap still probe"},
            [handle] { return IsWindow(handle) != FALSE; }};
}

QImage grabStill(exosnap::CaptureTargetStillService& service, const QString& identity, HWND handle) {
    QImage still;
    QEventLoop loop;
    QObject::connect(&service, &exosnap::CaptureTargetStillService::stillReady, &loop,
                     [&](const QString& source, const QImage& image) {
                         EXPECT_EQ(source, identity);
                         still = image;
                         loop.quit();
                     });
    service.setVisibleTargets({probeRequest(identity, handle)});
    service.start();
    QTimer::singleShot(kGrabTimeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
    return still;
}

} // namespace

TEST(CaptureTargetStillServiceTest, GrabsTheColourOfARealWindow) {
    ensureApplication();

    if (!wgcAvailable())
        GTEST_SKIP() << "Windows.Graphics.Capture is not available in this session";

    ColourWindow probe;
    ASSERT_NE(probe.handle(), nullptr) << "could not create the probe window";

    exosnap::diagnostics::AppLog::clear();
    exosnap::CaptureTargetStillService service;
    constexpr int kExpectedStills = 5;
    int delivered_stills = 0;
    QImage still;
    QString failed_identity;
    QEventLoop loop;
    QObject::connect(&service, &exosnap::CaptureTargetStillService::stillReady, &service,
                     [&still, &loop, &delivered_stills](const QString&, const QImage& image) {
                         still = image;
                         if (++delivered_stills == kExpectedStills)
                             loop.quit();
                     });
    QObject::connect(&service, &exosnap::CaptureTargetStillService::stillUnavailable, &service,
                     [&failed_identity, &loop](const QString& identity) {
                         failed_identity = identity;
                         loop.quit();
                     });

    service.setVisibleTargets({probeRequest(QStringLiteral("window:probe"), probe.handle())});
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
    ASSERT_EQ(delivered_stills, kExpectedStills);
    EXPECT_EQ(captureItemsCreated(), 1);
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

TEST(CaptureTargetStillServiceTest, ObservesClosureWithoutAnActiveCaptureSession) {
    ensureApplication();
    if (!wgcAvailable())
        GTEST_SKIP() << "Windows.Graphics.Capture is not available in this session";

    ColourWindow probe;
    ASSERT_NE(probe.handle(), nullptr);
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    try {
        exosnap::CaptureSourceKey key;
        key.kind = exosnap::CaptureSourceKey::Kind::Window;
        key.native_id = reinterpret_cast<uintptr_t>(probe.handle());
        exosnap::WgcCaptureItem item(key);
        EXPECT_FALSE(item.IsClosed());
        probe.close();

        QEventLoop loop;
        QTimer check;
        QObject::connect(&check, &QTimer::timeout, &loop, [&] {
            if (item.IsClosed())
                loop.quit();
        });
        check.start(10);
        QTimer::singleShot(kGrabTimeoutMs, &loop, &QEventLoop::quit);
        loop.exec();
        EXPECT_TRUE(item.IsClosed());
    } catch (const winrt::hresult_error& error) {
        ADD_FAILURE() << winrt::to_string(error.message());
    }
    winrt::uninit_apartment();
}

TEST(CaptureTargetStillServiceTest, ReplacesItemsAfterWindowDestructionAndWorkerStop) {
    ensureApplication();
    if (!wgcAvailable())
        GTEST_SKIP() << "Windows.Graphics.Capture is not available in this session";

    exosnap::diagnostics::AppLog::clear();
    exosnap::CaptureTargetStillService service;

    ColourWindow original;
    ASSERT_NE(original.handle(), nullptr);
    const auto original_still = grabStill(service, QStringLiteral("original"), original.handle());
    ASSERT_FALSE(original_still.isNull());
    ASSERT_EQ(captureItemsCreated(), 1);
    original.close();

    ColourWindow replacement(RGB(255, 32, 0));
    ASSERT_NE(replacement.handle(), nullptr);
    const auto replacement_still = grabStill(service, QStringLiteral("replacement"), replacement.handle());
    ASSERT_FALSE(replacement_still.isNull());
    const auto centre = replacement_still.pixelColor(replacement_still.width() / 2, replacement_still.height() / 2);
    EXPECT_NEAR(centre.red(), 255, 8);
    EXPECT_NEAR(centre.green(), 32, 8);
    EXPECT_NEAR(centre.blue(), 0, 8);
    EXPECT_EQ(captureItemsCreated(), 2);

    // Closing the picker removes its requests. Reopening the same target must
    // not depend on a new item, even when the worker has released its device.
    service.setVisibleTargets({});
    QEventLoop idle;
    QTimer::singleShot(exosnap::CaptureTargetStillService::kGrabIntervalMs * 2, &idle, &QEventLoop::quit);
    idle.exec();
    EXPECT_FALSE(grabStill(service, QStringLiteral("replacement"), replacement.handle()).isNull());
    EXPECT_EQ(captureItemsCreated(), 2);

    service.stop();
    EXPECT_FALSE(grabStill(service, QStringLiteral("replacement"), replacement.handle()).isNull());
    service.stop();
    EXPECT_EQ(captureItemsCreated(), 3);
}

TEST(CaptureTargetStillServiceTest, ReusesAnItemAfterItsWindowLeavesAndReturnsToTheViewport) {
    ensureApplication();
    if (!wgcAvailable())
        GTEST_SKIP() << "Windows.Graphics.Capture is not available in this session";

    ColourWindow first;
    ColourWindow second(RGB(255, 32, 0), 380);
    ASSERT_NE(first.handle(), nullptr);
    ASSERT_NE(second.handle(), nullptr);

    exosnap::diagnostics::AppLog::clear();
    exosnap::CaptureTargetStillService service;
    ASSERT_FALSE(grabStill(service, QStringLiteral("first"), first.handle()).isNull());
    ASSERT_FALSE(grabStill(service, QStringLiteral("second"), second.handle()).isNull());
    ASSERT_TRUE(SetWindowPos(first.handle(), nullptr, 0, 0, 240, 320, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE));
    const QImage resized = grabStill(service, QStringLiteral("first"), first.handle());
    ASSERT_FALSE(resized.isNull());
    EXPECT_GT(resized.height(), resized.width());
    EXPECT_EQ(captureItemsCreated(), 2);
}

TEST(CaptureTargetStillServiceTest, ReportsUnavailableWhenAWindowStaysVisibleAcrossARequestChange) {
    ensureApplication();
    if (!wgcAvailable())
        GTEST_SKIP() << "Windows.Graphics.Capture is not available in this session";

    ColourWindow probe;
    ASSERT_NE(probe.handle(), nullptr);
    std::atomic_int validations{0};
    std::binary_semaphore third_attempt{0};
    exosnap::CaptureTargetStillService service;
    const QString identity = QStringLiteral("probe");
    ASSERT_FALSE(grabStill(service, identity, probe.handle()).isNull());
    service.stop();
    ShowWindow(probe.handle(), SW_HIDE);

    auto request = probeRequest(identity, probe.handle());
    request.valid = [&] {
        // Each hidden-window attempt validates before and after skipping capture.
        // The third attempt proves that two failures finished while Qt was blocked.
        if (++validations == 5)
            third_attempt.release();
        return IsWindow(probe.handle()) != FALSE;
    };
    service.setVisibleTargets({request});
    service.start();
    ASSERT_TRUE(third_attempt.try_acquire_for(std::chrono::milliseconds(kGrabTimeoutMs)));

    QEventLoop loop;
    bool unavailable = false;
    QObject::connect(&service, &exosnap::CaptureTargetStillService::stillUnavailable, &loop,
                     [&](const QString& source) {
                         if (source == identity) {
                             unavailable = true;
                             loop.quit();
                         }
                     });
    service.setVisibleTargets({request, probeRequest(QStringLiteral("other"), nullptr)});
    QTimer::singleShot(kGrabTimeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
    EXPECT_TRUE(unavailable);
}
