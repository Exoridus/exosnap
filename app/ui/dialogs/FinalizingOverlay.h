#pragma once

#include <QWidget>

class QLabel;
class QProgressBar;

namespace exosnap::ui::dialogs {

// Calm, non-dismissible, in-app overlay shown over the Record page while a
// recording is finalizing (container finalize / MP4 remux). Shown for the
// FULL Stopping/Saving duration — not only on a close attempt — replacing the
// native QMessageBox MainWindow::closeEvent previously showed only when the
// user tried to quit mid-finalize. Plain MKV/WebM container finalize is
// normally near-instant (a brief flash); an MP4 remux is visibly longer and
// shows real progress via showSaving(). No buttons, no dismiss action — it
// disappears on its own once the state moves past Saving/Stopping.
//
// Deliberately a page-level sibling overlay (parent-geometry-synced, like
// EditExportOverlay), never a child widget stacked directly on preview_surface_:
// a translucent Qt child there would be occluded by the DXGI native preview
// child HWND while it is active.
class FinalizingOverlay : public QWidget {
    Q_OBJECT
  public:
    explicit FinalizingOverlay(QWidget* parent = nullptr);

    // Stopping: indeterminate progress, "Finishing…" label.
    void showFinalizing();
    // Saving: determinate progress (percent clamped to [0, 100]), "Saving… N%" label.
    void showSaving(int percent);
    void hideOverlay();

  protected:
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;

  private:
    void syncGeometryToParent();
    void applyThemeStyles();

    QLabel* label_ = nullptr;
    QProgressBar* bar_ = nullptr;
};

} // namespace exosnap::ui::dialogs
