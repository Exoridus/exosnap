#pragma once

#include <QObject>

namespace exosnap::quick {

class CrashReportAdapter;
class RecoveryAdapter;

// Which of the two startup-owned blocking surfaces may be up right now.
//
// Recovery and the crash-consent prompt are both full-window modal surfaces
// about a session that already ended, and both can be asked for at times the
// other is already on screen:
//
//   - startup finds recovery candidates AND a crash sidecar;
//   - the crash prompt is up and the user triggers the standing
//     "Recover last session?" notification (a desktop toast is a separate
//     always-on-top window, so it stays clickable behind a modal scrim);
//   - recovery is up and the deferred crash prompt comes due.
//
// Their QML loaders are independent and z-ordered, which decides what is drawn
// on top but not who owns the keyboard: two active modal loaders means the
// hidden one still holds focusable controls. So the decision is made here,
// once, instead of by each caller checking the other surface's `visible`.
//
// The contract:
//   - at most one of the two is raised at any time;
//   - the request that loses is QUEUED, not dropped — a crash prompt or a
//     recovery offer that silently disappears is information the user cannot
//     get back without restarting;
//   - a queued request is single-shot: it is raised when the other surface
//     comes down, and never again after that.
//
// The crash surface is emitted as a request rather than raised directly: only
// the composition root can build the previous session's context.
class BlockingSurfaceArbiter : public QObject {
    Q_OBJECT

  public:
    explicit BlockingSurfaceArbiter(QObject* parent = nullptr);

    // Both must outlive this object. Without them every request is a no-op,
    // which keeps a frontend without recovery/crash wiring inert.
    void setSurfaces(RecoveryAdapter* recovery, CrashReportAdapter* crash);

    // The recovery surface wants to be up: the startup scan found candidates,
    // or the standing notification's action asked for it again.
    void requestRecovery();
    // The crash-consent prompt wants to be up. Answered by crashSurfaceRequested()
    // when it is this surface's turn.
    void requestCrash();

    [[nodiscard]] bool recoveryQueued() const noexcept;
    [[nodiscard]] bool crashQueued() const noexcept;

  signals:
    // Raise the crash surface now — build the context and call
    // CrashReportAdapter::present().
    void crashSurfaceRequested();

  private:
    void onSurfaceStateChanged();
    [[nodiscard]] bool recoveryUp() const;
    [[nodiscard]] bool crashUp() const;

    RecoveryAdapter* recovery_ = nullptr;
    CrashReportAdapter* crash_ = nullptr;
    bool recovery_queued_ = false;
    bool crash_queued_ = false;
    // Guards the re-entry a raise causes: raising one surface emits the very
    // signal this class listens to.
    bool dispatching_ = false;
};

} // namespace exosnap::quick
