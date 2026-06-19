#pragma once
#include <QKeySequence>
#include <QWidget>
#include <array>

class QHBoxLayout;
class QKeySequenceEdit;
class QLabel;
class QPushButton;
class QVBoxLayout;

namespace exosnap {
class GlobalHotkeyService;
} // namespace exosnap

namespace exosnap::ui::widgets {

// PS-PHASE-C: Compact embedded hotkeys panel for the Settings page.
// Shows the five active (rebindable) hotkeys without page chrome, scroll area,
// or planned-actions section.  Designed to be hosted inside a Settings card.
class HotkeysSettingsPanel : public QWidget {
    Q_OBJECT
  public:
    explicit HotkeysSettingsPanel(QWidget* parent = nullptr);

    // Wire to a live GlobalHotkeyService for rebind/conflict/persist.
    void setService(GlobalHotkeyService* service);

    // Lock / unlock editing while recording is active.
    void setEditingLocked(bool locked);

  private:
    static constexpr int kActiveActionCount = 5;

    void buildRow(int index, const QString& action, const QKeySequence& default_binding, QVBoxLayout* parent_layout,
                  QWidget* parent_widget);
    void updateBindingChips(int index);
    void enterCapture(int index);
    void commitCapture(int index, const QKeySequence& seq);
    void cancelCapture(int index);
    void resetAll();
    void resetRow(int index);
    void showRowError(int index, const QString& message);
    void clearRowError(int index);
    void refreshRowButtons(int index);

    struct RowWidgets {
        QWidget* binding_chips = nullptr;
        QHBoxLayout* binding_layout = nullptr;
        QPushButton* set_btn = nullptr;
        QPushButton* unset_btn = nullptr;
        QPushButton* reset_btn = nullptr;
        QLabel* error_label = nullptr;
        QWidget* normal_container = nullptr;
        QWidget* capture_container = nullptr;
        QKeySequenceEdit* capture_edit = nullptr;
        QKeySequence current_binding;
        QKeySequence default_binding;
    };

    std::array<RowWidgets, kActiveActionCount> rows_{};
    int capturing_row_ = -1;
    bool editing_locked_ = false;
    GlobalHotkeyService* service_ = nullptr;
    QPushButton* reset_all_btn_ = nullptr;
};

} // namespace exosnap::ui::widgets
