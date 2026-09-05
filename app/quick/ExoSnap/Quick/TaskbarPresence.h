#pragma once

// The Windows taskbar button as a product surface: the thumbnail transport
// buttons and the progress bar.
//
// NOT the button's icon. That is the window's icon, which the chrome owns and
// which carries the session's state as the same rendered mark the notification
// area shows. An overlay badge used to carry it instead, and the two surfaces
// then said different things about one session: a coral aperture in the tray,
// and a mint aperture with a coral dot stuck in its corner on the taskbar. The
// dot also read as an unread-notification badge, which is what that corner means
// everywhere else in Windows.
//
// WHY THE LIFECYCLE IS THE HARD PART
// ----------------------------------
// ITaskbarList3 is not usable the moment a window has an HWND. Explorer creates
// the taskbar button asynchronously and announces it with a registered window
// message ("TaskbarButtonCreated"); anything sent before that is accepted by COM
// and dropped on the floor, which looks exactly like a silent bug. So readiness
// is a state this class tracks per HWND, and the desired product state is held
// until the shell says it can receive it.
//
// Display affinity has the same shape and the chrome already learned it: a
// recreated window comes back without any of it. A handle whose identity changed
// is therefore NOT ready, whatever the old one had applied.
//
// FAIL-OPEN, ALWAYS
// -----------------
// Nothing here is allowed to affect a recording. Every call reports an HRESULT
// and every refusal is logged once, but a taskbar that refuses to cooperate
// costs the user a button, never a capture.
//
// This class owns no message pump and installs no event filter: QuickWindowChrome
// already owns the shell HWND and a process-wide native event filter, and a
// second one would be a second thing to keep in sync with the window's identity.

#include <QObject>
#include <QVector>
#include <QtQmlIntegration/qqmlintegration.h>

#include "models/ShellPresence.h"
#include "models/TaskbarProgressLease.h"

#include <memory>

namespace exosnap::quick {

// THBN_CLICKED: the notification code a thumbnail button click carries in the
// high word of a WM_COMMAND wParam. Spelled out so the message filter, this class
// and its tests share one value without any of them including shobjidl_core.h.
inline constexpr int kThumbButtonClickedNotification = 0x1800;

// One registered thumbnail button. `action` is the button's meaning and survives
// being greyed -- it is what the icon and the tooltip are drawn from.
// Resource id of the thumbnail glyph for an action, per system appearance.
//
// Pure and declared here so a test can hold it to both halves: every action needs
// an entry in BOTH sets, and the two must differ -- a light-chrome entry that
// falls back to the dark glyph is the defect this pairing exists to prevent (the
// amber pause mark on light grey). `light_chrome` refers to the SYSTEM appearance,
// because the strip is drawn by the taskbar and not by this application.
[[nodiscard]] int ThumbIconResourceFor(ShellAction action, bool light_chrome) noexcept;

struct ThumbButtonSpec {
    int command_id = 0;
    ShellAction action = ShellAction::None;
    bool visible = false;
    bool enabled = false;
};

// The platform calls, isolated so the lifecycle above them is testable with no
// Explorer, no HWND and no message pump. Every method returns an HRESULT-shaped
// value: `desiredState = Recording` is not evidence that the shell accepted it.
class TaskbarShell {
  public:
    virtual ~TaskbarShell();

    virtual qint32 initialize() = 0;
    virtual qint32 addButtons(void* hwnd, const QVector<ThumbButtonSpec>& buttons) = 0;
    virtual qint32 updateButtons(void* hwnd, const QVector<ThumbButtonSpec>& buttons) = 0;
    virtual qint32 setProgressState(void* hwnd, TaskbarProgressState state) = 0;
    virtual qint32 setProgressValue(void* hwnd, quint64 completed, quint64 total) = 0;
};

// Builds the platform-independent default (COM on Windows, a no-op elsewhere).
[[nodiscard]] std::unique_ptr<TaskbarShell> MakePlatformTaskbarShell();

class TaskbarPresence : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("TaskbarPresence is provided by the application")

  public:
    explicit TaskbarPresence(QObject* parent = nullptr);
    ~TaskbarPresence() override;

    // Replaces the platform calls. An instance seam rather than a global so two
    // tests can run concurrently.
    void setShellForTest(std::unique_ptr<TaskbarShell> shell);

    // Re-pushes the thumbnail buttons with icons resolved for the CURRENT system
    // appearance. Connected to the system colour-scheme change; a no-op before the
    // buttons are registered.
    void refreshThumbIcons();

    // The shell window's native handle, as the chrome reports it. A handle whose
    // identity changed drops readiness and the applied state with it; the same
    // handle again does nothing.
    void setHandle(void* hwnd);
    [[nodiscard]] void* handle() const noexcept;

    // The registered TaskbarButtonCreated message arrived for `hwnd`. Ignored
    // for any other handle -- the filter it comes from is process-wide.
    //
    // Every announcement is honoured, not only the first. Explorer broadcasts it
    // again after it restarts, and the button it announces then is a NEW button
    // on the same HWND with none of this window's registrations and a dead COM
    // proxy behind the old interface. Ignoring the repeat would cost the user
    // the thumbnail transport for the rest of the session with nothing to show
    // for it. Re-arming never duplicates the set: it is registered once per
    // announcement, always as the same three slots.
    void notifyShellReady(void* hwnd);
    [[nodiscard]] bool ready() const noexcept;

    // The desired product state. Stored whether or not the shell can receive it
    // yet, and re-applied in full the moment it can.
    //
    // Only the thumbnail strip and the progress bar are this class's business.
    // What the button ICON shows is the window's icon, which the chrome owns --
    // the same rendered mark the notification area draws, so the two surfaces
    // cannot say different things about one session.
    void setPresence(const ShellPresenceState& state);

    // A WM_COMMAND wParam. Returns whether it was one of our thumbnail buttons,
    // which is what tells the filter to consume it. A recognised button whose
    // action the current state refuses is still ours -- it is consumed and
    // nothing happens, rather than falling through to DefWindowProc.
    bool handleCommand(quint64 wparam);

    // ---- taskbar progress -----------------------------------------------
    // The lease API of models/TaskbarProgressLease.h, with each accepted change
    // published to the shell. A refused call publishes nothing, which is what
    // keeps a stale producer off the current owner's bar.
    [[nodiscard]] TaskbarProgressLease acquireProgress(TaskbarProgressOwner owner);
    void updateProgress(const TaskbarProgressLease& lease, double fraction);
    void setProgressIndeterminate(const TaskbarProgressLease& lease);
    void finishProgress(const TaskbarProgressLease& lease);
    void failProgress(const TaskbarProgressLease& lease);
    void cancelProgress(const TaskbarProgressLease& lease);
    void releaseProgress(const TaskbarProgressLease& lease);
    [[nodiscard]] const TaskbarProgressLedger& progress() const noexcept;

    // Whether the fixed button set has been registered on the current handle.
    // ThumbBarAddButtons is once per window and cannot add a button later, which
    // is why the full set goes up front and only visibility changes afterwards.
    [[nodiscard]] bool buttonsRegistered() const noexcept;
    // Whether the last initialize() succeeded. False also means "not attempted".
    [[nodiscard]] bool shellAvailable() const noexcept;

    // The button set as it would be sent for `state`. Public because the strip
    // Explorer shows and the strip a click is resolved against must provably be
    // the same table.
    [[nodiscard]] static QVector<ThumbButtonSpec> ButtonsFor(const ShellPresenceState& state);

  signals:
    // A thumbnail button asked for a product action. Never emitted for an action
    // the current state refuses.
    void actionRequested(ShellAction action);

  private:
    // Re-creates the shell interface and pushes the full desired state at it.
    // Runs once per readiness announcement.
    void armShell();
    // Brings the shell up to the desired state. No-op until ready.
    void applyPresence();
    void applyProgress();
    void resetApplied();
    // Logs a refused call once per (operation, HRESULT) pair -- the shell is
    // called on every state change and a per-call line would bury the first one.
    void reportResult(const char* operation, qint32 hr);

    std::unique_ptr<TaskbarShell> shell_;
    void* hwnd_ = nullptr;
    bool ready_ = false;
    bool shell_available_ = false;
    bool buttons_registered_ = false;

    ShellPresenceState desired_;
    // What the shell was last told, so an unchanged state costs no COM call.
    // Reset with the handle: a new taskbar button has been told nothing.
    bool applied_valid_ = false;
    ShellPresenceState applied_;

    TaskbarProgressLedger progress_;
    bool progress_dirty_ = false;

    // (operation, hr) of the last reported failure, so a repeated refusal is
    // silent and a NEW cause is not.
    const char* last_failed_operation_ = nullptr;
    qint32 last_failed_hr_ = 0;
};

} // namespace exosnap::quick
