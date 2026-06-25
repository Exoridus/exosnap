#include "AboutOverlay.h"
#include "UpdateSettingsPanel.h"

namespace exosnap::ui::dialogs {

AboutOverlay::AboutOverlay(QWidget* parent) : QWidget(parent) {
    setObjectName("aboutOverlay");
    setVisible(false);

    // Hidden update settings panel — kept for MainWindow wiring compatibility only.
    // MainWindow calls updatePanel()->setState/setModel/setRecordingActive; this is
    // a no-op visually since the panel is never added to any visible layout.
    update_panel_ = new UpdateSettingsPanel(this);
    update_panel_->setObjectName(QStringLiteral("aboutUpdatePanel"));
    update_panel_->hide();
    connect(update_panel_, &UpdateSettingsPanel::checkRequested, this, &AboutOverlay::checkForUpdatesRequested);
}

} // namespace exosnap::ui::dialogs
