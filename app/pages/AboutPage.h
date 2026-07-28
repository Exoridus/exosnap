#pragma once
#include <QString>
#include <QWidget>

#include <update/update_types.h>

class QLabel;
class QPushButton;
class QThread;

namespace exosnap::pages {

// Fields injected into BuildAboutCopyText(). Keeping these as plain data lets
// the exact clipboard text be unit-tested without a QApplication, a real
// build, or filesystem/registry access.
struct AboutCopyFields {
    QString version;
    bool official_build = false;
    QString git_commit_full;
    QString build_timestamp_utc;
    QString build_id;      // empty renders as "(none)"
    QString configuration; // EXOSNAP_BUILD_CONFIG, e.g. "Debug"/"Release"
    bool dirty_source_tree = false;
    QString install_mode_label; // "Portable" | "MSI" | "Scoop"
    QString channel;            // e.g. "Stable", "Preview"
    QString executable_path;    // native-separator path
    QString executable_sha256;  // 64 lowercase hex chars
};

// ── Pure formatting helpers (testable without a widget) ───────────────────────

// Renders an ISO 8601 UTC timestamp (e.g. "2026-07-28T12:34:56Z", the format
// produced by GenerateBuildInfo.cmake) as "YYYY-MM-DD HH:MM UTC". Falls back
// to the raw input unchanged if it cannot be parsed.
QString FormatBuildTimestampForDisplay(const QString& iso8601_utc);

// "MSI" when install_mode is Installed (registry marker present -- takes
// precedence), else "Scoop" when running from a Scoop-managed tree, else
// "Portable".
QString ResolveInstallModeLabel(exosnap::update::InstallMode install_mode, bool is_scoop);

// Builds the exact "Copy details" clipboard text from injected fields.
QString BuildAboutCopyText(const AboutCopyFields& fields);

// About nav page — minimal identity card embedded directly in the
// QStackedWidget nav stack. Reached via the "About" nav item (normal
// navPageRequested, no overlay). No close/dismiss affordance.
//
// Content: identity card (brand mark + wordmark + version), description
// sentence, metadata table (Version/Commit/Built/Installation/Channel/Author),
// conditional notice line(s) (Unofficial/Debug/Dirty), and action buttons
// (GitHub / Copy details / Release notes).
//
// No update status line and no UpdateSettingsPanel — updates live in
// Settings only.
class AboutPage : public QWidget {
    Q_OBJECT
  public:
    explicit AboutPage(QWidget* parent = nullptr);
    ~AboutPage() override;

    // Sets the channel string shown in the Channel metadata row (e.g. "Stable", "Preview").
    void setChannelHint(const QString& channel);

    // Re-bakes the two-tone wordmark rich-text from ActiveTheme(). Call after a theme switch.
    void refreshBrand();

  signals:
    // Emitted once the "Copy details" click has finished writing to the
    // clipboard (after the async executable-hash step completes). Carries the
    // exact text written, mainly so tests can QSignalSpy/wait on it instead of
    // polling button state.
    void copyDetailsFinished(QString clipboardText);

  private:
    void startCopyDetails();
    void finishCopyDetails(const QString& exe_sha256);

    QLabel* wordmark_ = nullptr;
    QLabel* channel_value_ = nullptr;
    QPushButton* copy_button_ = nullptr;
    QString install_mode_label_;

    // Lazy, async executable hash (computed on first "Copy details" click so
    // the UI never blocks; cached so a second click is instant).
    QThread* hash_thread_ = nullptr;
    QString cached_exe_sha256_;
    bool hash_in_progress_ = false;
};

} // namespace exosnap::pages
