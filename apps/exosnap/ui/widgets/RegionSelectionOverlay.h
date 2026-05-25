#pragma once

#include <QPoint>
#include <QRect>
#include <QWidget>

namespace exosnap::ui::widgets {

// Transparent full-virtual-desktop overlay for drawing a capture region.
// Shows a dimmed backdrop; user drags to select a rectangle (Snipping-Tool-style).
// Emits regionSelected(QRect) with virtual-screen coordinates on valid release,
// or regionCancelled() on Escape or drag that is too small.
// No injection, no hooks — purely a top-level Qt window.
class RegionSelectionOverlay : public QWidget {
    Q_OBJECT
  public:
    explicit RegionSelectionOverlay(QWidget* parent = nullptr);

    // Cover all screens and activate the overlay for a new selection.
    void activateForSelection();

    static constexpr int kMinDimension = 64;

  signals:
    // region_virtual_screen: rectangle in virtual screen coordinates.
    void regionSelected(QRect region_virtual_screen);
    void regionCancelled();

  protected:
    void showEvent(QShowEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

  private:
    [[nodiscard]] QRect selectionRectLocal() const;

    QPoint drag_start_;
    QPoint drag_current_;
    bool dragging_ = false;
};

} // namespace exosnap::ui::widgets
