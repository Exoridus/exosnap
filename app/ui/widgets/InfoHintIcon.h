#pragma once

#include <QPoint>
#include <QToolButton>

class QTimer;

namespace exosnap::ui::widgets {

// SETTINGS-TIERS-R2 Phase 2: reusable per-setting info-hint icon button.
//
// A small flat QToolButton that renders the Lucide "info" glyph in a faint
// color at rest, switching to the Studio Mint accent on hover/focus. Showing
// the tooltip via keyboard focus (not only mouse hover) satisfies the
// accessibility requirement: the button is focusable (Tab-reachable) and
// carries a human-readable accessible name so screen readers announce it.
//
// The tooltip is shown/hidden explicitly (QToolTip::showText/hideText) rather
// than left to Qt's automatic per-motion QEvent::ToolTip dispatch:
//   - Stable anchor: both trigger paths (hover and keyboard focus) anchor the
//     tooltip to a fixed point just below the icon rect via hintAnchor(), NOT to
//     the live cursor. A cursor-anchored tip materialised under the pointer, so it
//     landed in a different spot for every approach direction, jittered as the
//     cursor covered the last pixels onto the glyph, and could be torn down by the
//     very next move of the entering motion — which read to users as "the tooltip
//     only opens when the cursor comes from below". An icon-anchored point removes
//     both the direction dependence and the jitter.
//   - Multi-monitor: hintAnchor() resolves the widget's own screen() explicitly and
//     clamps the point into that screen's available geometry, so a stale per-window
//     DPI/screen association right after the window is dragged to a differently
//     scaled monitor cannot land the tooltip on the wrong display — the same
//     defensive pattern CompareHint::repositionPopover() uses for its popover.
//   - Flicker: a poll timer (mirrors TransportDock::openChevronMenu /
//     CompareHint's hover_timer_) closes the tooltip only once the cursor has
//     settled truly outside a small hysteresis band around the icon, instead
//     of reacting to the bare Enter/Leave pair — cursor jitter of a couple of
//     physical pixels at the icon's edge crosses the widget boundary
//     repeatedly, and a leave-driven hide flickers the tooltip open/closed on
//     every crossing.
//
// Usage:
//   auto* hint = new InfoHintIcon(QStringLiteral("MKV safest · MP4 most compatible"), parent);
//   layout->addWidget(hint);
//
// Constructor: text = the hint string (verbatim from info-hints-content.md).
class InfoHintIcon : public QToolButton {
    Q_OBJECT
  public:
    explicit InfoHintIcon(const QString& hint_text, QWidget* parent = nullptr);

    // The hint string (the tooltip text).
    [[nodiscard]] const QString& hintText() const;

  protected:
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

  private:
    void updateIcon(bool highlighted);
    void hideHintTooltip();
    // Fixed global point just below the icon that both trigger paths anchor the
    // tooltip to; resolves and clamps against the widget's own screen.
    [[nodiscard]] QPoint hintAnchor() const;

    QString hint_text_;
    QTimer* hover_timer_ = nullptr; // polls the cursor while the tooltip is open; see class comment
    bool hovered_ = false;
};

} // namespace exosnap::ui::widgets
