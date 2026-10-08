// Test runner for the QML contracts of the Record source picker and the
// transport dock.
//
// A separate executable from record_controls_qml_tests for the same reason the
// edit runner exists: both take a REQUIRED, C++-typed adapter
// (RecordViewModelAdapter, QML_UNCREATABLE like every application-provided
// adapter), so neither can be stubbed from QML. The runner owns one real adapter
// over a seeded RecordViewModel and exposes it, plus a seed entry point that
// rebuilds the target list through the same revision bump a rescan uses.

#include "RecordViewModelAdapter.h"

#include "viewmodels/RecordViewModel.h"

#include <QCoreApplication>
#include <QObject>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQuickTest>

#include <cstdint>
#include <memory>

using exosnap::RecordViewModel;
using exosnap::quick::RecordViewModelAdapter;

class RecordPickerTestDriver final : public QObject {
    Q_OBJECT

    Q_PROPERTY(exosnap::quick::RecordViewModelAdapter* adapter READ adapter CONSTANT)

  public:
    explicit RecordPickerTestDriver(QObject* parent = nullptr) : QObject(parent) {
        seedTargets(2, {QStringLiteral("Claude Design - Brave"), QStringLiteral("Task Manager")});
    }

    [[nodiscard]] RecordViewModelAdapter* adapter() {
        return &adapter_;
    }

    Q_INVOKABLE void seedTargets(int display_count, const QStringList& window_labels) {
        source_.targets.clear();
        for (int i = 0; i < display_count; ++i) {
            source_.targets.push_back({exosnap::engine::CaptureTarget::Kind::Monitor, static_cast<std::uint64_t>(i + 1),
                                       "\\\\.\\DISPLAY" + std::to_string(i + 1)});
        }
        std::uint64_t window_id = 100;
        for (const QString& label : window_labels) {
            source_.targets.push_back({exosnap::engine::CaptureTarget::Kind::Window, window_id++, label.toStdString()});
        }
        source_.selected_target_index = 0;
        source_.capture_mode = exosnap::CaptureMode::Monitor;
        ++source_.targets_revision;
        adapter_.setSource(&source_);
    }

    // A rescan that keeps the native handles of the windows the caller names,
    // so a test can prove the pending selection follows its identity through an
    // insert or a reorder instead of the index it happened to have.
    Q_INVOKABLE void seedWindowTargets(const QVariantList& windows) {
        source_.targets.clear();
        for (const QVariant& value : windows) {
            const QVariantMap row = value.toMap();
            source_.targets.push_back({exosnap::engine::CaptureTarget::Kind::Window,
                                       row.value(QStringLiteral("id")).toULongLong(),
                                       row.value(QStringLiteral("label")).toString().toStdString()});
        }
        if (source_.selected_target_index >= static_cast<int>(source_.targets.size()))
            source_.selected_target_index = source_.targets.empty() ? -1 : 0;
        ++source_.targets_revision;
        adapter_.setSource(&source_);
    }

    // "ready", "recording", "paused", "completed" (a successful run), "failed",
    // or anything else for the view model's initial LoadingCapabilities state.
    // The finished states carry the run's duration, so a test can tell whether
    // the transport still shows it.
    Q_INVOKABLE void setRecordingState(const QString& state) {
        if (state == QLatin1String("ready"))
            source_.state = exosnap::UiRecordingState::Ready;
        else if (state == QLatin1String("recording"))
            source_.state = exosnap::UiRecordingState::Recording;
        else if (state == QLatin1String("paused"))
            source_.state = exosnap::UiRecordingState::Paused;
        else if (state == QLatin1String("completed"))
            source_.state = exosnap::UiRecordingState::Completed;
        else if (state == QLatin1String("failed"))
            source_.state = exosnap::UiRecordingState::Failed;
        else
            source_.state = exosnap::UiRecordingState::LoadingCapabilities;
        source_.last_succeeded = state == QLatin1String("completed");
        source_.elapsed_text = state == QLatin1String("paused")      ? L"12:34"
                               : state == QLatin1String("completed") ? L"1:23"
                               : state == QLatin1String("failed")    ? L"0:42"
                                                                     : L"";
        adapter_.setSource(&source_);
    }

    // The still service is a C++ collaborator the QML test does not have, so
    // the two edges it drives are reachable from the test instead.
    Q_INVOKABLE void deliverStill(const QString& identity, const QString& source) {
        adapter_.setTargetStill(identity, source);
    }

    Q_INVOKABLE void failStill(const QString& identity) {
        adapter_.setTargetStillUnavailable(identity);
    }

  private:
    RecordViewModel source_;
    RecordViewModelAdapter adapter_;
};

class Setup final : public QObject {
    Q_OBJECT

  public slots:
    void applicationAvailable() {
        QCoreApplication::setOrganizationName(QStringLiteral("ExoSnap"));
        QCoreApplication::setOrganizationDomain(QStringLiteral("exosnap.example"));
        QCoreApplication::setApplicationName(QStringLiteral("record-source-picker-qml-tests"));
        QQuickStyle::setStyle(QStringLiteral("Basic"));
        driver_ = std::make_unique<RecordPickerTestDriver>();
    }

    void qmlEngineAvailable(QQmlEngine* engine) {
        engine->rootContext()->setContextProperty(QStringLiteral("recordDriver"), driver_.get());
    }

  private:
    std::unique_ptr<RecordPickerTestDriver> driver_;
};

QUICK_TEST_MAIN_WITH_SETUP(record_source_picker, Setup)

#include "record_picker_qml_test_main.moc"
