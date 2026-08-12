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
// Every call runs on the Qt GUI thread; implementations do not need to be
// thread-safe.

#include <QJsonObject>
#include <QString>

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
};

} // namespace exosnap::live_verify
