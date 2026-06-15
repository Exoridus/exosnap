#pragma once

#include <QString>
#include <QStringList>
#include <QWidget>

class QButtonGroup;
class QFrame;
class QLabel;
class QPushButton;
class QVBoxLayout;

namespace exosnap::ui::dialogs {

// Lifecycle states the Settings update card can render. 0.4.0 ships the
// notify-and-open-releases scope: there is no in-place download/install state
// (the WinHTTP downloader + verified-install dance is deferred — see Update C),
// so the design's Downloading / Ready / Rollback states are intentionally absent.
enum class UpdateUiState {
    UpToDate,  // "You're up to date" + Check button + channel selector
    Checking,  // lightweight "Checking for updates…" state
    Available, // What's new + channel selector + Open-releases primary action
    Error,     // caution banner with a retry
};

// Data the card renders. Everything here is presentation-only; the host (S4's
// ConfigPage embed + UpdateService wiring) feeds a fresh model per state change.
struct UpdateUiModel {
    QString current_version;       // e.g. "0.4.0"
    QString available_version;     // e.g. "0.5.0" (Available only)
    QString last_checked;          // e.g. "2 minutes ago" / "Never"
    QStringList whats_new;         // bulleted changelog highlights (Available only)
    QString release_url;           // releases page (Open releases page)
    QString release_notes_url;     // tag-specific notes (Release notes link)
    QString error_message;         // shown in the Error banner
    bool recording_active = false; // pauses checks; shows the amber caution banner
    QString channel;               // "Stable" | "Preview"
};

// The Settings "Software updates" card — a plain embeddable QWidget (never a
// QDialog), mirroring SourcePickerPanel / WebcamSetupPanel so S4 can drop it into
// ConfigPage's Advanced/Updates section.
//
// 0.4.0 scope: user-initiated checks, notify-only on an available update, and a
// manual "Open releases page" hand-off — no in-place updater. Channel switches
// apply immediately (persist + re-check) rather than the design's restart dance.
class UpdateSettingsPanel : public QWidget {
    Q_OBJECT
  public:
    explicit UpdateSettingsPanel(QWidget* parent = nullptr);

    void setState(UpdateUiState state);
    void setModel(const UpdateUiModel& model);
    void setRecordingActive(bool active);
    QString channel() const;

  signals:
    void checkRequested();
    void channelChanged(const QString& channel);
    void openReleasesPageRequested();
    void openReleaseNotesRequested();
    void remindLaterRequested();

  private:
    void rebuild();         // re-renders the body for the current state/model
    QWidget* buildHeader(); // icon tile + title + subline + cur→next pill
    QWidget* buildChannelSelector();
    QWidget* buildRecordingBanner();
    void clearBody();
    void selectChannelButton(const QString& channel);

    UpdateUiState state_ = UpdateUiState::UpToDate;
    UpdateUiModel model_;

    QVBoxLayout* body_layout_ = nullptr; // holds the per-state content below the header

    // Header (rebuilt per state).
    QLabel* header_tile_ = nullptr;
    QLabel* header_title_ = nullptr;
    QLabel* header_sub_ = nullptr;
    QLabel* version_pill_ = nullptr;

    // Persistent controls created in the ctor and re-parented per state.
    QPushButton* check_button_ = nullptr;
    QPushButton* channel_stable_ = nullptr;
    QPushButton* channel_preview_ = nullptr;
    QButtonGroup* channel_group_ = nullptr;
};

} // namespace exosnap::ui::dialogs
