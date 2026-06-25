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

// PS-PHASE-C / v10: Compact embedded hotkeys panel for the Settings page.
// Shows the five active (rebindable) hotkeys as single-line rows:
//   [ action label ] ······ ⟨ state-slot ⟩  [×]  [ Set ]
// The state-slot is one fixed-position chip that morphs by state (bound chord /
// "Press keys…" / "Not set" / amber conflict chip). The borderless quiet × clears
// the binding; the bordered primary enters capture (Set/Change) and becomes a
// full-width Cancel while capturing. There is no per-row Reset — the card header
// carries a single "Reset all".
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
    // Repaints the fixed-position state-slot for the given row from its current
    // binding / capture / conflict state.
    void updateSlot(int index);
    void enterCapture(int index);
    void commitCapture(int index, const QKeySequence& seq);
    void cancelCapture(int index);
    void resetAll();
    void showRowConflict(int index, const QKeySequence& attempted, const QString& message);
    void clearRowConflict(int index);
    void refreshRowButtons(int index);

    enum class SlotState { Bound, Unset, Capturing, Conflict };

    struct RowWidgets {
        // The single fixed-position state-slot (holds the chord chip / hint chip).
        QWidget* slot = nullptr;
        QHBoxLayout* slot_layout = nullptr;
        // Right control cluster.
        QWidget* controls = nullptr;
        QPushButton* unset_btn = nullptr;  // borderless quiet ×
        QPushButton* set_btn = nullptr;    // bordered primary (Set / Change)
        QPushButton* cancel_btn = nullptr; // full-width Cancel (capturing)
        // Hidden capture edit (drives the OS key capture).
        QKeySequenceEdit* capture_edit = nullptr;
        QKeySequence current_binding;
        QKeySequence default_binding;
        // Conflict state (set on a rejected TrySetBinding; shown amber in the slot).
        bool conflict = false;
        QKeySequence conflict_binding;
        QString conflict_tooltip;
    };

    std::array<RowWidgets, kActiveActionCount> rows_{};
    int capturing_row_ = -1;
    bool editing_locked_ = false;
    GlobalHotkeyService* service_ = nullptr;
    QPushButton* reset_all_btn_ = nullptr;
};

} // namespace exosnap::ui::widgets
