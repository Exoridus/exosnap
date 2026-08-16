#pragma once

#include "models/CloseGuardPolicy.h"

#include <QObject>
#include <QString>
#include <QtQmlIntegration/qqmlintegration.h>

#include <functional>

namespace exosnap::quick {

// Shell-level concerns that belong to the application rather than to any one
// page: today the close guards, which decide whether the window may close at
// all.
//
// The asymmetry with the Widgets shell is deliberate. There, `closeEvent` ran
// `QMessageBox::exec()` and read the answer on the next line. A QML dialog has
// no `exec()`, so the sequence has to be split: `requestClose()` samples the
// state and either allows the close outright or publishes a prompt; QML shows
// the prompt and calls back; `closeApproved()` is what finally closes the
// window. The *policy* — which guard fires, in what order, with what wording —
// stays in C++ (models/CloseGuardPolicy). QML only displays and routes.
class ShellAdapter : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("ShellAdapter is provided by the application")

    // Canonical navigation order (product decision, see CLAUDE.md):
    // Record, Settings, Diagnostics, Logs, About — all five are direct
    // destinations in the title band, in this order. Named here so a navigation
    // request never has to spell a bare integer.
    //
    // Device is deliberately absent: it owned no user-selectable configuration,
    // and its read-only adapter/encoder capability content now lives in
    // Diagnostics, which is where what ExoSnap OBSERVES belongs.
  public:
    enum Page { RecordPage = 0, SettingsPage = 1, DiagnosticsPage = 2, LogsPage = 3, AboutPage = 4 };
    Q_ENUM(Page)

  private:
    Q_PROPERTY(bool closeGuardActive READ closeGuardActive NOTIFY closeGuardChanged FINAL)
    Q_PROPERTY(QString closeGuardTitle READ closeGuardTitle NOTIFY closeGuardChanged FINAL)
    Q_PROPERTY(QString closeGuardBody READ closeGuardBody NOTIFY closeGuardChanged FINAL)
    Q_PROPERTY(QString closeGuardProceedLabel READ closeGuardProceedLabel NOTIFY closeGuardChanged FINAL)
    Q_PROPERTY(QString closeGuardCancelLabel READ closeGuardCancelLabel NOTIFY closeGuardChanged FINAL)
    Q_PROPERTY(bool closeGuardDefaultIsCancel READ closeGuardDefaultIsCancel NOTIFY closeGuardChanged FINAL)

  public:
    explicit ShellAdapter(QObject* parent = nullptr);

    // Supplies the live state the guards read. Set once by QuickApplication;
    // invoked on every close attempt so the answer is never stale.
    void setStateProvider(std::function<CloseGuardState()> provider);

    // Answers "should this close attempt hide to the tray instead of closing?".
    // Set once by QuickApplication, which owns the persisted `keep running in
    // tray` preference, the force-quit latch and the tray icon itself. Consulted
    // BEFORE the close guards, mirroring the Widgets shell: hiding to the tray
    // must not interrogate the user about a recording that is deliberately meant
    // to keep running. Absent (or returning false) means the guards decide.
    void setHideToTrayProvider(std::function<bool()> provider);

    [[nodiscard]] bool closeGuardActive() const noexcept;
    [[nodiscard]] const QString& closeGuardTitle() const noexcept;
    [[nodiscard]] const QString& closeGuardBody() const noexcept;
    [[nodiscard]] const QString& closeGuardProceedLabel() const noexcept;
    [[nodiscard]] const QString& closeGuardCancelLabel() const noexcept;
    [[nodiscard]] bool closeGuardDefaultIsCancel() const noexcept;

    // Called from Window.onClosing. Returns true when the window may close
    // right now. False means either a prompt is up (closeGuardActive) or the
    // close was refused outright (finalize in flight).
    Q_INVOKABLE bool requestClose();
    // The user chose the proceeding option. Applies that guard's effect, waives
    // the condition, and re-evaluates — an export cancelled while a recording is
    // still running must still ask about the recording.
    Q_INVOKABLE void confirmCloseGuard();
    // The user chose to keep the window open. Clears every waiver, so a later
    // close attempt starts from a clean slate.
    Q_INVOKABLE void cancelCloseGuard();

  signals:
    // Navigation requested from outside a page — today a notification action
    // ("Open update", "Rebind hotkey", "See the drop breakdown"). The shell owns
    // the nav index, so the request lands here rather than at the source.
    //
    // Carries the `Page` enum above, not a bare int: that is the whole reason the
    // enum exists, and Q_ENUM means QML writes ShellAdapter.SettingsPage instead
    // of a 1 that silently means something else after a reorder.
    void navigateToPageRequested(Page page);

    void closeGuardChanged();
    // Every guard has cleared; the frontend may close the window now.
    void closeApproved();
    // This close attempt resolved to "hide to the tray". The window must stay
    // alive: recording, hotkeys and every service keep running behind it. The
    // hide itself is the application's to perform — this adapter holds no window
    // reference, the same way it holds no service references.
    void hideToTrayRequested();
    // Effects the application must apply. Emitted rather than executed here so
    // the adapter keeps no service references.
    void cancelRemuxRequested();
    void cancelExportRequested();
    void stopRecordingRequested();

  private:
    // Applies the waivers collected so far to a freshly sampled state.
    [[nodiscard]] CloseGuardState currentState() const;
    void publish(const CloseGuardPrompt& prompt);
    void clearPrompt();

    std::function<CloseGuardState()> state_provider_;
    std::function<bool()> hide_to_tray_provider_;
    CloseGuardPrompt prompt_;
    // Conditions the user has already answered "proceed" for during this close
    // attempt. Without them the re-evaluation after a confirm would show the
    // same dialog forever, because the effects are asynchronous: cancelling a
    // remux or stopping a recording does not clear the underlying state within
    // this call.
    bool waived_remux_ = false;
    bool waived_export_ = false;
    bool waived_recording_ = false;
    bool active_ = false;
};

} // namespace exosnap::quick
