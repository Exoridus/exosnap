#pragma once

#include <QLabel>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

class QString;

namespace exosnap::ui::widgets {

// S5: Reusable settings row with a cogwheel popover for collapsing sub-controls.
//
// Layout: [ label ] [ optional info-i ] ……stretch…… [ status text ] [ optional primary control ] [ ⚙ button ]
//
// The ⚙ button opens a Qt::Popup frame anchored below the button that holds any
// sub-controls added via popoverContentLayout(). Callers reparent existing widgets
// into the popover content — the row does NOT own or create the sub-controls.
//
// Usage:
//   auto* row = new SettingsPopoverRow(QStringLiteral("Microphone post-processing"), parent);
//   row->setInfoHint(QStringLiteral("HPF / Gate / AGC / RNNoise"));
//   row->popoverContentLayout()->addWidget(existingCheckBox);
//   row->setStatusText(QStringLiteral("Off"));
class SettingsPopoverRow : public QWidget {
    Q_OBJECT
  public:
    explicit SettingsPopoverRow(const QString& label, QWidget* parent = nullptr);

    // Adds an InfoHintIcon after the row label.
    void setInfoHint(const QString& hint);

    // Optional control placed just left of the ⚙ button (e.g. a master toggle/checkbox).
    // Ownership is taken (widget is reparented to this row).
    void setPrimaryControl(QWidget* w);

    // Optional muted status text shown left of the ⚙ button
    // (e.g. "High-pass · RNNoise" or "Off").
    void setStatusText(const QString& s);

    // Returns the layout of the popover panel. Callers add or reparent existing
    // sub-controls into it.
    [[nodiscard]] QVBoxLayout* popoverContentLayout() const;

  private slots:
    void onCogClicked();

  private:
    QLabel* status_label_ = nullptr;
    QToolButton* cog_btn_ = nullptr;
    QWidget* popover_panel_ = nullptr;
    QVBoxLayout* popover_content_layout_ = nullptr;
};

} // namespace exosnap::ui::widgets
