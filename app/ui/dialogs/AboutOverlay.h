#pragma once
#include <QWidget>

class QFrame;
class QEvent;
class QKeyEvent;
class QLabel;
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
// v10: About is minimal — identity card + facts table + actions. The full update
// control (channel selector, check button) lives in Settings. About shows only a
// quiet one-line update status row inside the info card.
//
// The UpdateSettingsPanel is kept as a hidden internal object so that MainWindow's
// existing update-service wiring (setState / setModel / setRecordingActive) continues
// to work without changes. Use setUpdateStatusText() to push a visible status line,
// and setChannelHint() to surface the active channel in the metadata table.
class AboutOverlay : public QWidget {
    Q_OBJECT
  public:
    explicit AboutOverlay(QWidget* parent = nullptr);

    void openOverlay();
    void closeOverlay();
    bool isOpen() const noexcept;

    // Returns the hidden update settings panel (never null after construction).
    // MainWindow uses this to drive update state without holding a separate reference.
    UpdateSettingsPanel* updatePanel() const {
        return update_panel_;
    }

    // Sets the quiet one-line update status text shown at the bottom of the info card.
    // Pass an empty string to hide the row entirely.
    void setUpdateStatusText(const QString& text);

    // Sets the channel string shown in the Channel metadata row (e.g. "Stable", "Preview").
    // Call whenever the persisted channel changes.
    void setChannelHint(const QString& channel);

    // Re-bakes the two-tone wordmark rich-text from ActiveTheme(). Call after a theme switch.
    void refreshBrand();

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
    // Builds and returns the info card QFrame (objectName "aboutCard").
    QFrame* buildCard();

    // The centered info card.
    QFrame* card_ = nullptr;
    // Stored to allow refreshBrand() to re-bake the rich-text wordmark on theme switch.
    QLabel* wordmark_ = nullptr;
    // Channel value shown in the metadata table — updated via setChannelHint().
    QLabel* channel_value_ = nullptr;
    // Quiet one-line update status at the bottom of the info card — updated via setUpdateStatusText().
    QLabel* update_status_line_ = nullptr;
    // Hidden update settings panel — kept for MainWindow wiring compatibility only; never shown.
    UpdateSettingsPanel* update_panel_ = nullptr;
};

} // namespace exosnap::ui::dialogs
