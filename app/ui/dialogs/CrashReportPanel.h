#pragma once

#include <QString>
#include <QStringList>
#include <QWidget>

class QFrame;
class QLabel;
class QPushButton;

namespace exosnap::ui::widgets {
class ExoCheckBox;
}

namespace exosnap::ui::dialogs {

// Data the crash reporter renders. This is local session evidence, not a claim
// that every displayed value belongs to the separate Sentry upload channels.
// crash_dir / dmp_path drive local actions and availability only; paths are
// never rendered into the report body.
struct CrashReportModel {
    bool recording_was_active = false; // shows the green "recording secured" banner when true
    QString exception;                 // e.g. "0xC0000005 · ACCESS_VIOLATION"
    QString module;                    // e.g. "exosnap.dll +0x3f1a2"
    QString thread;                    // e.g. "\"encoder\" (#7)"
    QStringList stack;                 // top frames, e.g. {"exo::EncoderNVENC::submitFrame()", ...}
    QString version;                   // e.g. "1.0.4 · build a5d55f1"
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
// Nothing is sent unless the user chooses to. Remember-choice is local draft
// state only; the host commits it transactionally with Send / Don't send.
class CrashReportPanel : public QWidget {
    Q_OBJECT
  public:
    explicit CrashReportPanel(const CrashReportModel& model, QWidget* parent = nullptr);

    bool rememberChoiceChecked() const;

  signals:
    void sendReportRequested();
    void openCrashFolderRequested();
    void dontSendRequested();
    void dismissRequested();

  private:
    // Rebuilds the theme-coloured card content from the active theme. Runs once at
    // construction and again on every ReapplyTheme() (theme switch), so the inline
    // stylesheets and tinted glyphs re-derive their colours instead of keeping the
    // dark values baked in at construction time. Draft checkbox and privacy
    // disclosure state are preserved without emitting host-facing signals.
    void applyTheme();

    QWidget* buildChromeBar();
    QWidget* buildStatement();
    QWidget* buildRecordingBanner();
    QWidget* buildSummary();
    QWidget* buildPrivacyDisclosure();
    QWidget* buildPrivacyDetails();
    QWidget* buildActionsRow();
    void updatePrivacyDisclosureState();
    void updateRememberState();

    CrashReportModel model_;

    QWidget* content_ = nullptr;
    QPushButton* privacy_toggle_ = nullptr;
    QLabel* privacy_chevron_ = nullptr;
    QWidget* privacy_details_ = nullptr;
    widgets::ExoCheckBox* remember_choice_check_ = nullptr;
    QLabel* remember_hint_ = nullptr;
    QPushButton* send_button_ = nullptr;
    QPushButton* decline_button_ = nullptr;
    bool privacy_expanded_ = false;
};

} // namespace exosnap::ui::dialogs
