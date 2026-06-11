#pragma once

#include <QWidget>

class QFrame;
class QLabel;

namespace exosnap::ui::widgets {

class SectionRuleHeader : public QWidget {
    Q_OBJECT
  public:
    explicit SectionRuleHeader(const QString& title = QString(), QWidget* parent = nullptr);

    void setTitle(const QString& title);
    void setMeta(const QString& meta);
    void clearMeta();

    QString title() const;
    QString meta() const;

  private:
    QLabel* title_label_ = nullptr;
    QFrame* rule_line_ = nullptr;
    QLabel* meta_label_ = nullptr;
};

} // namespace exosnap::ui::widgets
