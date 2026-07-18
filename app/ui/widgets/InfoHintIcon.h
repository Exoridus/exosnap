#pragma once

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
//   - Multi-monitor: the hover anchor is QCursor::pos() (the OS-reported
//     cursor position), not a widget-derived point such as
//     mapToGlobal(rect().center()) — the latter can be computed against a
//     stale per-window DPI/screen association right after the window is
//     dragged to a differently-scaled monitor, landing the tooltip on the
//     wrong display.
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

    QString hint_text_;
    QTimer* hover_timer_ = nullptr; // polls the cursor while the tooltip is open; see class comment
    bool hovered_ = false;
};

} // namespace exosnap::ui::widgets
