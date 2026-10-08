// Test runner for the Edit surface's keyboard contract (QCR-504).
//
// A separate executable from record_controls_qml_tests because EditTimeline.qml
// takes three REQUIRED, C++-typed adapters. They cannot be stubbed from QML and
// they cannot be seeded from QML either — `setEditContext` is a plain C++ entry
// point, not Q_INVOKABLE, and making it one to suit a test would widen the
// production API. So the adapters are built here, seeded with the same
// no-master-path fixture context `test_edit_adapters.cpp` uses (nothing is
// opened, decoded or remuxed), and handed to QML as context properties.

#include "EditExportAdapter.h"
#include "EditPlayerAdapter.h"
#include "EditPlayerWorker.h"
#include "EditSessionAdapter.h"
#include "EditTimelineAdapter.h"
#include "ExoEditPlayerItem.h"
#include "RecordViewModelAdapter.h"

#include "viewmodels/RecordViewModel.h"

#include <QCoreApplication>
#include <QObject>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QtQuickTest>

using exosnap::EditContext;
using exosnap::quick::EditPlayerAdapter;
using exosnap::quick::EditSessionAdapter;
using exosnap::quick::EditTimelineAdapter;

namespace {

// 100 s, and no `mkv_master_path`: the session reports a duration and accepts
// trims without a decoder ever being asked for anything.
EditContext FixtureContext() {
    EditContext context;
    context.output_path = QStringLiteral("D:/Recordings/clip.mkv");
    context.duration = QStringLiteral("1:40");
    context.size = QStringLiteral("120 MB");
    context.resolution = QStringLiteral("1920x1080");
    context.fps = QStringLiteral("60 fps CFR");
    context.video_codec = QStringLiteral("AV1 (NVENC)");
    context.audio_codec = QStringLiteral("Opus");
    context.container = QStringLiteral("MKV");
    context.duration_seconds = 100.0;
    return context;
}

} // namespace

class MediaPreviewDriver final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(QString error MEMBER error_ NOTIFY changed)
    Q_PROPERTY(bool ended MEMBER ended_ NOTIFY changed)
  public:
    explicit MediaPreviewDriver(QObject* parent) : QObject(parent) {
        sink_->attach(&item_);
        worker_ = new exosnap::quick::EditPlayerWorker(sink_);
        worker_->moveToThread(&thread_);
        connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
        connect(worker_, &exosnap::quick::EditPlayerWorker::openFinished, this, [this](bool ok, const QString& error) {
            if (!ok)
                error_ = error;
            emit changed();
        });
        connect(worker_, &exosnap::quick::EditPlayerWorker::reachedEnd, this, [this] {
            ended_ = true;
            emit changed();
        });
        thread_.start();
    }
    ~MediaPreviewDriver() override {
        QMetaObject::invokeMethod(worker_, &exosnap::quick::EditPlayerWorker::close, Qt::BlockingQueuedConnection);
        thread_.quit();
        thread_.wait();
        sink_->detach(&item_);
    }
    bool available() const {
        return !qEnvironmentVariable("EXOSNAP_EDIT_RENDER_FIXTURES").isEmpty();
    }
    Q_INVOKABLE void open() {
        exosnap::edit::Workspace workspace;
        for (const auto* name : {"red.mkv", "blue.mkv"}) {
            exosnap::edit::Asset asset;
            asset.path = (qEnvironmentVariable("EXOSNAP_EDIT_RENDER_FIXTURES") + "/" + name).toStdWString();
            asset.duration = 2'000'000;
            workspace.insert(workspace.addAsset(std::move(asset)));
        }
        if (!workspace.crossfade(workspace.clips()[0].id, workspace.clips()[1].id)) {
            error_ = QStringLiteral("Could not construct Crossfade fixture.");
            emit changed();
            return;
        }
        const auto generation = sink_->invalidate();
        QMetaObject::invokeMethod(worker_, [worker = worker_, workspace, generation] {
            worker->setVolume(0);
            worker->setRequestGeneration(generation);
            worker->setTimeline(workspace);
        });
    }
    Q_INVOKABLE void seek(int ms) {
        (void)item_.takePendingFrame();
        last_frame_.clear();
        const auto generation = sink_->invalidate();
        QMetaObject::invokeMethod(worker_, [worker = worker_, ms, generation] {
            worker->setRequestGeneration(generation);
            worker->seek(ms);
        });
    }
    Q_INVOKABLE void play() {
        QMetaObject::invokeMethod(worker_, [worker = worker_] { worker->play(0); });
    }
    Q_INVOKABLE QVariantMap takeFrame() {
        const auto pending = item_.takePendingFrame();
        QVariantMap result;
        if (!pending.timeline)
            return last_frame_;
        const auto& frame = *pending.timeline;
        result["time"] = static_cast<qlonglong>(frame.time_us / 1000);
        result["generation"] = static_cast<qulonglong>(pending.generation);
        result["layers"] = static_cast<int>(frame.layers.size());
        if (!frame.layers.empty()) {
            result["weight"] = frame.layers[0].weight;
            result["source"] = static_cast<qlonglong>(frame.layers[0].frame.pts_us);
        }
        last_frame_ = result;
        return result;
    }
    Q_INVOKABLE bool rejectsStaleFrame() {
        const auto generation = sink_->invalidate();
        (void)item_.takePendingFrame();
        sink_->deliverTimeline({}, generation - 1);
        return !item_.takePendingFrame().timeline.has_value();
    }
  signals:
    void changed();

  private:
    std::shared_ptr<exosnap::quick::EditPlayerFrameSink> sink_ =
        std::make_shared<exosnap::quick::EditPlayerFrameSink>();
    exosnap::quick::ExoEditPlayerItem item_;
    QThread thread_;
    exosnap::quick::EditPlayerWorker* worker_ = nullptr;
    QString error_;
    QVariantMap last_frame_;
    bool ended_ = false;
};

class Setup final : public QObject {
    Q_OBJECT

  public slots:
    void exportVisualState(int state) {
        page_exporter_->applyVisualState(static_cast<exosnap::quick::EditExportAdapter::State>(state), 25, {}, {});
    }
    void applicationAvailable() {
        QCoreApplication::setOrganizationName(QStringLiteral("ExoSnap"));
        QCoreApplication::setOrganizationDomain(QStringLiteral("exosnap.example"));
        QCoreApplication::setApplicationName(QStringLiteral("edit-timeline-qml-tests"));
        QQuickStyle::setStyle(QStringLiteral("Basic"));
    }

    void qmlEngineAvailable(QQmlEngine* engine) {
        engine->rootContext()->setContextProperty(QStringLiteral("testPersistenceStage"),
                                                  qEnvironmentVariable("EXOSNAP_EDIT_PERSISTENCE_STAGE"));
        engine->rootContext()->setContextProperty(QStringLiteral("testVisualDirectory"),
                                                  qEnvironmentVariable("EXOSNAP_EDIT_VISUAL_DIR"));
        engine->rootContext()->setContextProperty(QStringLiteral("testScale"),
                                                  qEnvironmentVariable("QT_SCALE_FACTOR", "1"));
        engine->rootContext()->setContextProperty(QStringLiteral("testMediaPreview"), new MediaPreviewDriver(this));
        session_ = new EditSessionAdapter(this);
        timeline_ = new EditTimelineAdapter(this);
        player_ = new EditPlayerAdapter(this);

        session_->setEditContext(FixtureContext());
        // A SECOND player, seeded with an open clip. The timeline's own cases
        // assert what the keyboard does when nothing is open, so the transport
        // cases -- which need the opposite -- cannot share that adapter.
        // Nothing decodes either way: the fixture carries no master path.
        transport_player_ = new EditPlayerAdapter(this);
        transport_player_->setClipStateForTest(/*clip_open=*/true, /*duration_ms=*/100'000);

        engine->rootContext()->setContextProperty(QStringLiteral("testSession"), session_);
        engine->rootContext()->setContextProperty(QStringLiteral("testTimeline"), timeline_);
        engine->rootContext()->setContextProperty(QStringLiteral("testPlayer"), player_);
        engine->rootContext()->setContextProperty(QStringLiteral("testTransportPlayer"), transport_player_);

        auto* page_session = new EditSessionAdapter(this);
        auto* page_timeline = new EditTimelineAdapter(this);
        auto* page_player = new EditPlayerAdapter(this);
        auto* page_exporter = new exosnap::quick::EditExportAdapter(this);
        page_exporter_ = page_exporter;
        engine->rootContext()->setContextProperty(QStringLiteral("testEditHarness"), this);
        auto* page_recordings = new exosnap::quick::RecordViewModelAdapter(&record_view_model_, this);
        page_exporter->setSession(page_session);
        engine->rootContext()->setContextProperty(QStringLiteral("testPageSession"), page_session);
        engine->rootContext()->setContextProperty(QStringLiteral("testPageTimeline"), page_timeline);
        engine->rootContext()->setContextProperty(QStringLiteral("testPagePlayer"), page_player);
        engine->rootContext()->setContextProperty(QStringLiteral("testPageExporter"), page_exporter);
        engine->rootContext()->setContextProperty(QStringLiteral("testPageRecordings"), page_recordings);
    }

  private:
    exosnap::RecordViewModel record_view_model_;
    EditSessionAdapter* session_ = nullptr;
    EditTimelineAdapter* timeline_ = nullptr;
    EditPlayerAdapter* player_ = nullptr;
    EditPlayerAdapter* transport_player_ = nullptr;
    exosnap::quick::EditExportAdapter* page_exporter_ = nullptr;
};

QUICK_TEST_MAIN_WITH_SETUP(edit_timeline, Setup)

#include "edit_timeline_qml_test_main.moc"
