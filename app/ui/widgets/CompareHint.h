#pragma once

#include <QToolButton>

class QTimer;

namespace exosnap::ui::widgets {

// Settings-Redesign D6: CompareHint — the multi-option sibling of InfoHintIcon.
//
// Renders the same Lucide "info" glyph (18×18, kText3 → kAccent on hover/open).
// Hovering or focusing the glyph opens a frameless popover (~312px wide) that lists
// every option for the bound setting: name, optional "recommended" / version tag,
// and a qualitative effect line. The currently-selected option is accent-tinted
// with a 2px left edge so the popover shows which value is active.
//
// The popover is an explainer only — the option rows are non-interactive (no hover
// affordance, no click). The value is changed with the real control (combo box) next
// to the glyph; the popover does not duplicate that picker. The glyph itself keeps
// hover/focus-to-open and click-to-pin.
//
// Content comes from SettingsCompareData::compareData(key). If the key is unknown
// the widget renders only the glyph with no popover (safe, no crash).
//
// Usage:
//   auto* hint = new CompareHint(QStringLiteral("container"),
//                                QStringLiteral("MKV"), parent);
//
// Integration note: call setCurrentValue() whenever the external control changes
// so the marker stays in sync. The integration pass wires this; this widget does
// not subscribe to any external model directly.
class CompareHint : public QToolButton {
    Q_OBJECT
  public:
    explicit CompareHint(const QString& compare_key, const QString& current_value, QWidget* parent = nullptr);

    // Updates the highlighted row without reopening/closing the popover.
    void setCurrentValue(const QString& value);

    [[nodiscard]] const QString& compareKey() const;
    [[nodiscard]] const QString& currentValue() const;

  protected:
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

  private slots:
    void onClicked();
    void showPopover();
    void hidePopover();

  private:
    void applyTheme();
    void updateIcon(bool highlighted);
    void buildPopover();
    void repositionPopover();
    void rebuildRows();

    QString compare_key_;
    QString current_value_;
    QWidget* popover_ = nullptr; // owned, frameless popup
    bool popover_pinned_ = false;
    bool popover_hovered_ = false;
    QTimer* hover_timer_ = nullptr; // polls the cursor while open; hides on true hover-out
};

} // namespace exosnap::ui::widgets
