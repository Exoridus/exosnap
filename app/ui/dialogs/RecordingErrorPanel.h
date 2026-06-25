#pragma once

#include <QString>
#include <QWidget>

class QLabel;
class QPushButton;

namespace exosnap::ui::dialogs {

// Data the recording-error reporter renders. Unlike CrashReportModel this is a
// recoverable, non-fatal failure surfaced right after a Record attempt. Every
// field here is safe to show locally; the Sentry report path scrubs paths in
// crash_capture::ReportNonFatalError, so `detail` may contain a local path for
// on-screen display without it ever leaving the machine verbatim.
struct RecordingErrorModel {
    QString title;   // e.g. "Recording could not start"
    QString summary; // one-line plain explanation
    QString phase;   // engine error phase, e.g. "Validate" / "Mux" / "Encode"
    QString code;    // HRESULT text, e.g. "0x80004001" (may be empty)
    QString detail;  // human-readable engine detail (may contain a local path)

    // Container/codec context — shown to the user and (when sent) attached as
    // allow-listed Sentry tags. Empty strings are omitted from the display.
    QString container;
    QString video_codec;
    QString audio_codec;

    // True only in an official build with a compiled-in DSN AND crash capture
    // active. When false the "Send report" action is hidden entirely (self-builds
    // never phone home, so offering it would be misleading).
    bool can_send_report = false;
};

// In-window recording-error card. A compact sibling of CrashReportPanel: a warn
// header, the scrubbable failure detail, and up to three actions. It never
// assumes an overlay parent, so it can be embedded in RecordingErrorOverlay's
// scrim or hosted standalone in tests.
//
// Nothing is sent unless the user explicitly clicks "Send report"; the action is
// only present when can_send_report is true.
class RecordingErrorPanel : public QWidget {
    Q_OBJECT
  public:
    explicit RecordingErrorPanel(const RecordingErrorModel& model, QWidget* parent = nullptr);

    // Whether this panel exposes the Sentry "Send report" action (mirrors
    // model.can_send_report). Used by tests and by the overlay host.
    bool canSendReport() const noexcept;

  signals:
    void sendReportRequested();
    void openLogsRequested();
    void dismissRequested();

  private:
    QWidget* buildHeader();
    QWidget* buildDetailBox();
    QWidget* buildPrivacyNote();
    QWidget* buildActionsRow();

    RecordingErrorModel model_;
    QPushButton* send_button_ = nullptr;
};

} // namespace exosnap::ui::dialogs
