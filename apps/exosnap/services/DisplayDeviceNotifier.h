#pragma once

#include <QMetaType>
#include <QObject>
#include <QRect>
#include <QTimer>
#include <QVector>

#include <functional>
#include <vector>

#include "services/DeviceDiscoveryCommon.h"

// Forward declarations to keep Qt screen types out of compile units that don't need them.
class QScreen;

namespace exosnap {

// ---------------------------------------------------------------------------
// Value object: information about one display at a point in time.
// ---------------------------------------------------------------------------
struct DisplayInfo {
    // STABLE KEY = QScreen::name() — this is the GDI device name on Windows
    // (e.g. "\\.\DISPLAY1"). Note: GDI device names can be reassigned on
    // topology change (display disconnect/reconnect or mode-set). A hardware-
    // stable identity based on EDID or the monitor device path (SetupAPI /
    // DISPLAYCONFIG_PATH_INFO) is deferred to a future revision. Do NOT use
    // this id as a long-lived persistent key across reboots or monitor changes.
    QString id;

    // Human-readable label. Uses manufacturer/model from QScreen if available,
    // otherwise falls back to QScreen::name().
    QString name;

    QRect geometry;           // virtual-desktop pixels
    QRect available_geometry; // geometry minus taskbar / docks
    qreal device_pixel_ratio = 1.0;
    qreal logical_dpi = 96.0;
    int rotation_degrees = 0; // derived from QScreen::orientation(), or 0
    bool primary = false;

    [[nodiscard]] bool operator==(const DisplayInfo& other) const noexcept;
    [[nodiscard]] bool operator!=(const DisplayInfo& other) const noexcept;
};

// ---------------------------------------------------------------------------
// Value object: snapshot of all displays at a point in time.
// No native handles. Deterministic ordering (sorted by id).
// ---------------------------------------------------------------------------
struct DisplaySnapshot {
    QVector<DisplayInfo> displays; // sorted by id

    [[nodiscard]] bool operator==(const DisplaySnapshot& other) const noexcept;
    [[nodiscard]] bool operator!=(const DisplaySnapshot& other) const noexcept;
};

// ---------------------------------------------------------------------------
// DisplayDeviceNotifier
//
// Lives on the Qt main thread. Connects to QGuiApplication screen signals
// (screenAdded, screenRemoved, primaryScreenChanged) and per-screen property
// signals (geometryChanged, orientationChanged, …) to detect display topology
// changes, and re-emits a typed, deduplicated snapshot via snapshotChanged().
// A QTimer debounces rapid bursts (e.g. multi-monitor mode-sets).
//
// Threading contract
// ------------------
//   Qt screen signal (main thread)
//     -> scheduleRefresh(reason)  [main thread, restarts debounce timer]
//     -> debounce_timer_ timeout  [main thread]
//     -> refreshNow(reason)       [main thread: enumerate, dedup, emit]
//
// Dependency injection for tests
// --------------------------------
// Construct without connecting any Qt screen signals by using the default
// constructor and then calling setEnumeratorForTest() before start().
// simulateNativeEvent() drives the exact same debounce/dedup path, so tests
// exercise the real scheduling logic without needing actual QScreen objects.
// ---------------------------------------------------------------------------
class DisplayDeviceNotifier : public QObject {
    Q_OBJECT

  public:
    using Enumerator = std::function<DisplaySnapshot()>;

    // Production constructor: uses QGuiApplication::screens().
    explicit DisplayDeviceNotifier(QObject* parent = nullptr);

    // Destructor: calls stop() to disconnect all screen signals.
    ~DisplayDeviceNotifier() override;

    DisplayDeviceNotifier(const DisplayDeviceNotifier&) = delete;
    DisplayDeviceNotifier& operator=(const DisplayDeviceNotifier&) = delete;

    // -----------------------------------------------------------------------
    // Test-support API
    // -----------------------------------------------------------------------

    // Replace the enumerator used during refresh. Call before start().
    void setEnumeratorForTest(Enumerator enumerator);

    // Set the debounce interval (default: 200 ms). Use 0 in tests.
    void setDebounceIntervalMsForTest(int ms);

    // Inject a fake native event — drives the same debounce path that a real
    // screen signal callback would follow. Safe to call before start() when in
    // test mode (no screen signal connections made).
    void simulateNativeEvent(DiscoveryReason reason);

    // Process all pending Qt events until the debounce timer fires and the
    // refresh completes. Convenience for synchronous test assertions.
    void flushPendingForTest();

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    // Connect QGuiApplication screen signals. Idempotent.
    // No-op (safe) when an injected enumerator is set (test mode).
    void start();

    // Disconnect all screen signals. Idempotent.
    // Called automatically by the destructor.
    void stop();

    // Immediate synchronous refresh: enumerate now, compare, emit if changed.
    // reason is reported as-is (typically DiscoveryReason::Rescan).
    void rescan();

    // -----------------------------------------------------------------------
    // Accessors
    // -----------------------------------------------------------------------

    [[nodiscard]] DisplaySnapshot currentSnapshot() const;

  signals:
    // Emitted when the snapshot actually changes (deduplicated).
    // Never emitted while holding any internal lock.
    void snapshotChanged(const exosnap::DisplaySnapshot& snapshot, exosnap::DiscoveryReason reason);

  private:
    // Called on the Qt main thread from screen signals or simulateNativeEvent.
    // Restarts the debounce timer and remembers the most recent reason.
    void scheduleRefresh(DiscoveryReason reason);

    // Core refresh: enumerates, sorts, compares to last_snapshot_, emits if changed.
    void refreshNow(DiscoveryReason reason);

    // Builds a snapshot using enumerator_ and sorts displays by id.
    [[nodiscard]] DisplaySnapshot buildSnapshot() const;

    // Connect property signals for a single screen (geometry, dpi, orientation).
    // Called from start() for all existing screens and from the screenAdded handler.
    void connectScreenSignals(QScreen* screen);

    // Disconnect all per-screen connections and clear screen_connections_.
    void disconnectAllScreenSignals();

    // -----------------------------------------------------------------------
    // Data members
    // -----------------------------------------------------------------------

    Enumerator enumerator_;
    DisplaySnapshot last_snapshot_;
    QTimer debounce_timer_;
    DiscoveryReason pending_reason_ = DiscoveryReason::Rescan;
    bool started_ = false;
    bool test_mode_ = false;

    // Per-screen signal connections (geometry, dpi, orientation).
    // Stored so we can disconnect them in stop() / before reconnecting.
    std::vector<QMetaObject::Connection> screen_connections_;

    // Application-level connections (screenAdded, screenRemoved, primaryScreenChanged).
    std::vector<QMetaObject::Connection> app_connections_;
};

} // namespace exosnap

Q_DECLARE_METATYPE(exosnap::DisplayInfo)
Q_DECLARE_METATYPE(exosnap::DisplaySnapshot)
