#pragma once

#include "services/DeviceDiscoveryCommon.h"
#include "services/DisplayDeviceNotifier.h"

#include <QObject>
#include <QTimer>

#include <exosnap/engine/recorder_session.h>

#include <functional>
#include <vector>

#include <windows.h>

namespace exosnap {

struct CaptureTargetSnapshot {
    std::vector<exosnap::engine::CaptureTarget> targets;

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

    explicit CaptureTargetNotifier(QObject* parent = nullptr);
    ~CaptureTargetNotifier() override;

    CaptureTargetNotifier(const CaptureTargetNotifier&) = delete;
    CaptureTargetNotifier& operator=(const CaptureTargetNotifier&) = delete;

    void setEnumerator(Enumerator enumerator);
    void setEnumeratorForTest(Enumerator enumerator);
    void setDebounceIntervalMsForTest(int milliseconds);
    void simulateNativeEvent(DiscoveryReason reason);
    void flushPendingForTest();

    void start();
    void stop();
    void rescan();

    [[nodiscard]] const CaptureTargetSnapshot& currentSnapshot() const noexcept;

    // The display-topology notifier this class already owns and already listens to.
    // Exposed so a second consumer can observe a display-configuration change
    // without installing a second set of QGuiApplication screen connections — the
    // target list and the per-display DXGI facts both go stale on the same event.
    [[nodiscard]] DisplayDeviceNotifier& displayNotifier() noexcept {
        return display_notifier_;
    }

  signals:
    void snapshotChanged(const exosnap::CaptureTargetSnapshot& snapshot, exosnap::DiscoveryReason reason);

  private:
    static void CALLBACK WinEventCallback(HWINEVENTHOOK hook, DWORD event, HWND window, LONG object_id, LONG child_id,
                                          DWORD event_thread, DWORD event_time);
    void scheduleRefresh(DiscoveryReason reason);
    void refreshNow(DiscoveryReason reason);
    void installWindowHook();
    void removeWindowHook();

    Enumerator enumerator_;
    CaptureTargetSnapshot last_snapshot_;
    DisplayDeviceNotifier display_notifier_;
    QTimer debounce_timer_;
    DiscoveryReason pending_reason_ = DiscoveryReason::Rescan;
    HWINEVENTHOOK window_hook_ = nullptr;
    bool started_ = false;
    bool test_mode_ = false;
};

} // namespace exosnap

Q_DECLARE_METATYPE(exosnap::CaptureTargetSnapshot)
