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
// A close is decided HERE and by nothing else. There is no preference that turns
// it into a hide -- putting the window away is the minimize gesture's job
// (models/WindowPresencePolicy), which never travels this path.
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
    // ── What is on screen, published by the frontend ──────────────────────────
    //
    // Written by QML, read by C++. The shell is the only object that knows which
    // destination is current and whether the Edit workspace and the source
    // picker are showing, and until now that knowledge existed ONLY inside the
    // QML document: the control channel read `currentPage` back out through
    // findChild("quickAppShell") plus a property lookup, which made a QML
    // objectName part of a public protocol and answered a bare integer whose
    // meaning is an enumerator order.
    //
    // These are facts, not commands. Navigation itself still goes through
    // navigateToPageRequested() below and through AppShell.navigateTo(), which
    // is where the one navigation policy lives (QCR-001) — writing `currentPage`
    // here does not move anything, it records where the shell arrived.
    Q_PROPERTY(int currentPage READ currentPage WRITE setCurrentPage NOTIFY currentPageChanged FINAL)
    // QCR-001 again: an open edit session is state of the Record destination, so
    // "the session is loaded" and "the workspace is on screen" are two different
    // facts. This is the second one.
    Q_PROPERTY(bool editSurfaceVisible READ editSurfaceVisible WRITE setEditSurfaceVisible NOTIFY
                   editSurfaceVisibleChanged FINAL)
    Q_PROPERTY(
        bool sourcePickerOpen READ sourcePickerOpen WRITE setSourcePickerOpen NOTIFY sourcePickerOpenChanged FINAL)

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

    [[nodiscard]] int currentPage() const noexcept;
    void setCurrentPage(int page);
    [[nodiscard]] bool editSurfaceVisible() const noexcept;
    void setEditSurfaceVisible(bool visible);
    [[nodiscard]] bool sourcePickerOpen() const noexcept;
    void setSourcePickerOpen(bool open);

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

    // The source picker, asked for from outside the Record page — today only the
    // control channel. Routed as a request rather than by writing
    // `sourcePickerOpen` for the same reason navigation is: the Loader, the
    // Popup and the resident-after-first-open contract belong to RecordPage, and
    // a second opener would be a second policy.
    void sourcePickerRequested(bool open);

    void currentPageChanged();
    void editSurfaceVisibleChanged();
    void sourcePickerOpenChanged();

    void closeGuardChanged();
    // Every guard has cleared; the frontend may close the window now.
    void closeApproved();
    // Every outcome of requestClose(), reported so the decision is recoverable from
    // a log. Every outcome except "allow" is indistinguishable from outside the
    // process -- the window simply stays -- so a user reporting "Quit did nothing"
    // otherwise leaves nothing behind that says whether the product refused, asked
    // something nobody saw, or failed to tear down. `kind` is one of the
    // CloseGuardKind keys.
    void closeDecided(const QString& kind, bool recording, bool exporting, bool remuxing);
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
    int current_page_ = RecordPage;
    bool edit_surface_visible_ = false;
    bool source_picker_open_ = false;
};

} // namespace exosnap::quick
