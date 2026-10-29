#pragma once

#include <exosnap/engine/audio_routing_model.h>
#include <exosnap/engine/session_stats.h>

#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

#include <deque>
#include <functional>

namespace exosnap::quick {

class AudioRoutingAdapter : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("AudioRoutingAdapter is provided by the application")

    Q_PROPERTY(QVariantList tracks READ tracks NOTIFY changed FINAL)
    Q_PROPERTY(QVariantList sources READ sources NOTIFY changed FINAL)
    Q_PROPERTY(QVariantList activeTracks READ activeTracks NOTIFY changed FINAL)
    Q_PROPERTY(QVariantList activeSources READ activeSources NOTIFY changed FINAL)
    Q_PROPERTY(qulonglong revision READ revision NOTIFY changed FINAL)
    Q_PROPERTY(bool readOnly READ readOnly NOTIFY changed FINAL)
    Q_PROPERTY(bool legacy READ legacy NOTIFY changed FINAL)
    Q_PROPERTY(bool conversionPending READ conversionPending NOTIFY changed FINAL)
    Q_PROPERTY(QString conversionText READ conversionText CONSTANT FINAL)
    Q_PROPERTY(QString lastError READ lastError NOTIFY changed FINAL)
    Q_PROPERTY(QStringList warnings READ warnings NOTIFY changed FINAL)
    Q_PROPERTY(bool monitorConfirmationRequired READ monitorConfirmationRequired NOTIFY changed FINAL)
    Q_PROPERTY(QStringList defaultMonitorTrackIds READ defaultMonitorTrackIds NOTIFY changed FINAL)
    Q_PROPERTY(QStringList effectiveMonitorTrackIds READ effectiveMonitorTrackIds NOTIFY changed FINAL)
    Q_PROPERTY(QString monitorSelectionOrigin READ monitorSelectionOrigin NOTIFY changed FINAL)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY changed FINAL)
    Q_PROPERTY(QString planLabel READ planLabel NOTIFY changed FINAL)
    Q_PROPERTY(bool recordingActive READ recordingActive NOTIFY changed FINAL)
    Q_PROPERTY(QVariantMap sourceMeters READ sourceMeters NOTIFY metersChanged FINAL)
    Q_PROPERTY(QVariantMap trackMeters READ trackMeters NOTIFY metersChanged FINAL)
    Q_PROPERTY(double minimumGainDb READ minimumGainDb CONSTANT FINAL)
    Q_PROPERTY(double maximumGainDb READ maximumGainDb CONSTANT FINAL)
    Q_PROPERTY(QVariantList sourceCreationOptions READ sourceCreationOptions NOTIFY changed FINAL)
    Q_PROPERTY(QVariantMap sourceDetailDefaults READ sourceDetailDefaults CONSTANT FINAL)

  public:
    enum AssignmentMode { Move, Additional };
    Q_ENUM(AssignmentMode)
    using SaveCallback = std::function<bool(const engine::AudioIntent&, QString&)>;
    using EnvironmentProvider = std::function<engine::AudioEnvironmentSnapshot()>;
    using LiveMuteCallback = std::function<bool(engine::AudioSourceId, bool)>;

    explicit AudioRoutingAdapter(QObject* parent = nullptr);
    void configure(SaveCallback save, EnvironmentProvider environment, LiveMuteCallback liveMute = {});
    void refresh(const engine::AudioIntent& intent, bool readOnly = false, QString validationError = {});
    void setActivePlan(std::optional<engine::ResolvedAudioPlan> plan);
    void setMeters(std::vector<engine::SourceMeterSnapshot> sources, std::vector<engine::TrackMeterSnapshot> tracks,
                   std::vector<engine::PhysicalMeterSnapshot> physical = {});
    [[nodiscard]] QVariantMap resourceDiagnostics() const;

    QVariantList tracks() const;
    QVariantList sources() const;
    QVariantList activeTracks() const;
    QVariantList activeSources() const;
    qulonglong revision() const {
        return revision_;
    }
    bool readOnly() const {
        return read_only_;
    }
    bool legacy() const {
        return std::holds_alternative<engine::LegacyRows25Intent>(intent_);
    }
    bool conversionPending() const {
        return bool(pending_);
    }
    QString conversionText() const;
    const QString& lastError() const {
        return error_;
    }
    QStringList warnings() const;
    bool monitorConfirmationRequired() const;
    QStringList defaultMonitorTrackIds() const;
    QStringList effectiveMonitorTrackIds() const;
    QVariantList sourceCreationOptions() const;
    QVariantMap sourceDetailDefaults() const;
    QString monitorSelectionOrigin() const;
    bool canUndo() const {
        return !undo_.empty();
    }
    QString planLabel() const;
    bool recordingActive() const {
        return active_.has_value();
    }
    QVariantMap sourceMeters() const;
    QVariantMap trackMeters() const;
    double minimumGainDb() const {
        return engine::kMinGainDb;
    }
    double maximumGainDb() const {
        return engine::kMaxGainDb;
    }

    Q_INVOKABLE bool addEmptyTrack(qulonglong expectedRevision);
    Q_INVOKABLE bool removeTrack(const QString& trackId, qulonglong expectedRevision);
    Q_INVOKABLE bool moveTrackTo(const QString& trackId, int position, qulonglong expectedRevision);
    Q_INVOKABLE bool addSource(int bindingKind, const QString& name, qulonglong expectedRevision);
    Q_INVOKABLE bool addSourceToTrack(int bindingKind, const QString& name, const QString& trackId,
                                      qulonglong expectedRevision);
    Q_INVOKABLE bool setTrackName(QString trackId, QString name, qulonglong expectedRevision);
    Q_INVOKABLE bool reorderTrackById(const QString& trackId, int delta, qulonglong expectedRevision);
    Q_INVOKABLE bool assignSource(const QString& sourceId, const QString& trackId, int mode,
                                  qulonglong expectedRevision);
    Q_INVOKABLE bool removeRoute(const QString& routeId, qulonglong expectedRevision);
    Q_INVOKABLE bool setTrackSourceAssigned(const QString& sourceId, const QString& trackId, bool assigned,
                                            qulonglong expectedRevision);
    Q_INVOKABLE bool setRouteGain(QString routeId, double gainDb, qulonglong expectedRevision);
    Q_INVOKABLE bool setRouteMute(QString routeId, bool muted, qulonglong expectedRevision);
    Q_INVOKABLE bool setSourceParticipation(QString sourceId, bool participating, qulonglong expectedRevision);
    Q_INVOKABLE bool setSourceMute(QString sourceId, bool muted, qulonglong expectedRevision);
    Q_INVOKABLE bool setSourceDetails(QString sourceId, QVariantMap details, qulonglong expectedRevision);
    Q_INVOKABLE bool setTrackRecordEnabled(QString trackId, bool enabled, qulonglong expectedRevision);
    Q_INVOKABLE bool setTrackRole(QString trackId, int role, qulonglong expectedRevision);
    Q_INVOKABLE bool setDefaultMonitorSelection(const QStringList& trackIds, qulonglong expectedRevision);
    Q_INVOKABLE bool confirmLegacyConversion(qulonglong expectedRevision);
    Q_INVOKABLE bool cancelLegacyConversion(qulonglong expectedRevision);
    Q_INVOKABLE bool undo(qulonglong expectedRevision);

  signals:
    void changed();
    void metersChanged();

  private:
    using GraphEdit = std::function<bool(engine::TrackGraph1Intent&)>;
    struct PendingEdit {
        GraphEdit edit;
        engine::AudioEnvironmentSnapshot environment;
    };
    bool checkRevision(qulonglong expectedRevision);
    bool fail(QString error);
    bool editGraph(qulonglong expectedRevision, GraphEdit edit);
    bool publish(engine::AudioIntent candidate, const engine::AudioEnvironmentSnapshot& environment,
                 bool remember = true);
    bool patchLegacy(engine::LegacyRowId row, const engine::LegacyRowPatch& patch);
    engine::AudioEnvironmentSnapshot environment() const;
    engine::TrackGraph1Intent presentationGraph() const;
    std::optional<engine::ResolvedAudioPlan> resolvedPlan() const;
    QVariantList sourceRows(const engine::TrackGraph1Intent& graph, bool active) const;
    QVariantList trackRows(const engine::TrackGraph1Intent& graph, bool active) const;
    engine::AudioIntent intent_{engine::TrackGraph1Intent{}};
    SaveCallback save_;
    EnvironmentProvider environment_;
    LiveMuteCallback live_mute_;
    std::optional<engine::ResolvedAudioPlan> active_;
    std::optional<PendingEdit> pending_;
    std::deque<engine::AudioIntent> undo_;
    std::vector<engine::SourceMeterSnapshot> source_meters_;
    std::vector<engine::TrackMeterSnapshot> track_meters_;
    std::vector<engine::PhysicalMeterSnapshot> physical_meters_;
    qulonglong revision_ = 0;
    bool read_only_ = false;
    bool saving_ = false;
    QString error_;
};

} // namespace exosnap::quick
