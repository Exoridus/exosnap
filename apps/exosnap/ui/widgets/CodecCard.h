#pragma once

#include <QWidget>

class QLabel;
class QMouseEvent;

namespace exosnap::ui::widgets {

class CodecCard : public QWidget {
    Q_OBJECT
  public:
    explicit CodecCard(const QString& name, const QString& tag, const QString& description, QWidget* parent = nullptr);

    void setSelected(bool selected);
    [[nodiscard]] bool isSelected() const noexcept;

  signals:
    void clicked();

  protected:
    void mousePressEvent(QMouseEvent* event) override;

  private:
    QLabel* name_label_ = nullptr;
    QLabel* tag_label_ = nullptr;
    QLabel* description_label_ = nullptr;
    bool selected_ = false;
};

} // namespace exosnap::ui::widgets
