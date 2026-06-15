#pragma once

#include <QString>
#include <QStringList>
#include <QWidget>

class QCheckBox;
class QFrame;
class QLabel;
class QMenu;
class QPushButton;

namespace exosnap::ui::dialogs {

// Data the crash reporter renders. The reporter is intentionally fed a
// pre-scrubbed model: every field here is already the exact, privacy-safe value
// that the read-only report displays (and, on consent, uploads). No file paths,
// usernames or recording content ever appear here — see crash_dir / dmp_path
// which are referenced by the actions but never rendered into the report body.
struct CrashReportModel {
    bool recording_was_active = false; // shows the green "recording secured" banner when true
    QString exception;                 // e.g. "0xC0000005 · ACCESS_VIOLATION"
    QString module;                    // e.g. "exosnap.dll +0x3f1a2"
    QString thread;                    // e.g. "\"encoder\" (#7)"
    QStringList stack;                 // top frames, e.g. {"exo::EncoderNVENC::submitFrame()", ...}
    QString version;                   // e.g. "1.0.4 · build a5d55f1"
    QString os;                        // e.g. "Windows 11 · 26100.1742"
    QString gpu;                       // e.g. "NVIDIA RTX 4070 · driver 552.44"
    QString encoder;                   // e.g. "NVENC AV1 → MKV"
    QString crash_dir;                 // folder for "Open crash folder"
    QString dmp_path;                  // raw minidump path (referenced, not displayed)
};

// The full crash-report card (FINAL design · ADR 0017). Renders the complete
// 460px-wide card *including* the separate-process window-chrome bar, so it can
// be used two ways:
//   1. embedded inside CrashReportOverlay (in-window scrim), or
//   2. as the central content of a standalone top-level reporter window
//      (a later slice hosts it in a dedicated reporter process).
// It therefore never assumes an overlay parent.
//
// Nothing is sent unless the user chooses to: the auto-send opt-in defaults OFF,
// the scrubbed report is collapsed by default, and every action is surfaced as a
// signal for the host to wire.
class CrashReportPanel : public QWidget {
    Q_OBJECT
  public:
    explicit CrashReportPanel(const CrashReportModel& model, QWidget* parent = nullptr);

    // Current state of the "Send reports automatically next time" opt-in.
    bool autoSendChecked() const;

  signals:
    void sendReportRequested();
    void restartRequested();
    void reportOnGitHubRequested();
    void openCrashFolderRequested();
    void dontSendRequested();
    void autoSendToggled(bool checked);

  private:
    QWidget* buildChromeBar();
    QWidget* buildStatement();
    QWidget* buildRecordingBanner();
    QWidget* buildTransparencyBlock();
    QWidget* buildDetailsSection();
    QWidget* buildScrubbedReport();
    QWidget* buildActionsRow();
    void toggleDetails();

    CrashReportModel model_;

    QPushButton* details_toggle_ = nullptr;
    QFrame* scrubbed_report_ = nullptr;
    QCheckBox* auto_send_check_ = nullptr;
    QPushButton* overflow_button_ = nullptr;
    QMenu* overflow_menu_ = nullptr;
    bool details_expanded_ = false;
};

} // namespace exosnap::ui::dialogs
