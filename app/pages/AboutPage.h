#pragma once
#include <QWidget>

class QLabel;

namespace exosnap::pages {

// About nav page — minimal identity card embedded directly in the
// QStackedWidget nav stack. Reached via the "About" nav item (normal
// navPageRequested, no overlay). No close/dismiss affordance.
//
// Content: identity card (brand mark + wordmark + version), description
// sentence, metadata table (Version/Build/Commit/Channel/Author),
// and action buttons (GitHub / Copy details / Release notes).
//
// No update status line and no UpdateSettingsPanel — updates live in
// Settings only.
class AboutPage : public QWidget {
    Q_OBJECT
  public:
    explicit AboutPage(QWidget* parent = nullptr);

    // Sets the channel string shown in the Channel metadata row (e.g. "Stable", "Preview").
    void setChannelHint(const QString& channel);

    // Re-bakes the two-tone wordmark rich-text from ActiveTheme(). Call after a theme switch.
    void refreshBrand();

  private:
    QLabel* wordmark_ = nullptr;
    QLabel* channel_value_ = nullptr;
};

} // namespace exosnap::pages
