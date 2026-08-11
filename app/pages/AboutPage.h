#pragma once
#include "../models/AboutInfo.h"

#include <QString>
#include <QWidget>

class QLabel;
class QPushButton;
class QThread;

namespace exosnap::pages {

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
    models::AboutInfo about_info_;

    // Lazy, async executable hash (computed on first "Copy details" click so
    // the UI never blocks; cached so a second click is instant).
    QThread* hash_thread_ = nullptr;
    QString cached_exe_sha256_;
    bool hash_in_progress_ = false;
};

} // namespace exosnap::pages
