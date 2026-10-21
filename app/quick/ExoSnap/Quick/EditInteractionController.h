#pragma once

#include "models/EditPointerGeometry.h"

#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

namespace exosnap::quick {

class EditTimelineClipModel;

[[nodiscard]] QVariantMap ResolveTimelineWheel(int modifiers, QPointF pixels, QPointF angles, bool space_reserved);
[[nodiscard]] QString ResolveTimelineKey(int key, int modifiers, bool auto_repeat);

class EditInteractionController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_MOC_INCLUDE("EditTimelineModels.h")
    Q_PROPERTY(QQuickItem* scopeItem READ scopeItem WRITE setScopeItem NOTIFY scopeItemChanged FINAL)
    Q_PROPERTY(bool blocked READ blocked WRITE setBlocked NOTIFY blockedChanged FINAL)
    Q_PROPERTY(bool spaceHeld READ spaceHeld NOTIFY gestureChanged FINAL)
    Q_PROPERTY(bool pointerCaptured READ pointerCaptured NOTIFY gestureChanged FINAL)
    Q_PROPERTY(bool panActive READ panActive NOTIFY gestureChanged FINAL)

  public:
    enum PointerTarget {
        PointerNone = static_cast<int>(edit::ClipPointerTarget::None),
        PointerHeader = static_cast<int>(edit::ClipPointerTarget::Header),
        PointerContent = static_cast<int>(edit::ClipPointerTarget::Content),
        PointerTrimStart = static_cast<int>(edit::ClipPointerTarget::TrimStart),
        PointerTrimEnd = static_cast<int>(edit::ClipPointerTarget::TrimEnd),
        PointerFadeIn = static_cast<int>(edit::ClipPointerTarget::FadeIn),
        PointerFadeOut = static_cast<int>(edit::ClipPointerTarget::FadeOut),
        PointerGain = static_cast<int>(edit::ClipPointerTarget::Gain),
        PointerJunction = static_cast<int>(edit::ClipPointerTarget::Junction)
    };
    Q_ENUM(PointerTarget)
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
    bool panActive() const {
        return panning_;
    }
    Q_INVOKABLE bool armSpace(int modifiers, bool auto_repeat);
    Q_INVOKABLE void beginPan(QPointF position, bool emit_pixels = true);
    Q_INVOKABLE void updatePan(QPointF position);
    Q_INVOKABLE void endPan();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE QVariantMap handleWheel(int modifiers, QPointF pixels, QPointF angles, int phase);
    Q_INVOKABLE QString keyAction(int key, int modifiers, bool auto_repeat) const;
    Q_INVOKABLE int clipPointerTarget(const QVariantMap& row, qreal width, qreal height, qreal header_height, qreal x,
                                      qreal y, bool junction = false) const;
    Q_INVOKABLE bool clipPointerThreshold(int target, qreal dx, qreal dy, qreal threshold) const;
    Q_INVOKABLE qreal clipGainLineY(const QVariantMap& row, qreal height, qreal header_height) const;
    Q_INVOKABLE QVariantMap clipPointerPolicy(int target, int modifiers, bool has_range) const;
    Q_INVOKABLE qulonglong dragTrackId(const QVariantList& rows, qulonglong previous_id, qreal pointer_y) const;
    Q_INVOKABLE QPointF edgeAutoScroll(QPointF pointer, QSizeF extent) const;
    Q_INVOKABLE QVariantList fadeEnvelopePoints(int curve, qreal bias, bool outgoing = false, qreal from = 0,
                                                qreal to = 1) const;
    Q_INVOKABLE qulonglong junctionAt(const QVariantList& rows, qulonglong clip_id, qint64 at_us) const;
    Q_INVOKABLE qulonglong modelJunctionAt(EditTimelineClipModel* model, qulonglong clip_id, qint64 at_us) const;
    Q_INVOKABLE int junctionHandleAt(qreal x, qreal start, qreal end, int last_edge = -1, qreal hit = 6) const;
    Q_INVOKABLE bool junctionPointerEligible(const QVariantMap& row, qreal pixels_per_us, qint64 at_us, qreal y,
                                             qreal height, qreal header_height) const;
    Q_INVOKABLE qulonglong resolveNarrowClip(EditTimelineClipModel* model, qulonglong track_id, qint64 at_us,
                                             qreal pixels_per_us);

  signals:
    void scopeItemChanged();
    void blockedChanged();
    void gestureChanged();
    void togglePlayRequested();
    void rangeOnceRequested();
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
    bool emit_pan_pixels_ = true;
    QPointF press_;
    QPointF previous_;
    QList<qulonglong> narrow_candidates_;
    qulonglong narrow_track_ = 0;
    qulonglong narrow_last_ = 0;
    qint64 narrow_time_ = 0;
    qreal narrow_scale_ = 0;
};

} // namespace exosnap::quick
