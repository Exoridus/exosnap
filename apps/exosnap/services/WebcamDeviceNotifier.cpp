// clang-format off
#include <windows.h>
#include <dbt.h>
#include <ks.h>
#include <ksmedia.h>
// clang-format on

#include "services/WebcamDeviceNotifier.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>

#include <algorithm>

#include "diagnostics/AppLog.h"

namespace exosnap {

// ---------------------------------------------------------------------------
// KSCATEGORY_VIDEO GUID — identifies video capture devices.
// {6994AD05-93EF-11D0-A3CC-00A0C9223196}
// Using this class GUID as the DEV_BROADCAST_DEVICEINTERFACE filter reduces
// WM_DEVICECHANGE noise: only video-capture device topology changes arrive.
// ---------------------------------------------------------------------------
static const GUID kKsCategoryVideo = {0x6994AD05, 0x93EF, 0x11D0, {0xA3, 0xCC, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96}};

// Name used to identify our message-only window class.
static constexpr wchar_t kWindowClassName[] = L"ExoSnapWebcamNotifier";

// ---------------------------------------------------------------------------
// WebcamDeviceSnapshot
// ---------------------------------------------------------------------------

bool WebcamDeviceSnapshot::operator==(const WebcamDeviceSnapshot& other) const noexcept {
    if (devices.size() != other.devices.size()) {
        return false;
    }
    for (int i = 0; i < devices.size(); ++i) {
        if (devices[i].id != other.devices[i].id || devices[i].name != other.devices[i].name) {
            return false;
        }
    }
    return true;
}

bool WebcamDeviceSnapshot::operator!=(const WebcamDeviceSnapshot& other) const noexcept {
    return !(*this == other);
}

// ---------------------------------------------------------------------------
// Default enumerator (production)
// ---------------------------------------------------------------------------

static WebcamDeviceSnapshot DefaultEnumerator() {
    WebcamDeviceSnapshot snap;
    const auto vec = WebcamService::EnumerateDevices();
    snap.devices = QVector<WebcamDeviceInfo>(vec.begin(), vec.end());
    return snap;
}

// ---------------------------------------------------------------------------
// WndProc (file-local) — receives WM_DEVICECHANGE on the Qt main thread
// (message-only windows are pumped by the Qt main event loop).
// We use QMetaObject::invokeMethod with QueuedConnection to decouple from
// WNDPROC reentrancy and to share the same debounce path that
// simulateNativeEvent uses.
// ---------------------------------------------------------------------------

// clang-format off
static LRESULT CALLBACK WebcamNotifierWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    // clang-format on
    if (msg == WM_DEVICECHANGE) {
        if (wparam == DBT_DEVICEARRIVAL || wparam == DBT_DEVICEREMOVECOMPLETE) {
            const DiscoveryReason reason =
                (wparam == DBT_DEVICEARRIVAL) ? DiscoveryReason::DeviceAdded : DiscoveryReason::DeviceRemoved;
            // Retrieve the notifier pointer stored in GWLP_USERDATA.
            auto* notifier = reinterpret_cast<WebcamDeviceNotifier*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (notifier) {
                // QPointer guard: safe even if the notifier is somehow destroyed between
                // the WndProc call and the queued lambda execution.
                QPointer<WebcamDeviceNotifier> guard(notifier);
                QMetaObject::invokeMethod(
                    notifier,
                    [guard, reason]() {
                        if (guard) {
                            guard->scheduleRefreshFromWndProc(reason);
                        }
                    },
                    Qt::QueuedConnection);
            }
        }
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// ---------------------------------------------------------------------------
// WebcamDeviceNotifier — construction / destruction
// ---------------------------------------------------------------------------

WebcamDeviceNotifier::WebcamDeviceNotifier(QObject* parent) : QObject(parent), enumerator_(&DefaultEnumerator) {
    debounce_timer_.setSingleShot(true);
    debounce_timer_.setInterval(200);
    connect(&debounce_timer_, &QTimer::timeout, this, [this]() { refreshNow(pending_reason_); });
}

WebcamDeviceNotifier::~WebcamDeviceNotifier() {
    stop();
}

// ---------------------------------------------------------------------------
// Test-support
// ---------------------------------------------------------------------------

void WebcamDeviceNotifier::setEnumeratorForTest(Enumerator enumerator) {
    enumerator_ = std::move(enumerator);
    test_mode_ = true;
}

void WebcamDeviceNotifier::setDebounceIntervalMsForTest(int ms) {
    debounce_timer_.setInterval(ms);
}

void WebcamDeviceNotifier::simulateNativeEvent(DiscoveryReason reason) {
    // Drive the same marshaled path a real WNDPROC callback would follow.
    QPointer<WebcamDeviceNotifier> guard(this);
    QMetaObject::invokeMethod(
        this,
        [guard, reason]() {
            if (guard) {
                guard->scheduleRefresh(reason);
            }
        },
        Qt::QueuedConnection);
}

void WebcamDeviceNotifier::flushPendingForTest() {
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    if (debounce_timer_.isActive()) {
        debounce_timer_.stop();
        refreshNow(pending_reason_);
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void WebcamDeviceNotifier::start() {
    if (started_) {
        return;
    }

    started_ = true;

    if (test_mode_) {
        // Test mode: no Win32 objects, just mark as started.
        return;
    }

    // Register a window class for the message-only window (idempotent via CS_NOCLOSE).
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WebcamNotifierWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWindowClassName;
    // RegisterClassExW returns 0 if the class already exists (ERROR_CLASS_ALREADY_EXISTS);
    // we ignore that error since the existing class is functionally identical.
    RegisterClassExW(&wc);

    // Create a message-only window (HWND_MESSAGE parent means it receives no
    // paint / layout messages, only device-notification messages).
    HWND hwnd = CreateWindowExW(0, kWindowClassName, L"ExoSnapWebcamNotifier", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        diagnostics::AppLog::warning(QStringLiteral("WebcamDiscovery"),
                                     QStringLiteral("CreateWindowExW failed — no webcam notifications"));
        return;
    }
    hwnd_ = hwnd;

    // Store this pointer in GWLP_USERDATA so WndProc can retrieve it.
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    // Register for video-capture device interface arrivals/removals.
    DEV_BROADCAST_DEVICEINTERFACE_W filter = {};
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = kKsCategoryVideo;

    HDEVNOTIFY hdevnotify = RegisterDeviceNotificationW(hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
    if (!hdevnotify) {
        diagnostics::AppLog::warning(
            QStringLiteral("WebcamDiscovery"),
            QStringLiteral("RegisterDeviceNotificationW failed — webcam plug/unplug may be missed"));
        // Window still exists; clean up GWLP_USERDATA so stray WM_DEVICECHANGE is harmless.
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        DestroyWindow(hwnd);
        hwnd_ = nullptr;
        return;
    }
    hdevnotify_ = hdevnotify;
}

void WebcamDeviceNotifier::stop() {
    if (!started_) {
        return;
    }
    started_ = false;
    debounce_timer_.stop();

    if (hdevnotify_) {
        UnregisterDeviceNotification(static_cast<HDEVNOTIFY>(hdevnotify_));
        hdevnotify_ = nullptr;
    }
    if (hwnd_) {
        // Clear GWLP_USERDATA first so any stray message pump call is harmless.
        SetWindowLongPtrW(static_cast<HWND>(hwnd_), GWLP_USERDATA, 0);
        DestroyWindow(static_cast<HWND>(hwnd_));
        hwnd_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Immediate synchronous refresh
// ---------------------------------------------------------------------------

void WebcamDeviceNotifier::rescan() {
    refreshNow(DiscoveryReason::Rescan);
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

WebcamDeviceSnapshot WebcamDeviceNotifier::currentSnapshot() const {
    return last_snapshot_;
}

// ---------------------------------------------------------------------------
// Internal
// ---------------------------------------------------------------------------

void WebcamDeviceNotifier::scheduleRefreshFromWndProc(DiscoveryReason reason) {
    scheduleRefresh(reason);
}

void WebcamDeviceNotifier::scheduleRefresh(DiscoveryReason reason) {
    // DeviceRemoved takes priority over other reasons while debouncing.
    if (!debounce_timer_.isActive() || reason == DiscoveryReason::DeviceRemoved) {
        pending_reason_ = reason;
    }
    debounce_timer_.start();
}

void WebcamDeviceNotifier::refreshNow(DiscoveryReason reason) {
    const WebcamDeviceSnapshot fresh = buildSnapshot();
    if (fresh == last_snapshot_) {
        return;
    }
    last_snapshot_ = fresh;

    diagnostics::AppLog::info(QStringLiteral("WebcamDiscovery"),
                              QStringLiteral("Snapshot changed — devices:%1 reason:%2")
                                  .arg(fresh.devices.size())
                                  .arg(QLatin1StringView(DiscoveryReasonName(reason))));

    emit snapshotChanged(last_snapshot_, reason);
}

WebcamDeviceSnapshot WebcamDeviceNotifier::buildSnapshot() const {
    WebcamDeviceSnapshot snap = enumerator_();

    std::sort(snap.devices.begin(), snap.devices.end(),
              [](const WebcamDeviceInfo& a, const WebcamDeviceInfo& b) { return a.id < b.id; });

    return snap;
}

} // namespace exosnap
