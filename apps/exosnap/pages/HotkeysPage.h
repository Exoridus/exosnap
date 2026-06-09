#pragma once
#include <QKeySequence>
#include <QWidget>
#include <array>

#include "services/GlobalHotkeyService.h"

class QHBoxLayout;
class QKeySequenceEdit;
class QLabel;
class QPushButton;
class QVBoxLayout;

namespace exosnap {

class HotkeysPage : public QWidget {
    Q_OBJECT
  public:
    explicit HotkeysPage(QWidget* parent = nullptr);

    // Wire to a live GlobalHotkeyService.
    // After this call the page drives all operations via the service.
    void setService(GlobalHotkeyService* service);

    // Update binding display from an external source (no-service / test mode only).
    void setBindings(const std::array<QKeySequence, 2>& bindings);

    // Lock / unlock editing (e.g. while recording).
    void setEditingLocked(bool locked);

  signals:
    // Emitted in no-service mode only (backward compat for tests without service).
    void bindingChanged(int action_index, QKeySequence seq);

  private:
    static constexpr int kActionCount = 10;
    static constexpr int kActiveActionCount = 2;

    void buildRow(int index, const QString& action, const QKeySequence& default_binding, bool supported,
                  QVBoxLayout* parent_layout, QWidget* parent_widget);
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
        bool supported = false;
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

    std::array<RowWidgets, kActionCount> rows_{};
    int capturing_row_ = -1;
    bool editing_locked_ = false;
    GlobalHotkeyService* service_ = nullptr;
    QPushButton* reset_all_btn_ = nullptr;
};

} // namespace exosnap
