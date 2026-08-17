#pragma once

// LiveVerifySource.h -- everything the Live Verify control channel is allowed to
// see or do, expressed as one narrow interface.
//
// This is the security boundary, not a convenience abstraction. There is no
// "invoke method by name", no property path, no object lookup: a command that is
// not a member function here cannot be reached over the pipe, and adding one is
// a code change with a review, not a runtime capability. The methods are
// deliberately coarse (whole snapshots, whole intents) so the wire format cannot
// be used to poke at internals one field at a time.
//
// The implementation binds to the SAME application intents the QML surface uses
// (RecordViewModelAdapter::requestStart() and friends). It must never reach past
// them into the recorder or the view model -- an acceptance check that drove a
// private path would prove something users never execute.
//
// Preconditions are NOT here. They live in LiveVerifyCommandPolicy, evaluated
// against State() before any of these run, so the same predicates answer "may
// this run" and "what is available right now". An intent that still returns
// false after its precondition passed means the product refused between the two,
// and the dispatcher re-reads the policy to say why rather than inventing a
// second explanation.
//
// Every call runs on the Qt GUI thread; implementations do not need to be
// thread-safe.

#include "LiveVerifyAutomationState.h"

#include <QJsonObject>
#include <QString>

#include <cstdint>

namespace exosnap::live_verify {

class LiveVerifySource {
  public:
    virtual ~LiveVerifySource() = default;

    // --- Identity -----------------------------------------------------------
    // Answered by system.hello. Must carry enough for the runner to refuse a
    // process it did not mean to talk to: product version, full commit SHA,
    // build id, configuration, executable path + SHA-256, PID, install mode and
    // channel.
    [[nodiscard]] virtual QJsonObject Identity() const = 0;

    // --- Product state (protocol 2) -----------------------------------------
    // The observable state, in product vocabulary. Feeds ui.getState, the
    // precondition policy and availableActions -- one read, three consumers, so
    // a client can never be told an action is available by one of them and
    // refused by another.
    [[nodiscard]] virtual AutomationState State() const = 0;
    // Monotonic. Advances exactly when State() would compare unequal, which is
    // what lets a runner wait on "something observable changed" instead of on a
    // clock.
    [[nodiscard]] virtual std::uint64_t StateRevision() const = 0;

    // --- Read-only snapshots ------------------------------------------------
    [[nodiscard]] virtual QJsonObject SystemSnapshot() const = 0;
    [[nodiscard]] virtual QJsonObject AppSnapshot() const = 0;
    [[nodiscard]] virtual QJsonObject WindowSnapshot() const = 0;
    [[nodiscard]] virtual QJsonObject PreviewSnapshot() const = 0;
    [[nodiscard]] virtual QJsonObject RecordSnapshot() const = 0;
    [[nodiscard]] virtual QJsonObject RecordResult() const = 0;
    [[nodiscard]] virtual QJsonObject OverlaySnapshot() const = 0;
    [[nodiscard]] virtual QJsonObject EditorSnapshot() const = 0;
    [[nodiscard]] virtual QJsonObject DiagnosticsSnapshot() const = 0;

    // --- Observability surfaces (Wave C) -------------------------------------
    // Each of these answers from the ONE owner of the fact it reports -- the
    // engine's diagnostics snapshot, the recommendation engine's checklist, the
    // settings models plus the capability resolver, the logging ring, the session
    // reports on disk. None of them measures anything: a surface that took its
    // own measurement would be a second truth about a machine that already has
    // one, and the two would disagree the first time either changed.
    //
    // Separate queries rather than one everything.snapshot. They have different
    // costs, different availability stories and different consumers; a client
    // that wants the pipeline should not be made to pay for a display probe.

    // The live recording pipeline, from recorder_core::RecordingDiagnosticsSnapshot.
    [[nodiscard]] virtual QJsonObject PipelineSnapshot() const = 0;
    // requested / effective / running recording configuration, plus app settings.
    [[nodiscard]] virtual QJsonObject SettingsSnapshot() const = 0;
    // The structured diagnostics checklist, tiers and fix actions preserved.
    [[nodiscard]] virtual QJsonObject DiagnosticsResults() const = 0;
    // What ExoSnap OBSERVES about the machine. Read-only in every sense: nothing
    // reachable from here can change a Windows-global state.
    [[nodiscard]] virtual QJsonObject EnvironmentSnapshot() const = 0;
    // Every native top-level window with its semantic ROLE. The role plus this
    // process's identity is the automation identity; the title is reported for
    // humans and must never be matched on.
    [[nodiscard]] virtual QJsonObject WindowsSnapshot() const = 0;
    // A bounded, filtered read of the structured event ring. Never a log-file
    // API: no path, no offset, no follow. `error` is set for a malformed filter.
    [[nodiscard]] virtual QJsonObject RecentEvents(const QJsonObject& params, QString* error) const = 0;
    // The canonical on-disk session report -- the latest one when the id is
    // empty. The same document the support bundle ships, not a second report.
    [[nodiscard]] virtual QJsonObject SessionReport(const QString& recording_session_id) const = 0;

    // --- Intents ------------------------------------------------------------
    // All return false with a filled `error` rather than throwing; a refused
    // intent is a normal, reportable acceptance outcome.

    // Places the main window on the named screen. The name is QScreen::name(),
    // which on Windows is the monitor's friendly name (e.g. "27GL850") and NOT
    // the "\\\\.\\DISPLAYn" device name -- callers should read it out of
    // SystemSnapshot() rather than construct it. Deliberately the ONLY window
    // mutation exposed: the
    // cross-monitor Preview check needs a boundary crossing, and no acceptance
    // requirement needs arbitrary HWND style changes.
    virtual bool MoveWindowToScreen(const QString& screen_name, QString* error) = 0;

    // Selects a capture target the way a source-picker click does.
    // `kind` is "monitor" | "window"; `title_filter` is ignored for monitors.
    virtual bool SelectRecordTarget(const QString& kind, const QString& title_filter, QString* error) = 0;

    virtual bool RecordStart(QString* error) = 0;
    virtual bool RecordPause(QString* error) = 0;
    virtual bool RecordResume(QString* error) = 0;
    virtual bool RecordStop(QString* error) = 0;
    virtual bool RecordSplit(QString* error) = 0;
    virtual bool RecordCaptureFrame(QString* error) = 0;

    // --- Shell intents (protocol 2) -----------------------------------------

    // Navigates through the shell's single navigation edge -- the same one the
    // five tabs, Ctrl+1..5 and every notification action write to. `page` is a
    // product page name, never an index.
    virtual bool Navigate(const QString& page, QString* error) = 0;

    enum class RevealOutcome {
        Revealed,
        // The name is not one of the surface's automation targets. A client
        // error, and answered as one -- a silent no-op here is the exact defect
        // --settings-visual-bottom had: every capture claimed to show the end of
        // the page while showing its top.
        UnknownTarget,
        // The target is a real one and it did not end up in the viewport, or the
        // surface could not be reached at all. NOT the same statement as
        // UnknownTarget, and it must not borrow that code: "you asked for
        // something that does not exist" and "what you asked for did not happen"
        // send a runner to two different places.
        Failed,
    };
    [[nodiscard]] virtual RevealOutcome Reveal(const QString& surface, const QString& target, QString* error) = 0;
    virtual bool ScrollHome(const QString& surface, QString* error) = 0;
    virtual bool ScrollEnd(const QString& surface, QString* error) = 0;

    // --- Update intents -----------------------------------------------------
    // Both bind to the SAME product paths the Settings update card drives. An
    // automation-only shortcut into the update engine would prove something no
    // user executes -- and for the update path that is the whole point, since
    // what is being accepted is the real handoff, not a test harness's.

    // The card's "Check for updates": the manual check, with its recording
    // guard and its loop-guard reset. Asynchronous -- the result arrives as an
    // update state change.
    virtual bool UpdateCheck(QString* error) = 0;
    // The card's primary action when an update is offered: stage the updater and
    // launch it. Asynchronous, and the meaningful completion is the CHILD
    // process, not this call.
    virtual bool UpdateApply(QString* error) = 0;
    // What the last apply actually started, for a client that has to reach the
    // child: pid, the staged executable and its SHA-256, the pinned target
    // version, and the endpoint the child was given. Empty before the first
    // launch. Read-only.
    [[nodiscard]] virtual QJsonObject UpdaterLaunchSnapshot() const = 0;

    virtual bool SetSourcePickerOpen(bool open, QString* error) = 0;
    virtual bool SetNotificationHubOpen(bool open, QString* error) = 0;
    virtual bool ClearNotifications(QString* error) = 0;

    // --- Edit intents (protocol 2) ------------------------------------------
    // Clamping, trim ordering, keyframe snapping and the close teardown all stay
    // where they are: these call the same Q_INVOKABLE seams the Edit surface's
    // own controls and keys call.
    virtual bool EditOpen(QString* error) = 0;
    virtual bool EditPlayPause(QString* error) = 0;
    virtual bool EditSeek(qint64 position_ms, QString* error) = 0;
    virtual bool EditSetTrimIn(qint64 position_ms, QString* error) = 0;
    virtual bool EditSetTrimOut(qint64 position_ms, QString* error) = 0;
    virtual bool EditTimelineHome(QString* error) = 0;
    virtual bool EditTimelineEnd(QString* error) = 0;
    virtual bool EditClose(QString* error) = 0;
};

} // namespace exosnap::live_verify
