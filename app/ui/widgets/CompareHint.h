#pragma once

#include <QToolButton>

namespace exosnap::ui::widgets {

// Settings-Redesign D6: CompareHint — the multi-option sibling of InfoHintIcon.
//
// Renders the same Lucide "info" glyph (18×18, kText3 → kAccent on hover/open).
// Clicking or hovering opens a frameless QFrame popover (~312px wide) that lists
// every option for the bound setting: name, optional "recommended" / version tag,
// and a qualitative effect line. The currently-selected option is accent-tinted
// with a 2px left edge. Clicking an option emits optionSelected(value) and closes
// the panel — the widget doubles as both explainer and picker.
//
// Content comes from SettingsCompareData::compareData(key). If the key is unknown
// the widget renders only the glyph with no popover (safe, no crash).
//
// Usage:
//   auto* hint = new CompareHint(QStringLiteral("container"),
//                                QStringLiteral("MKV"), parent);
//   connect(hint, &CompareHint::optionSelected, this, &MyWidget::onContainerChanged);
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

    [[nodiscard]] QString compareKey() const;
    [[nodiscard]] QString currentValue() const;

  signals:
    // Emitted when the user clicks an option row in the popover.
    void optionSelected(const QString& value);

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
    void updateIcon(bool highlighted);
    void buildPopover();
    void repositionPopover();
    void rebuildRows();

    QString compare_key_;
    QString current_value_;
    QWidget* popover_ = nullptr; // owned, frameless popup
    bool popover_pinned_ = false;
    bool popover_hovered_ = false;
};

} // namespace exosnap::ui::widgets
