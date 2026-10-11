#pragma once

#include "services/DeviceDiscoveryCommon.h"
#include "services/DisplayDeviceNotifier.h"

#include <QObject>
#include <QString>
#include <QTimer>

#include <exosnap/engine/recorder_session.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include <windows.h>

namespace exosnap {

struct WindowCaptureIdentity {
    uintptr_t native_id = 0;
    uint32_t process_id = 0;
    uint64_t process_creation_time = 0;
    uint64_t window_generation = 0;
    QString application_identity;

    [[nodiscard]] QString stableIdentity() const;
    [[nodiscard]] bool operator==(const WindowCaptureIdentity&) const = default;
};
struct CaptureTargetSnapshot {
    std::vector<exosnap::engine::CaptureTarget> targets;
    std::vector<WindowCaptureIdentity> window_identities;

    [[nodiscard]] bool operator==(const CaptureTargetSnapshot& other) const noexcept;
    [[nodiscard]] bool operator!=(const CaptureTargetSnapshot& other) const noexcept;
};

// Main-thread, event-driven discovery for the complete Record capture-target
// list. Display changes come from Qt's screen topology signals; top-level window
// availability comes from WinEvent show/hide/create/destroy notifications. The
// burst is debounced before the shared engine enumerator is called.
class CaptureTargetNotifier final : public QObject {
    Q_OBJECT

  public:
    using Enumerator = std::function<CaptureTargetSnapshot()>;
    using WindowIdentityObserver = std::function<std::optional<WindowCaptureIdentity>(const engine::CaptureTarget&)>;

    explicit CaptureTargetNotifier(QObject* parent = nullptr);
    ~CaptureTargetNotifier() override;

    CaptureTargetNotifier(const CaptureTargetNotifier&) = delete;
    CaptureTargetNotifier& operator=(const CaptureTargetNotifier&) = delete;

    void setEnumerator(Enumerator enumerator);
    void setEnumeratorForTest(Enumerator enumerator);
    void setWindowIdentityObserverForTest(WindowIdentityObserver observer);
    void simulateWindowLifecycleForTest(uintptr_t native_id, bool destroyed);
    void setDebounceIntervalMsForTest(int milliseconds);
    void simulateNativeEvent(DiscoveryReason reason);
    void flushPendingForTest();
    // Requests a refresh the way a window lifecycle event does, WITHOUT flushing:
    // the deferral cap can only be observed by letting the debounce actually run.
    void requestRefreshForTest(DiscoveryReason reason);
    void setMaxDeferralMsForTest(qint64 milliseconds);

    void start();
    void stop();
    void rescan();

    [[nodiscard]] const CaptureTargetSnapshot& currentSnapshot() const noexcept;
    [[nodiscard]] bool isCurrent(const WindowCaptureIdentity& identity) const;
    // Worker-safe validation owns copied facts and a cancellation token, never this notifier.
    [[nodiscard]] std::function<bool()> windowIdentityValidator(WindowCaptureIdentity identity) const;

    // The display-topology notifier this class already owns and already listens to.
    // Exposed so a second consumer can observe a display-configuration change
    // without installing a second set of QGuiApplication screen connections — the
    // target list and the per-display DXGI facts both go stale on the same event.
    [[nodiscard]] DisplayDeviceNotifier& displayNotifier() noexcept {
        return display_notifier_;
    }

  signals:
    void snapshotChanged(const exosnap::CaptureTargetSnapshot& snapshot, exosnap::DiscoveryReason reason);
    void windowLifetimeInvalidated(qulonglong native_id);

  private:
    static void CALLBACK WinEventCallback(HWINEVENTHOOK hook, DWORD event, HWND window, LONG object_id, LONG child_id,
                                          DWORD event_thread, DWORD event_time);
    void noteWindowLifecycle(uintptr_t native_id, bool destroyed);
    std::optional<WindowCaptureIdentity> observeWindowIdentity(const engine::CaptureTarget& target);
    void scheduleRefresh(DiscoveryReason reason);
    void refreshNow(DiscoveryReason reason);
    void installWindowHook();
    void removeWindowHook();

    Enumerator enumerator_;
    WindowIdentityObserver window_identity_observer_;
    QString window_witness_property_;
    bool custom_window_identity_observer_ = false;
    std::unordered_map<uintptr_t, uint64_t> window_generations_;
    std::unordered_map<uintptr_t, uint64_t> native_witnesses_;
    uint64_t next_window_generation_ = 0;
    CaptureTargetSnapshot last_snapshot_;
    DisplayDeviceNotifier display_notifier_;
    // Longest the debounce may postpone a refresh. Five debounce intervals: long
    // enough that a burst still collapses into one enumeration, short enough that
    // a window the user just opened appears in the source list while they are
    // still looking for it.
    static constexpr qint64 kMaxRefreshDeferralMs = 1000;
    qint64 max_deferral_ms_ = kMaxRefreshDeferralMs;

    QTimer debounce_timer_;
    // When the currently pending refresh was first requested, for the cap above.
    qint64 first_pending_at_ms_ = 0;
    DiscoveryReason pending_reason_ = DiscoveryReason::Rescan;
    HWINEVENTHOOK window_hook_ = nullptr;
    std::shared_ptr<std::atomic<bool>> lifetime_active_ = std::make_shared<std::atomic<bool>>(false);
    bool started_ = false;
    bool test_mode_ = false;
};

} // namespace exosnap

Q_DECLARE_METATYPE(exosnap::CaptureTargetSnapshot)
