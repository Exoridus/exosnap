#pragma once

#include "services/RecordingClipJobs.h"

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

#include <cstdint>
#include <vector>

namespace exosnap::quick {

class RecordingClipAdapter : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("RecordingClipAdapter is provided by the application")

    Q_PROPERTY(bool canRequest READ canRequest NOTIFY availabilityChanged FINAL)
    Q_PROPERTY(QString unavailableReason READ unavailableReason NOTIFY availabilityChanged FINAL)
    Q_PROPERTY(QVariantList jobs READ jobs NOTIFY jobsChanged FINAL)
    Q_PROPERTY(QVariantList durationOptions READ durationOptions CONSTANT FINAL)
    Q_PROPERTY(QString preferredDurationSeconds READ preferredDurationSeconds NOTIFY preferredDurationChanged FINAL)

  public:
    [[nodiscard]] QVariantList diagnosticResources() const;
    explicit RecordingClipAdapter(QObject* parent = nullptr);

    [[nodiscard]] bool canRequest() const noexcept;
    [[nodiscard]] const QString& unavailableReason() const noexcept;
    [[nodiscard]] const QVariantList& jobs() const noexcept;
    [[nodiscard]] static QVariantList durationOptions();
    [[nodiscard]] QString preferredDurationSeconds() const;

    void setAvailability(bool available, QString reason);
    void setJobs(std::vector<RecordingClipJobSnapshot> snapshots);
    void acceptConfirmedDuration(int64_t seconds);

    Q_INVOKABLE void requestPlan(const QString& duration_seconds);
    Q_INVOKABLE void confirmPlan(const QString& job_id, const QString& generation);
    Q_INVOKABLE void cancelJob(const QString& job_id, const QString& generation);
    Q_INVOKABLE void openCompleted(const QString& asset_id);

  signals:
    void availabilityChanged();
    void jobsChanged();
    void preferredDurationChanged();
    void planRequested(int64_t duration_seconds);
    void confirmRequested(engine::JobId job_id, uint64_t generation);
    void cancelRequested(engine::JobId job_id, uint64_t generation);
    void openCompletedRequested(engine::MediaId asset_id);

  private:
    bool can_request_ = false;
    QString unavailable_reason_;
    std::vector<RecordingClipJobSnapshot> snapshots_;
    QVariantList jobs_;
    int64_t preferred_duration_seconds_ = 30;
};

} // namespace exosnap::quick
