#pragma once

#include <QFrame>
#include <QString>

class QVBoxLayout;
class QPushButton;
class QWidget;

namespace exosnap::ui::widgets {

// SETTINGS-TIERS-R1: collapsible Advanced expander for a Settings card.
// Usage: create an expander, add rows to contentWidget(), then add the expander
// to the card layout.
// The header label shows "Advanced · N options" (N = child row count).
class SettingsCardExpander : public QFrame {
    Q_OBJECT
  public:
    explicit SettingsCardExpander(int option_count, QWidget* parent = nullptr);

    // The widget whose layout you add Advanced rows into.
    [[nodiscard]] QWidget* contentWidget() const noexcept;

    // Expanded state (persisted by ConfigPage via AppSettingsStore).
    void setExpanded(bool expanded);
    [[nodiscard]] bool isExpanded() const noexcept;

  signals:
    void expandedChanged(bool expanded);

  private:
    void toggle();

    QPushButton* header_btn_ = nullptr;
    QWidget* content_widget_ = nullptr;
    bool expanded_ = false;
};

} // namespace exosnap::ui::widgets
