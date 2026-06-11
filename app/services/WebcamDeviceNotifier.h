#pragma once

#include <QMetaType>
#include <QObject>
#include <QTimer>
#include <QVector>

#include <functional>

#include "services/DeviceDiscoveryCommon.h"
#include "services/WebcamService.h"

namespace exosnap {

// ---------------------------------------------------------------------------
// Value object: snapshot of all webcam devices at a point in time.
// No native handles. Deterministic ordering (sorted by id).
// ---------------------------------------------------------------------------
struct WebcamDeviceSnapshot {
    QVector<WebcamDeviceInfo> devices; // sorted by id

    [[nodiscard]] bool operator==(const WebcamDeviceSnapshot& other) const noexcept;
    [[nodiscard]] bool operator!=(const WebcamDeviceSnapshot& other) const noexcept;
};

// ---------------------------------------------------------------------------
// WebcamDeviceNotifier
//
// Lives on the Qt main thread. Wraps a WM_DEVICECHANGE message-only window
// with RegisterDeviceNotification (KSCATEGORY_VIDEO) to detect webcam plug /
// unplug, and re-emits a typed, deduplicated snapshot via snapshotChanged().
// A QTimer debounces rapid bursts from the OS.
//
// Threading contract
// ------------------
//   WNDPROC receives WM_DEVICECHANGE (main thread, message-only window)
//     -> QMetaObject::invokeMethod(Qt::QueuedConnection)
//     -> scheduleRefreshFromWndProc(reason)  [main thread, enters debounce]
//     -> debounce_timer_ timeout  [main thread]
//     -> refreshNow(reason)       [main thread: enumerate, dedup, emit]
//
// Dependency injection for tests
// --------------------------------
// Construct without creating any Win32 objects by using the default constructor
// and then calling setEnumeratorForTest() before start(). simulateNativeEvent()
// drives the exact same marshaled path a real WNDPROC callback would follow, so
// tests exercise the real scheduling/debounce/dedup logic.
// ---------------------------------------------------------------------------
class WebcamDeviceNotifier : public QObject {
    Q_OBJECT

  public:
    using Enumerator = std::function<WebcamDeviceSnapshot()>;

    // Production constructor: uses the real webcam enumerator.
    explicit WebcamDeviceNotifier(QObject* parent = nullptr);

    // Destructor: calls stop() to destroy the message window and unregister the notification.
    ~WebcamDeviceNotifier() override;

    WebcamDeviceNotifier(const WebcamDeviceNotifier&) = delete;
    WebcamDeviceNotifier& operator=(const WebcamDeviceNotifier&) = delete;

    // -----------------------------------------------------------------------
    // Test-support API
    // -----------------------------------------------------------------------

    // Replace the enumerator used during refresh. Call before start().
    void setEnumeratorForTest(Enumerator enumerator);

    // Set the debounce interval (default: 200 ms). Use 0 in tests.
    void setDebounceIntervalMsForTest(int ms);

    // Inject a fake native event — drives the same marshaled path that a real
    // WNDPROC WM_DEVICECHANGE callback would follow. Safe to call before start()
    // when in test mode (no Win32 objects created).
    void simulateNativeEvent(DiscoveryReason reason);

    // Process all pending Qt events until the debounce timer fires and the
    // refresh completes. Convenience for synchronous test assertions.
    void flushPendingForTest();

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    // Create message-only window and register device notification. Idempotent.
    // No-op (safe) when an injected enumerator is set (test mode).
    void start();

    // Destroy message-only window and unregister device notification. Idempotent.
    // Called automatically by the destructor.
    void stop();

    // Immediate synchronous refresh: enumerate now, compare, emit if changed.
    // reason is reported as-is (typically DiscoveryReason::Rescan).
    void rescan();

    // -----------------------------------------------------------------------
    // Accessors
    // -----------------------------------------------------------------------

    [[nodiscard]] WebcamDeviceSnapshot currentSnapshot() const;

    // -----------------------------------------------------------------------
    // Internal bridge — NOT part of the general public API.
    // Called by the file-local WNDPROC (WebcamNotifierWndProc in the .cpp)
    // via QueuedConnection to route a WM_DEVICECHANGE event into the debounce
    // path without exposing Win32 types (HWND/UINT/WPARAM/LPARAM) in this
    // header. Do not call directly.
    // -----------------------------------------------------------------------
    void scheduleRefreshFromWndProc(DiscoveryReason reason);

  signals:
    // Emitted when the snapshot actually changes (deduplicated).
    // Never emitted while holding any internal lock.
    void snapshotChanged(const exosnap::WebcamDeviceSnapshot& snapshot, exosnap::DiscoveryReason reason);

  private:
    // Called on the Qt main thread. Restarts the debounce timer and
    // remembers the most recent reason.
    void scheduleRefresh(DiscoveryReason reason);

    // Core refresh: enumerates, sorts, compares to last_snapshot_, emits if changed.
    void refreshNow(DiscoveryReason reason);

    // Builds a snapshot using enumerator_ and sorts devices by id.
    [[nodiscard]] WebcamDeviceSnapshot buildSnapshot() const;

    // -----------------------------------------------------------------------
    // Data members
    // -----------------------------------------------------------------------

    Enumerator enumerator_;
    WebcamDeviceSnapshot last_snapshot_;
    QTimer debounce_timer_;
    DiscoveryReason pending_reason_ = DiscoveryReason::Rescan;
    bool started_ = false;
    bool test_mode_ = false;

    // Win32 message-only window and device notification handle.
    // nullptr when in test mode or before start().
    void* hwnd_ = nullptr;       // HWND (void* avoids Win32 in header)
    void* hdevnotify_ = nullptr; // HDEVNOTIFY
};

} // namespace exosnap

Q_DECLARE_METATYPE(exosnap::WebcamDeviceSnapshot)
