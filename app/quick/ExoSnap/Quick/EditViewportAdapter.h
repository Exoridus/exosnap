#pragma once

#include <QObject>
#include <QTimer>
#include <QtQml/qqmlregistration.h>
#include <optional>
#include <vector>

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
    Q_PROPERTY(qint64 navigationEndUs READ navigationEndUs NOTIFY viewChanged FINAL)
    Q_PROPERTY(qint64 fitPaddingUs READ fitPaddingUs WRITE setFitPaddingUs NOTIFY viewChanged FINAL)
    Q_PROPERTY(int historyDepth READ historyDepth NOTIFY viewChanged FINAL)
    Q_PROPERTY(int overviewTarget READ overviewTarget NOTIFY viewChanged FINAL)
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
    qint64 navigationEndUs() const;
    qint64 fitPaddingUs() const {
        return fit_padding_;
    }
    int historyDepth() const {
        return static_cast<int>(history_.size());
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
        return {view_.start, navigationEndUs(), view_.scale};
    }
    void setTimelineEndUs(qint64 value);
    void setUsableWidth(qreal value);
    void setPixelsPerSecond(qreal value);
    void setFitPaddingUs(qint64 value);
    Q_INVOKABLE qint64 timeAtX(qreal x) const;
    Q_INVOKABLE qreal xAtTime(qint64 time) const;
    Q_INVOKABLE void setVisibleInterval(qint64 from, qint64 to);
    Q_INVOKABLE void zoomAt(qreal scale, qreal anchor_x);
    Q_INVOKABLE void zoomWheelAt(qreal scale, qreal anchor_x);
    Q_INVOKABLE void beginViewOperation();
    Q_INVOKABLE void endViewOperation();
    // Positive movement reveals later timeline positions.
    Q_INVOKABLE void panPixels(qreal delta);
    Q_INVOKABLE void fitTimeline();
    Q_INVOKABLE void fitOccupied(qint64 from, qint64 to);
    Q_INVOKABLE void fitSelection(qint64 from, qint64 to);
    Q_INVOKABLE void restorePreviousView();
    int overviewTarget() const {
        return overview_target_;
    }
    Q_INVOKABLE bool beginOverviewOperation(qreal x, qreal width, qreal edge_hit = 6, bool body_only = false);
    Q_INVOKABLE void updateOverviewOperation(qreal x, qreal width);
    Q_INVOKABLE void endOverviewOperation();
    Q_INVOKABLE void centerOverviewAt(qreal x, qreal width);

  signals:
    void viewChanged();

  private:
    qint64 end_ = 0;
    qint64 fit_padding_ = 16667;
    qreal width_ = 1;
    struct View {
        qint64 start = 0;
        qreal scale = 40;
    };
    struct Interval {
        qint64 from = 0;
        qint64 to = 0;
        bool operator==(const Interval&) const = default;
    };
    View view_;
    std::vector<Interval> history_;
    std::optional<Interval> operation_before_;
    QTimer wheel_timer_;
    bool wheel_operation_ = false;
    bool operation_zoomed_ = false;
    bool initialized_ = false;
    int overview_target_ = 0;
    int overview_last_edge_ = 2;
    qreal overview_press_fraction_ = 0;
    qreal overview_scale_ = 40;
    Interval overview_before_{};
    qreal minimumScale() const;
    qreal boundedScale(qreal scale) const;
    qreal maximumScale() const;
    void normalize();
    void fitInterval(qint64 from, qint64 to, bool padding);
    void publishView(const View& before, bool history);
    Interval interval(const View& view) const;
    void pushHistory(Interval before);
    void finishWheelOperation();
};

} // namespace exosnap::quick

Q_DECLARE_METATYPE(exosnap::quick::EditViewportMapping)
