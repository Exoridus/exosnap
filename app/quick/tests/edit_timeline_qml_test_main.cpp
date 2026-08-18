// Test runner for the Edit surface's keyboard contract (QCR-504).
//
// A separate executable from record_controls_qml_tests because EditTimeline.qml
// takes three REQUIRED, C++-typed adapters. They cannot be stubbed from QML and
// they cannot be seeded from QML either — `setEditContext` is a plain C++ entry
// point, not Q_INVOKABLE, and making it one to suit a test would widen the
// production API. So the adapters are built here, seeded with the same
// no-master-path fixture context `test_edit_adapters.cpp` uses (nothing is
// opened, decoded or remuxed), and handed to QML as context properties.

#include "EditPlayerAdapter.h"
#include "EditSessionAdapter.h"
#include "EditTimelineAdapter.h"

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

class Setup final : public QObject {
    Q_OBJECT

  public slots:
    void applicationAvailable() {
        QCoreApplication::setOrganizationName(QStringLiteral("ExoSnap"));
        QCoreApplication::setOrganizationDomain(QStringLiteral("exosnap.example"));
        QCoreApplication::setApplicationName(QStringLiteral("edit-timeline-qml-tests"));
        QQuickStyle::setStyle(QStringLiteral("Basic"));
    }

    void qmlEngineAvailable(QQmlEngine* engine) {
        session_ = new EditSessionAdapter(this);
        timeline_ = new EditTimelineAdapter(this);
        player_ = new EditPlayerAdapter(this);

        session_->setEditContext(FixtureContext());

        engine->rootContext()->setContextProperty(QStringLiteral("testSession"), session_);
        engine->rootContext()->setContextProperty(QStringLiteral("testTimeline"), timeline_);
        engine->rootContext()->setContextProperty(QStringLiteral("testPlayer"), player_);
    }

  private:
    EditSessionAdapter* session_ = nullptr;
    EditTimelineAdapter* timeline_ = nullptr;
    EditPlayerAdapter* player_ = nullptr;
};

QUICK_TEST_MAIN_WITH_SETUP(edit_timeline, Setup)

#include "edit_timeline_qml_test_main.moc"
