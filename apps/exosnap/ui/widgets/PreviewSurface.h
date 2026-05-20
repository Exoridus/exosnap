#pragma once

#include <QWidget>

class QLabel;

namespace exosnap::ui::widgets {

class StatusPill;

class PreviewSurface : public QWidget {
    Q_OBJECT
  public:
    explicit PreviewSurface(QWidget* parent = nullptr);

    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    void setRecording(bool recording);
    bool isRecording() const noexcept;

    void setStatusText(const QString& text);
    void setTopMetaText(const QString& text);
    void setCenterTitle(const QString& text);
    void setCenterSubtitle(const QString& text);
    void setBottomLeftText(const QString& text);
    void setBottomRightText(const QString& text);

    StatusPill* statusPill() const noexcept;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    StatusPill* status_pill_ = nullptr;
    QLabel* top_meta_label_ = nullptr;
    QLabel* center_title_label_ = nullptr;
    QLabel* center_subtitle_label_ = nullptr;
    QLabel* bottom_left_label_ = nullptr;
    QLabel* bottom_right_label_ = nullptr;
    QWidget* top_row_ = nullptr;
    QWidget* center_box_ = nullptr;
    QWidget* bottom_row_ = nullptr;
    bool recording_ = false;
};

} // namespace exosnap::ui::widgets
