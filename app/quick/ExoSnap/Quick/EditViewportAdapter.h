#pragma once

#include <QObject>
#include <QtQml/qqmlregistration.h>
#include <optional>

namespace exosnap::quick {

class EditViewportMapping {
    Q_GADGET
    QML_VALUE_TYPE(editViewportMapping)

  public:
    EditViewportMapping() = default;
    EditViewportMapping(qint64 origin, qint64 end, qreal scale) : origin_(origin), end_(end), scale_(scale) {
    }
    Q_INVOKABLE qint64 timeAtX(qreal x) const;
    Q_INVOKABLE qreal xAtTime(qint64 time) const;

  private:
    qint64 origin_ = 0;
    qint64 end_ = 0;
    qreal scale_ = 40;
};

class EditViewportAdapter : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(qint64 timelineEndUs READ timelineEndUs WRITE setTimelineEndUs NOTIFY viewChanged FINAL)
    Q_PROPERTY(qreal usableWidth READ usableWidth WRITE setUsableWidth NOTIFY viewChanged FINAL)
    Q_PROPERTY(qint64 startUs READ startUs NOTIFY viewChanged FINAL)
    Q_PROPERTY(qint64 endUs READ endUs NOTIFY viewChanged FINAL)
    Q_PROPERTY(qreal pixelsPerSecond READ pixelsPerSecond WRITE setPixelsPerSecond NOTIFY viewChanged FINAL)
    Q_PROPERTY(qreal minimumZoom READ minimumScale NOTIFY viewChanged FINAL)
    Q_PROPERTY(qreal maximumZoom READ maximumScale NOTIFY viewChanged FINAL)
    Q_PROPERTY(exosnap::quick::EditViewportMapping mapping READ mapping NOTIFY viewChanged FINAL)

  public:
    explicit EditViewportAdapter(QObject* parent = nullptr);
    qint64 timelineEndUs() const {
        return end_;
    }
    qreal usableWidth() const {
        return width_;
    }
    qint64 startUs() const {
        return view_.start;
    }
    qint64 endUs() const;
    qreal pixelsPerSecond() const {
        return view_.scale;
    }
    EditViewportMapping mapping() const {
        return {view_.start, end_, view_.scale};
    }
    void setTimelineEndUs(qint64 value);
    void setUsableWidth(qreal value);
    void setPixelsPerSecond(qreal value);
    Q_INVOKABLE qint64 timeAtX(qreal x) const;
    Q_INVOKABLE qreal xAtTime(qint64 time) const;
    Q_INVOKABLE void setVisibleInterval(qint64 from, qint64 to);
    Q_INVOKABLE void zoomAt(qreal scale, qreal anchor_x);
    // Positive movement reveals later timeline positions.
    Q_INVOKABLE void panPixels(qreal delta);
    Q_INVOKABLE void fitTimeline();
    Q_INVOKABLE void fitOccupied(qint64 from, qint64 to);
    Q_INVOKABLE void fitSelection(qint64 from, qint64 to);
    Q_INVOKABLE void restorePreviousView();

  signals:
    void viewChanged();

  private:
    qint64 end_ = 0;
    qreal width_ = 1;
    enum class Fit { Free, Timeline, Interval };
    struct View {
        qint64 start = 0;
        qreal scale = 40;
        Fit fit = Fit::Free;
        qint64 from = 0;
        qint64 to = 0;
    };
    View view_;
    std::optional<View> previous_;
    qreal minimumScale() const;
    qreal boundedScale(qreal scale) const;
    qreal maximumScale() const;
    void normalize();
    void fitInterval(qint64 from, qint64 to);
    void publishView(const View& before);
};

} // namespace exosnap::quick

Q_DECLARE_METATYPE(exosnap::quick::EditViewportMapping)
