#pragma once

#include <QFrame>
#include <QString>

class QLabel;
class QObject;
class QEvent;
class QKeyEvent;
class QMouseEvent;
class QPixmap;
class QResizeEvent;

namespace exosnap::ui::widgets {

class CaptureTargetCard : public QFrame {
    Q_OBJECT
  public:
    explicit CaptureTargetCard(QWidget* parent = nullptr);

    void setTitle(const QString& title);
    QString title() const;

    void setSubtitle(const QString& subtitle);
    QString subtitle() const;

    void setStatusText(const QString& status);
    QString statusText() const;

    void setSelected(bool selected);
    bool isSelected() const noexcept;

    void setThumbnail(const QPixmap& pixmap);
    void setThumbnailPlaceholder();
    void setThumbnailLoadingText(const QString& text);
    void setThumbnailFailureText(const QString& text);
    void setThumbnailUnavailableText(const QString& text);
    bool hasThumbnail() const noexcept;

    void setUnavailable(bool unavailable);
    bool isUnavailable() const noexcept;

    void setHelpText(const QString& text);

  signals:
    void clicked();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

  private:
    enum class ThumbnailState {
        Loading,
        Ready,
        Failed,
        Unavailable,
    };

    void setThumbnailState(ThumbnailState state, const QString& text);
    void updateTitleLabel();
    void updateStatusLabel();

    QFrame* thumbnail_surface_ = nullptr;
    QLabel* thumbnail_label_ = nullptr;
    QLabel* thumbnail_state_label_ = nullptr;
    QLabel* title_label_ = nullptr;
    QLabel* selected_chip_label_ = nullptr;
    QLabel* status_label_ = nullptr;
    QLabel* subtitle_label_ = nullptr;
    QLabel* help_label_ = nullptr;
    QString title_text_;
    QString status_text_;
    bool selected_ = false;
    bool click_armed_ = false;
    bool has_thumbnail_ = false;
    bool unavailable_ = false;
};

} // namespace exosnap::ui::widgets
