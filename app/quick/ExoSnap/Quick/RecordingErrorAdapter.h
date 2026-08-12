#pragma once

#include "models/RecordingFailurePolicy.h"

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

namespace exosnap::quick {

// Narrow QML boundary for a failed recording.
//
// There is exactly one authoritative failure state, and it is the recording
// result itself: `present()` is fed the report that models::BuildRecordingFailureReport
// derived from it, so the surface can never claim a failure the engine did not
// report, or a different one. Nothing here re-decides whether a result failed,
// whether the disk-space stop counts, or what the failure is called.
//
// Sending is deliberately NOT performed here. It needs crash_capture consent,
// encoder context and the non-fatal report call — SDK concerns that belong to
// the composition root, which reads report() when sendReportRequested() fires.
class RecordingErrorAdapter : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("RecordingErrorAdapter is provided by the application")

    Q_PROPERTY(bool active READ active NOTIFY changed FINAL)
    Q_PROPERTY(QString title READ title NOTIFY changed FINAL)
    Q_PROPERTY(QString summary READ summary NOTIFY changed FINAL)
    // Label/value pairs for the shared fact table: PHASE, CODE, FORMAT, DETAIL.
    // Assembled here — which rows exist depends on which fields the engine
    // filled, and the engine's enum spelling is humanized before it is shown.
    Q_PROPERTY(QVariantList detailRows READ detailRows NOTIFY changed FINAL)
    // False in a self-build or with crash capture off. The action is then hidden
    // rather than disabled: offering to send from a build that never phones home
    // would be a lie about what the button does.
    Q_PROPERTY(bool canSendReport READ canSendReport NOTIFY changed FINAL)

  public:
    explicit RecordingErrorAdapter(QObject* parent = nullptr);

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] const QString& title() const noexcept;
    [[nodiscard]] const QString& summary() const noexcept;
    [[nodiscard]] const QVariantList& detailRows() const noexcept;
    [[nodiscard]] bool canSendReport() const noexcept;

    // The report the composition root attaches when the user opts in. Empty
    // while inactive.
    [[nodiscard]] const models::RecordingFailureReport& report() const noexcept;

    // Raises the surface. A second failure replaces the first: the newest
    // attempt is the one the user just made.
    void present(const models::RecordingFailureReport& report, bool can_send_report);

    Q_INVOKABLE void dismiss();
    // Explicit opt-in. Lowers the surface afterwards, matching the Widgets
    // shell: the user has acted, and leaving a modal up with a spent button is
    // not a state worth having.
    Q_INVOKABLE void sendReport();
    Q_INVOKABLE void openLogs();

  signals:
    void changed();
    void sendReportRequested();
    void openLogsRequested();

  private:
    void rebuildDetailRows();

    models::RecordingFailureReport report_;
    QVariantList detail_rows_;
    bool active_ = false;
    bool can_send_report_ = false;
};

} // namespace exosnap::quick
