#pragma once

#include "RecordingErrorPanel.h"

#include <QWidget>

class QEvent;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QShowEvent;

namespace exosnap::ui::dialogs {

// In-window recording-error surface. Renders a translucent backdrop with a
// centered RecordingErrorPanel *inside* the application window — no separate
// native OS dialog, so nothing lingers as a focus-sticky child window after
// closing. Mirrors CrashReportOverlay / AboutOverlay.
//
// Parent it to the host whose client area it should cover (typically the central
// widget); it tracks the parent's geometry and fills it. Open with
// openOverlay(); it dismisses on Escape, on a backdrop click, or via the panel's
// Close button, emitting closed() each time it is dismissed. The panel's action
// signals are re-exposed so the host can wire them.
class RecordingErrorOverlay : public QWidget {
    Q_OBJECT
  public:
    explicit RecordingErrorOverlay(const RecordingErrorModel& model, QWidget* parent = nullptr);

    void openOverlay();
    void closeOverlay();
    bool isOpen() const noexcept;

  signals:
    // Forwarded from the embedded RecordingErrorPanel.
    void sendReportRequested();
    void openLogsRequested();

    // Emitted whenever the overlay is dismissed (Escape / backdrop / Close).
    void closed();

  protected:
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;

  private:
    void syncGeometryToParent();

    RecordingErrorPanel* panel_ = nullptr;
};

} // namespace exosnap::ui::dialogs
