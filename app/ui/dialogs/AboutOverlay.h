#pragma once
#include <QWidget>

class QFrame;
class QEvent;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QShowEvent;

namespace exosnap::ui::dialogs {
class UpdateSettingsPanel;
} // namespace exosnap::ui::dialogs

namespace exosnap::ui::dialogs {

// In-window About surface. Renders as a translucent backdrop with a centered
// About card *inside* the application window — there is no separate native OS
// dialog, so nothing can linger as a focus-sticky child window after closing.
//
// Parent it to the host whose client area it should cover (typically the
// central widget); it tracks the parent's geometry and fills it. Open with
// openOverlay(); it dismisses on Escape, on a backdrop click, or via the Close
// action, emitting closed() each time it is dismissed.
//
// PS-PHASE-E: The overlay now hosts the UpdateSettingsPanel below the info card.
// MainWindow wires the update panel via updatePanel() instead of findChild on ConfigPage.
class AboutOverlay : public QWidget {
    Q_OBJECT
  public:
    explicit AboutOverlay(QWidget* parent = nullptr);

    void openOverlay();
    void closeOverlay();
    bool isOpen() const noexcept;

    // Returns the embedded update settings panel (never null after construction).
    UpdateSettingsPanel* updatePanel() const {
        return update_panel_;
    }

  signals:
    void closed();
    // Forwarded from UpdateSettingsPanel::checkRequested — MainWindow connects here.
    void checkForUpdatesRequested();

  protected:
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;

  private:
    void syncGeometryToParent();
    // Builds and returns the container widget (holds the aboutCard QFrame +
    // the UpdateSettingsPanel). card_ points to this container; the inner QFrame
    // keeps the objectName "aboutCard" for tests.
    QWidget* buildCard();

    // Container widget (holds aboutCard QFrame + update panel).
    QWidget* card_ = nullptr;
    // Embedded update settings panel — wired by MainWindow via updatePanel().
    UpdateSettingsPanel* update_panel_ = nullptr;
};

} // namespace exosnap::ui::dialogs
