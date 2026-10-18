#pragma once

#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QtQml/qqmlregistration.h>

namespace exosnap::quick {

class EditInteractionController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickItem* scopeItem READ scopeItem WRITE setScopeItem NOTIFY scopeItemChanged FINAL)
    Q_PROPERTY(bool blocked READ blocked WRITE setBlocked NOTIFY blockedChanged FINAL)
    Q_PROPERTY(bool spaceHeld READ spaceHeld NOTIFY gestureChanged FINAL)
    Q_PROPERTY(bool pointerCaptured READ pointerCaptured NOTIFY gestureChanged FINAL)

  public:
    explicit EditInteractionController(QObject* parent = nullptr);
    ~EditInteractionController() override;
    QQuickItem* scopeItem() const {
        return scope_.data();
    }
    void setScopeItem(QQuickItem* item);
    bool blocked() const {
        return blocked_;
    }
    void setBlocked(bool value);
    bool spaceHeld() const {
        return held_;
    }
    bool pointerCaptured() const {
        return captured_;
    }
    Q_INVOKABLE bool armSpace(int modifiers, bool auto_repeat);
    Q_INVOKABLE void beginPan(QPointF position);
    Q_INVOKABLE void updatePan(QPointF position);
    Q_INVOKABLE void endPan();
    Q_INVOKABLE void cancel();

  signals:
    void scopeItemChanged();
    void blockedChanged();
    void gestureChanged();
    void togglePlayRequested();
    void panRequested(qreal pixels);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void bindWindow();
    bool eligible() const;
    QPointer<QQuickItem> scope_;
    QPointer<QQuickWindow> window_;
    bool blocked_ = false;
    bool held_ = false;
    bool consumed_ = false;
    bool captured_ = false;
    bool panning_ = false;
    QPointF press_;
    QPointF previous_;
};

} // namespace exosnap::quick
