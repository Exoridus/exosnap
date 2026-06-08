#pragma once

#include <QPoint>
#include <QRect>
#include <QWidget>

#include "RegionGeometry.h"

class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QPushButton;
class QShowEvent;

namespace exosnap::ui::widgets {

// Transparent full-virtual-desktop overlay for drawing a capture region.
// Shows a dimmed backdrop; user drags to select a rectangle (Snipping-Tool-style).
// Emits regionSelected(QRect) with virtual-screen coordinates on valid release,
// or regionCancelled() on Escape or drag that is too small.
// No injection, no hooks — purely a top-level Qt window.
class RegionSelectionOverlay : public QWidget {
    Q_OBJECT
  public:
    enum class InteractionMode {
        None,
        Selecting,
        Moving,
        ResizingTopLeft,
        ResizingTopRight,
        ResizingBottomLeft,
        ResizingBottomRight,
    };

    explicit RegionSelectionOverlay(QWidget* parent = nullptr);

    // Cover all screens and activate the overlay. monitor_virtual_screen is the
    // selected display in virtual-screen coordinates; initial_region_virtual is
    // optional and enables editable move/resize before explicit confirmation.
    void activateForSelection(QRect monitor_virtual_screen = QRect(), QRect initial_region_virtual = QRect());
    void cancelSelection();

    static constexpr int kMinDimension = 64;

  signals:
    // region_virtual_screen: rectangle in virtual screen coordinates.
    void regionSelected(QRect region_virtual_screen);
    void regionCancelled();
    void interactionModeChanged(InteractionMode mode);

  protected:
    void showEvent(QShowEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

  private:
    [[nodiscard]] QRect selectionRectLocal() const;
    [[nodiscard]] QRect monitorRectLocal() const;
    [[nodiscard]] QRect toLocal(const QRect& virtual_screen_rect) const;
    [[nodiscard]] QRect toVirtual(const QRect& local_rect) const;
    [[nodiscard]] RegionResizeHandle hitTestHandle(const QPoint& pos) const;
    [[nodiscard]] bool hitTestMove(const QPoint& pos) const;
    void setOverlayInteraction(InteractionMode mode);
    void updateCursorForPosition(const QPoint& pos);
    void updateActionGeometry();
    void confirmSelection();

    QPoint drag_start_;
    QPoint drag_current_;
    QPoint drag_last_;
    QRect selection_rect_;
    QRect monitor_rect_;
    QRect initial_region_;
    QRect drag_origin_rect_;
    RegionResizeHandle active_handle_ = RegionResizeHandle::None;
    InteractionMode interaction_mode_ = InteractionMode::None;
    QPushButton* confirm_button_ = nullptr;
    QPushButton* cancel_button_ = nullptr;
    bool dragging_ = false;
    bool editing_existing_ = false;
};

} // namespace exosnap::ui::widgets
