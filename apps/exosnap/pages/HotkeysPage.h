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

class HotkeysPage : public QWidget {
    Q_OBJECT
  public:
    explicit HotkeysPage(QWidget* parent = nullptr);
    void setBindings(const std::array<QKeySequence, 4>& bindings);

  signals:
    void bindingChanged(int action_index, QKeySequence seq);

  private:
    static constexpr int kActionCount = 4;
    void buildRow(int index, const QString& action, const QKeySequence& default_binding, bool supported,
                  QVBoxLayout* parent_layout, QWidget* parent_widget);
    void updateBindingChips(int index);
    void enterCapture(int index);
    void commitCapture(int index, const QKeySequence& seq);
    void cancelCapture(int index);
    void resetToDefaults();

    struct RowWidgets {
        bool supported = false;
        QWidget* binding_chips = nullptr;
        QHBoxLayout* binding_layout = nullptr;
        QPushButton* set_btn = nullptr;
        QPushButton* unset_btn = nullptr;
        QWidget* normal_container = nullptr;
        QWidget* capture_container = nullptr;
        QKeySequenceEdit* capture_edit = nullptr;
        QKeySequence current_binding;
        QKeySequence default_binding;
    };

    std::array<RowWidgets, kActionCount> rows_{};
    int capturing_row_ = -1;
};

} // namespace exosnap
