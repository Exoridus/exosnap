#pragma once

#include "CrashReportPanel.h"

#include <QWidget>

class QEvent;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QShowEvent;

namespace exosnap::ui::dialogs {

// In-window crash-report surface. Renders as a translucent backdrop with a
// centered CrashReportPanel *inside* the application window — there is no
// separate native OS dialog, so nothing lingers as a focus-sticky child window
// after closing. Mirrors AboutOverlay / SourcePickerOverlay.
//
// Parent it to the host whose client area it should cover (typically the central
// widget); it tracks the parent's geometry and fills it. Open with
// openOverlay(); it dismisses on Escape, on a backdrop click, or via the panel's
// chrome close X, emitting closed() each time it is dismissed. The panel's
// action signals are re-exposed so the host can wire them.
class CrashReportOverlay : public QWidget {
    Q_OBJECT
  public:
    explicit CrashReportOverlay(const CrashReportModel& model, QWidget* parent = nullptr);

    void openOverlay();
    void closeOverlay();
    bool isOpen() const noexcept;

    // Whether the panel's "Send reports automatically next time" opt-in is checked.
    bool autoSendChecked() const;

  signals:
    // Forwarded from the embedded CrashReportPanel.
    void sendReportRequested();
    void restartRequested();
    void reportOnGitHubRequested();
    void openCrashFolderRequested();
    void dontSendRequested();
    void autoSendToggled(bool checked);

    // Emitted whenever the overlay is dismissed (Escape / backdrop / chrome X).
    void closed();

  protected:
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;

  private:
    void syncGeometryToParent();

    CrashReportPanel* panel_ = nullptr;
};

} // namespace exosnap::ui::dialogs
