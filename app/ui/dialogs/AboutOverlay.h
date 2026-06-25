#pragma once
#include <QWidget>

namespace exosnap::ui::dialogs {
class UpdateSettingsPanel;
} // namespace exosnap::ui::dialogs

namespace exosnap::ui::dialogs {

// Thin compatibility shim — holds the hidden UpdateSettingsPanel so that
// MainWindow's existing update-service wiring (setState / setModel /
// setRecordingActive / channelChanged) continues to compile without changes.
//
// The visible About surface is now AboutPage (a plain QStackedWidget nav page).
// This class is no longer rendered or opened; it is kept only so that the
// update-panel wiring in MainWindow does not need a separate refactor.
class AboutOverlay : public QWidget {
    Q_OBJECT
  public:
    explicit AboutOverlay(QWidget* parent = nullptr);

    // Returns the hidden update settings panel (never null after construction).
    UpdateSettingsPanel* updatePanel() const {
        return update_panel_;
    }

  signals:
    // Forwarded from UpdateSettingsPanel::checkRequested — MainWindow connects here.
    void checkForUpdatesRequested();

  private:
    UpdateSettingsPanel* update_panel_ = nullptr;
};

} // namespace exosnap::ui::dialogs
