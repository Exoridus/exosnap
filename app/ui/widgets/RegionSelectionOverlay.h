#pragma once

#include <QPoint>
#include <QRect>
#include <QString>
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
//
// Visual spec (REGION-SELECTION-SKIN-R1, Mappe wave03 RegionMock):
//   Selection rect: 1.5px border, accent #9BD9D2, corner radius 4.
//   Scrim: rgba(8,8,10,0.5) over everything outside the selection.
//   Corner handles: 13×13px squares, radius 4, accent fill, 2px bg-color border.
//   Readout: "W × H · ratio" in IBM Plex Mono 11px above top-left of rect.
//   Actions: Confirm (accent pill) and Esc (dark pill) below-right of rect.
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

    // Aspect ratio hint reported when the selection is near a common preset.
    struct AspectHint {
        bool active = false;
        QString label; // e.g. "16:9"
    };

    explicit RegionSelectionOverlay(QWidget* parent = nullptr);

    // Cover all screens and activate the overlay. monitor_virtual_screen is the
    // selected display in virtual-screen coordinates; initial_region_virtual is
    // optional and enables editable move/resize before explicit confirmation.
    void activateForSelection(QRect monitor_virtual_screen = QRect(), QRect initial_region_virtual = QRect());
    void cancelSelection();

    // Pure helper: format a size as "W × H · ratio".
    // Exposed for unit testing.
    [[nodiscard]] static QString formatReadout(int width, int height);

    // Pure helper: snap a width/height pair towards the nearest common preset
    // ratio when within snap_threshold_pct % of that ratio. Returns the snapped
    // rect (same area, adjusted width) or the original if no snap applies.
    // Exposed for unit testing.
    [[nodiscard]] static QRect snapToPresetAspect(const QRect& sel, const QRect& monitor, int snap_threshold_pct = 5);

    // Pure helper: return the nearest preset ratio label for a size, or
    // an empty string if none is close enough.
    [[nodiscard]] static QString nearestPresetLabel(int width, int height, int threshold_pct = 5);

    static constexpr int kMinDimension = 64;

    // Design-spec accent color (Studio Mint) — used when no QSS palette is available.
    static constexpr QRgb kAccentRgb = 0xFF9BD9D2;
    // Ink-on-accent (dark) for the Confirm pill text.
    static constexpr QRgb kAccentInkRgb = 0xFF08130F;
    // Overlay background used for scrim / readout / Esc pill.
    static constexpr QRgb kBgRgb = 0xFF0E0E10;

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

    // Paint helpers (called from paintEvent).
    void paintScrimAndSelection(QPainter& p, const QRect& sel) const;
    void paintCornerHandles(QPainter& p, const QRect& sel) const;
    void paintReadout(QPainter& p, const QRect& sel) const;
    void paintInstruction(QPainter& p) const;

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
