#pragma once

#include <QMetaType>
#include <QObject>
#include <QTimer>
#include <QVector>

#include <functional>
#include <string>

#include <recorder_core/audio_input_device.h>

#include "services/DeviceDiscoveryCommon.h"

namespace exosnap {

// ---------------------------------------------------------------------------
// Value object: snapshot of all audio endpoints at a point in time.
// No COM/native handles. Deterministic ordering (sorted by device_id).
// ---------------------------------------------------------------------------
struct AudioDeviceSnapshot {
    QVector<recorder_core::AudioInputDeviceInfo> inputs;  // eCapture, sorted by device_id
    QVector<recorder_core::AudioInputDeviceInfo> outputs; // eRender,  sorted by device_id
    std::string default_input_id;
    std::string default_output_id;

    [[nodiscard]] bool operator==(const AudioDeviceSnapshot& other) const noexcept;
    [[nodiscard]] bool operator!=(const AudioDeviceSnapshot& other) const noexcept;
};

// ---------------------------------------------------------------------------
// AudioDeviceNotifier
//
// Lives on the Qt main thread. Wraps IMMNotificationClient to detect Windows
// audio-endpoint changes and re-emits a typed, deduplicated snapshot via
// snapshotChanged(). A QTimer debounces rapid bursts from the OS.
//
// Threading contract
// ------------------
//   native COM callback (MTA thread)
//     -> QMetaObject::invokeMethod(Qt::QueuedConnection)
//     -> scheduleRefresh(reason)  [main thread, restarts debounce timer]
//     -> debounce_timer_ timeout  [main thread]
//     -> refreshNow(reason)       [main thread: enumerate, dedup, emit]
//
// Dependency injection for tests
// --------------------------------
// Construct without touching COM by using the default constructor and then
// calling setEnumeratorForTest() before start().  simulateNativeEvent() drives
// the exact same marshaled path a real IMMNotificationClient callback would
// follow, so tests exercise the real scheduling/debounce/dedup logic.
// ---------------------------------------------------------------------------
class AudioDeviceNotifier : public QObject {
    Q_OBJECT

  public:
    using Enumerator = std::function<AudioDeviceSnapshot()>;

    // Production constructor: uses the real WASAPI enumerator.
    explicit AudioDeviceNotifier(QObject* parent = nullptr);

    // Destructor: calls stop() to unregister callbacks and release COM objects.
    ~AudioDeviceNotifier() override;

    AudioDeviceNotifier(const AudioDeviceNotifier&) = delete;
    AudioDeviceNotifier& operator=(const AudioDeviceNotifier&) = delete;

    // -----------------------------------------------------------------------
    // Test-support API
    // -----------------------------------------------------------------------

    // Replace the enumerator used during refresh. Call before start().
    // In test mode (no COM registration), also used by production path if set.
    void setEnumeratorForTest(Enumerator enumerator);

    // Set the debounce interval (default: 200 ms). Use 0 in tests.
    void setDebounceIntervalMsForTest(int ms);

    // Inject a fake native event — drives the same marshaled path that a real
    // IMMNotificationClient callback would follow. Safe to call before start()
    // when in test mode (no COM registration performed).
    void simulateNativeEvent(DiscoveryReason reason);

    // Process all pending Qt events until the debounce timer fires and the
    // refresh completes. Convenience for synchronous test assertions.
    void flushPendingForTest();

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    // Register the IMMNotificationClient with WASAPI. Idempotent.
    // No-op (safe) when an injected enumerator is set (test mode).
    void start();

    // Unregister the IMMNotificationClient and release COM objects. Idempotent.
    // Called automatically by the destructor.
    void stop();

    // Immediate synchronous refresh: enumerate now, compare, emit if changed.
    // Uses the same compare/emit path as the debounced refresh.
    // reason is reported as-is (typically DiscoveryReason::Rescan).
    void rescan();

    // -----------------------------------------------------------------------
    // Accessors
    // -----------------------------------------------------------------------

    [[nodiscard]] AudioDeviceSnapshot currentSnapshot() const;

  signals:
    // Emitted when the snapshot actually changes (deduplicated).
    // Never emitted while holding any internal lock.
    void snapshotChanged(const exosnap::AudioDeviceSnapshot& snapshot, exosnap::DiscoveryReason reason);

  private:
    // Called on the Qt main thread (via QueuedConnection from native callback
    // or directly from simulateNativeEvent). Restarts the debounce timer and
    // remembers the most recent reason. DeviceRemoved takes priority over all
    // other reasons so that the UI knows something disappeared.
    void scheduleRefresh(DiscoveryReason reason);

    // Core refresh: enumerates, sorts, compares to last_snapshot_, emits if
    // changed. Shared by both the debounce-timer path and rescan().
    void refreshNow(DiscoveryReason reason);

    // Builds a snapshot using enumerator_ and sorts both vectors by device_id.
    [[nodiscard]] AudioDeviceSnapshot buildSnapshot() const;

    // -----------------------------------------------------------------------
    // Data members
    // -----------------------------------------------------------------------

    Enumerator enumerator_;                                    // injected or default (real WASAPI)
    AudioDeviceSnapshot last_snapshot_;                        // last published snapshot (for dedup)
    QTimer debounce_timer_;                                    // single-shot; parent = this
    DiscoveryReason pending_reason_ = DiscoveryReason::Rescan; // most recent reason in flight
    bool started_ = false;                                     // true after start() registered COM callbacks
    bool test_mode_ = false;                                   // true when enumerator was replaced by test code

    // Opaque pointer to the IMMDeviceEnumerator + notification client COM objects.
    // Defined only in the .cpp to keep COM headers out of this public header.
    struct ComState;
    std::unique_ptr<ComState> com_state_;

    // AudioEndpointNotificationClient needs to call scheduleRefresh().
    friend class AudioEndpointNotificationClient;
};

} // namespace exosnap

Q_DECLARE_METATYPE(exosnap::AudioDeviceSnapshot)
Q_DECLARE_METATYPE(exosnap::DiscoveryReason)
