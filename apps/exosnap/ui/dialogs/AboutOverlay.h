#pragma once
#include <QWidget>

class QFrame;
class QEvent;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QShowEvent;

namespace exosnap::ui::dialogs {

// In-window About surface. Renders as a translucent backdrop with a centered
// About card *inside* the application window — there is no separate native OS
// dialog, so nothing can linger as a focus-sticky child window after closing.
//
// Parent it to the host whose client area it should cover (typically the
// central widget); it tracks the parent's geometry and fills it. Open with
// openOverlay(); it dismisses on Escape, on a backdrop click, or via the Close
// action, emitting closed() each time it is dismissed.
class AboutOverlay : public QWidget {
    Q_OBJECT
  public:
    explicit AboutOverlay(QWidget* parent = nullptr);

    void openOverlay();
    void closeOverlay();
    bool isOpen() const noexcept;

  signals:
    void closed();

  protected:
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;

  private:
    void syncGeometryToParent();
    QFrame* buildCard();

    QFrame* card_ = nullptr;
};

} // namespace exosnap::ui::dialogs
