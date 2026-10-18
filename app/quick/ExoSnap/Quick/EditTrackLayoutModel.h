#pragma once

#include <QHash>
#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

namespace exosnap::quick {

class EditTrackLayoutModel : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QVariantList tracks READ tracks WRITE setTracks NOTIFY layoutChanged FINAL)
    Q_PROPERTY(QVariantList previewTracks READ previewTracks WRITE setPreviewTracks NOTIFY layoutChanged FINAL)
    Q_PROPERTY(QVariantList rows READ rows NOTIFY layoutChanged FINAL)
    Q_PROPERTY(QVariantMap geometry READ geometry NOTIFY layoutChanged FINAL)
    Q_PROPERTY(QVariantMap previewGeometry READ previewGeometry NOTIFY layoutChanged FINAL)
    Q_PROPERTY(qreal totalHeight READ totalHeight NOTIFY layoutChanged FINAL)
    Q_PROPERTY(qreal baseHeight READ baseHeight NOTIFY layoutChanged FINAL)
    Q_PROPERTY(qreal headerHeight READ headerHeight CONSTANT FINAL)
    Q_PROPERTY(qreal minimumHeight READ minimumHeight CONSTANT FINAL)
    Q_PROPERTY(qreal maximumHeight READ maximumHeight CONSTANT FINAL)

  public:
    explicit EditTrackLayoutModel(QObject* parent = nullptr);
    QVariantList tracks() const {
        return tracks_;
    }
    QVariantList previewTracks() const {
        return preview_tracks_;
    }
    QVariantList rows() const {
        return rows_;
    }
    QVariantMap geometry() const {
        return geometry_;
    }
    QVariantMap previewGeometry() const {
        return preview_tracks_.empty() ? geometry_ : preview_geometry_;
    }
    qreal totalHeight() const {
        return total_height_;
    }
    qreal baseHeight() const {
        return base_height_;
    }
    qreal headerHeight() const {
        return 18;
    }
    qreal minimumHeight() const {
        return 28;
    }
    qreal maximumHeight() const {
        return 240;
    }
    void setTracks(const QVariantList& tracks);
    void setPreviewTracks(const QVariantList& tracks);
    void reset();
    Q_INVOKABLE void setTrackHeight(qulonglong id, qreal height);
    Q_INVOKABLE qreal topForId(qulonglong id, bool preview = false) const;
    Q_INVOKABLE qreal heightForId(qulonglong id, bool preview = false) const;
    Q_INVOKABLE qulonglong trackAtY(qreal y) const;
    Q_INVOKABLE QVariantMap dropTargetAtY(qreal y) const;

  signals:
    void layoutChanged();

  private:
    QVariantList tracks_;
    QVariantList preview_tracks_;
    QVariantList rows_;
    QVariantMap geometry_;
    QVariantMap preview_geometry_;
    QHash<qulonglong, qreal> heights_;
    qreal base_height_ = 0;
    qreal total_height_ = 0;
    void rebuild();
    QVariantList layoutRows(const QVariantList& tracks) const;
    static QVariantMap geometryFor(const QVariantList& rows);
    static bool validTracks(const QVariantList& tracks);
};

} // namespace exosnap::quick
