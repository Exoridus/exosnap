#pragma once

#include <QToolButton>

namespace exosnap::ui::widgets {

// SETTINGS-TIERS-R2 Phase 2: reusable per-setting info-hint icon button.
//
// A small flat QToolButton that renders the Lucide "info" glyph in a faint
// color at rest, switching to the Studio Mint accent on hover/focus. Showing
// the tooltip via keyboard focus (not only mouse hover) satisfies the
// accessibility requirement: the button is focusable (Tab-reachable) and
// carries a human-readable accessible name so screen readers announce it.
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

    QString hint_text_;
};

} // namespace exosnap::ui::widgets
