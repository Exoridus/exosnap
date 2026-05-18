#pragma once
#include <QKeySequence>
#include <QWidget>
#include <array>

class QKeySequenceEdit;
class QLabel;
class QPushButton;
class QVBoxLayout;

namespace exosnap {

class HotkeysPage : public QWidget {
    Q_OBJECT
  public:
    explicit HotkeysPage(QWidget* parent = nullptr);

  private:
    void buildRow(int index, const QString& action, const QKeySequence& default_binding, QVBoxLayout* parent_layout,
                  QWidget* parent_widget);
    void enterCapture(int index);
    void commitCapture(int index, const QKeySequence& seq);
    void cancelCapture(int index);

    struct RowWidgets {
        QLabel* binding_label = nullptr;
        QPushButton* set_btn = nullptr;
        QPushButton* unset_btn = nullptr;
        QWidget* capture_container = nullptr;
        QKeySequenceEdit* capture_edit = nullptr;
        QKeySequence current_binding;
    };

    std::array<RowWidgets, 7> rows_{};
    int capturing_row_ = -1;
};

} // namespace exosnap
