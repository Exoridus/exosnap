#pragma once

#include "EditPlayerAdapter.h"
#include "EditSessionAdapter.h"

namespace exosnap::quick {

class RecordingReviewAdapter final : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("RecordingReviewAdapter is provided by the application")
    Q_PROPERTY(bool active READ active NOTIFY activeChanged FINAL)
    Q_PROPERTY(EditSessionAdapter* session READ session CONSTANT FINAL)
    Q_PROPERTY(EditPlayerAdapter* player READ player CONSTANT FINAL)
    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY audioChanged FINAL)
    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY audioChanged FINAL)
    Q_PROPERTY(QString editError READ editError NOTIFY editErrorChanged FINAL)

  public:
    explicit RecordingReviewAdapter(QObject* parent = nullptr);
    void open(const EditContext& context);
    Q_INVOKABLE void close();
    Q_INVOKABLE void suspend();
    Q_INVOKABLE void selectTrack(const QString& id);
    Q_INVOKABLE void requestEdit() {
        if (active_)
            emit editRequested(recording_identity_);
    }
    void completeEditRequest(const QString& identity, bool selected);
    const QString& editError() const {
        return edit_error_;
    }
    bool active() const {
        return active_;
    }
    EditSessionAdapter* session() {
        return &session_;
    }
    EditPlayerAdapter* player() {
        return &player_;
    }
    bool muted() const {
        return muted_;
    }
    double volume() const {
        return volume_;
    }
    void setMuted(bool muted);
    void setVolume(double volume);

  signals:
    void activeChanged();
    void audioChanged();
    void editRequested(QString identity);
    void editErrorChanged();

  private:
    void initializeSelection();
    EditSessionAdapter session_;
    EditPlayerAdapter player_;
    bool active_ = false;
    bool muted_ = false;
    double volume_ = 1.0;
    QStringList initialized_tracks_;
    QString recording_identity_;
    QString edit_error_;
};

} // namespace exosnap::quick
