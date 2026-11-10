#include "ControllerState.h"
#include "GamepadArtwork.h"
#include "GamepadOverlay.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSurfaceFormat>
#include <QTextStream>
#include <QTimer>
#include <algorithm>

using namespace exosnap::quick;
using namespace exosnap::gamepad;

namespace {
QString Option(const QStringList& args, const QString& name, const QString& fallback = {}) {
    const auto index = args.indexOf(name);
    return index >= 0 && index + 1 < args.size() ? args[index + 1] : fallback;
}

GamepadReading Fixture(const QString& scenario, uint64_t timestamp) {
    GamepadReading reading;
    reading.capture_100ns = timestamp;
    reading.state.controls_normalized = true;
    for (auto& value : reading.state.controls)
        value = {ControlStatus::Released, 0, 0};
    const bool half = scenario.endsWith("half");
    const bool pressed = half || scenario.endsWith("full") || scenario.endsWith("side") ||
                         scenario.endsWith("diagonal") || scenario.endsWith("click");
    if (pressed) {
        for (auto& value : reading.state.controls)
            value = {ControlStatus::Active, half ? .5F : 1.0F, 0};
        for (const auto control : {Control::LeftStick, Control::RightStick})
            reading.state.controls[static_cast<size_t>(control)] = {ControlStatus::Active, half ? .5F : 1.0F,
                                                                    scenario.contains("diagonal") ? 1.0F : 0};
        if (!scenario.endsWith("click")) {
            reading.state.controls[static_cast<size_t>(Control::LeftStickClick)] = {ControlStatus::Released, 0, 0};
            reading.state.controls[static_cast<size_t>(Control::RightStickClick)] = {ControlStatus::Released, 0, 0};
        }
    }
    if (scenario.endsWith("unavailable"))
        reading.connected = false;
    if (scenario.endsWith("unsupported"))
        for (auto& value : reading.state.controls)
            value = {};
    return reading;
}

QByteArray Hash(const QImage& image) {
    return QCryptographicHash::hash(
               QByteArrayView(reinterpret_cast<const char*>(image.constBits()), image.sizeInBytes()),
               QCryptographicHash::Sha256)
        .toHex();
}
} // namespace

int main(int argc, char** argv) {
    RegisterInputMainThread();
    QCoreApplication::setAttribute(Qt::AA_DisableShaderDiskCache);
    QSurfaceFormat format;
    format.setSwapInterval(0);
    QSurfaceFormat::setDefaultFormat(format);
    QGuiApplication app(argc, argv);
    const auto args = app.arguments();
    const QString output = Option(args, QStringLiteral("--visual-test"));
    const bool capture = !output.isEmpty();
    const QString scenario = qEnvironmentVariable("EXOSNAP_VISUAL_CONTROLLER3D", "normal-neutral");
    const QStringList scenarios{
        "normal-neutral",      "normal-half",           "normal-full",        "normal-diagonal",
        "normal-click",        "normal-diagonal-click", "normal-unavailable", "normal-unsupported",
        "normal-side",         "ghost-neutral",         "ghost-full",         "ghost-pattern",
        "ghost-side",          "ghost-depth",           "normal-depth",       "normal-closeup-neutral",
        "normal-closeup-half", "normal-closeup-full"};
    if (!scenarios.contains(scenario)) {
        qCritical("Unknown controller3d fixture");
        return 2;
    }
    QString error;
    QStringList warnings;
    const QString pack = QStringLiteral(":/gamepad/controllers/") +
                         Option(args, QStringLiteral("--controller-pack"), QStringLiteral("dualsense"));
    const auto model = SharedControllerModel(pack, warnings, error);
    const auto mapping = model ? ControllerPressMapping(pack, error) : MappedGamepadArtwork{};
    for (const auto& warning : warnings)
        qWarning().noquote() << warning;
    if (!model || !error.isEmpty()) {
        qCritical().noquote() << error;
        return 2;
    }
    GamepadOverlay owner;
    ControllerState state(model, mapping);
    if (!capture) {
        exosnap::GamepadOverlaySettings settings;
        settings.enabled = true;
        settings.layout = 0;
        settings.device = qEnvironmentVariable("EXOSNAP_CONTROLLER3D_DEVICE").toStdString();
        owner.Configure(settings);
    }
    const auto reading = Fixture(scenario, NowCapture100ns());
    if (capture)
        state.ApplyReading(reading, reading.capture_100ns);

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(2); },
        Qt::QueuedConnection);
    const auto size = Option(args, QStringLiteral("--visual-test-size"), QStringLiteral("960x640")).split('x');
    if (size.size() != 2 || size[0].toInt() < 120 || size[1].toInt() < 100 || size[0].toInt() > 4096 ||
        size[1].toInt() > 4096)
        return 2;
    engine.setInitialProperties(
        {{QStringLiteral("controller"), QVariant::fromValue(&state)},
         {QStringLiteral("capture"), capture},
         {QStringLiteral("width"), size[0].toInt()},
         {QStringLiteral("height"), size[1].toInt()},
         {QStringLiteral("light"), Option(args, QStringLiteral("--visual-appearance")) == "light"},
         {QStringLiteral("ghost"), scenario.startsWith("ghost")},
         {QStringLiteral("pattern"), scenario.endsWith("pattern")},
         {QStringLiteral("closeup"), scenario.contains("closeup")},
         {QStringLiteral("viewRotation"),
          QVariant::fromValue(scenario.endsWith("side") ? QVector3D(0, -65, 0) : QVector3D())}});
    engine.loadFromModule("ExoSnap.Controller3DPilot", "PilotWindow");
    if (engine.rootObjects().isEmpty())
        return 2;
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    if (!window)
        return 2;
    QQmlComponent depth_component(&engine);
    std::unique_ptr<QObject> depth_probe;
    if (capture && scenario.endsWith("depth")) {
        depth_component.loadUrl(QUrl(QStringLiteral("qrc:/qt/qml/ExoSnap/Controller3DPilot/ControllerDepthProbe.qml")));
        depth_probe.reset(depth_component.createWithInitialProperties(
            {{QStringLiteral("parent"), QVariant::fromValue(window->findChild<QObject*>("controllerRig"))}}));
        if (!depth_probe)
            return 2;
    }
    QTimer live;
    if (!capture) {
        QObject::connect(&live, &QTimer::timeout, &state, [&] {
            const auto now = NowCapture100ns();
            state.ApplyReading(owner.ReadingAt(now), now);
        });
        live.start(8);
        return app.exec();
    }

    const int fps = qEnvironmentVariableIntValue("EXOSNAP_CONTROLLER3D_FPS")
                        ? qEnvironmentVariableIntValue("EXOSNAP_CONTROLLER3D_FPS")
                        : 60;
    const int seconds = qEnvironmentVariableIntValue("EXOSNAP_CONTROLLER3D_SECONDS")
                            ? qEnvironmentVariableIntValue("EXOSNAP_CONTROLLER3D_SECONDS")
                            : 1;
    if ((fps != 30 && fps != 60 && fps != 120) || seconds < 1 || seconds > 30)
        return 2;
    if (!QDir().mkpath(QFileInfo(output).absolutePath()))
        return 2;
    QFile trace(output + ".csv");
    if (!trace.open(QIODevice::WriteOnly | QIODevice::Text))
        return 2;
    QTextStream trace_stream(&trace);
    trace_stream << "frame,capture_100ns,input_100ns,pose_100ns,pose_generation,geometry_generation,pose_sha256,frame_"
                    "sha256,stick_roi_sha256\n";
    QElapsedTimer elapsed;
    QTimer timer;
    timer.setTimerType(Qt::PreciseTimer);
    timer.setSingleShot(true);
    QImage first;
    QByteArray first_hash;
    int frames = 0, changed = 0;
    int first_geometry = -1;
    bool geometry_changed = false;
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        if (!elapsed.isValid())
            elapsed.start();
        const auto now = NowCapture100ns();
        state.ApplyReading(reading, now);
        const auto pose_time = NowCapture100ns();
        if (depth_probe)
            depth_probe->setProperty("visible", frames % 2 == 0);
        const QImage image = window->grabWindow().convertToFormat(QImage::Format_RGBA8888);
        if (image.isNull()) {
            qCritical("Controller3D returned an empty rendered frame");
            app.exit(3);
            return;
        }
        // Geometry is immutable once loaded, so the count of drawn primitives identifies it.
        const int geometry_generation = static_cast<int>(state.Parts().Rows().size());
        const auto hash = Hash(image);
        QByteArray pose_bytes;
        QDataStream pose_stream(&pose_bytes, QIODevice::WriteOnly);
        for (const auto& row : state.Parts().Rows())
            pose_stream << row.position << row.rotation << row.scale << row.active;
        const auto pose_hash = QCryptographicHash::hash(pose_bytes, QCryptographicHash::Sha256).toHex();
        const auto roi = image.copy(image.width() / 4, image.height() / 3, image.width() / 2, image.height() / 2);
        if (frames == 0) {
            first = image;
            first_hash = hash;
            first_geometry = geometry_generation;
        } else if (hash != first_hash) {
            ++changed;
        }
        geometry_changed = geometry_changed || geometry_generation != first_geometry;
        trace_stream << frames << ',' << now << ',' << state.inputTime() << ',' << pose_time << ','
                     << state.poseGeneration() << ',' << geometry_generation << ',' << pose_hash << ',' << hash << ','
                     << Hash(roi) << '\n';
        ++frames;
        if (elapsed.elapsed() < seconds * 1000) {
            const auto due = static_cast<qint64>(frames) * 1000 / fps;
            timer.start(static_cast<int>(std::max<qint64>(1, due - elapsed.elapsed())));
            return;
        }
        trace_stream.flush();
        const auto measured_ms = elapsed.elapsed();
        const bool saved = first.save(output);
        bool depth_probe_verified = true;
        if (depth_probe) {
            for (auto* shell : window->findChildren<QObject*>(QStringLiteral("controllerShell")))
                shell->setProperty("visible", false);
            depth_probe->setProperty("visible", false);
            const auto uncovered = window->grabWindow();
            depth_probe->setProperty("visible", true);
            const auto exposed = window->grabWindow();
            depth_probe_verified = uncovered != exposed;
            exposed.save(output + ".depth-open.png");
        }
        QJsonObject receipt{
            {"scenario", scenario},
            {"frames", frames},
            {"requested_fps", fps},
            {"elapsed_ms", measured_ms},
            {"changed_frames", changed},
            {"cadence_met", frames >= fps * seconds},
            {"pose_sha256", QString::fromLatin1(pose_hash)},
            {"geometry_generation", first_geometry},
            {"geometry_changed", geometry_changed},
            {"texture_generation", QJsonValue::Null},
            {"frame_sha256", QString::fromLatin1(first_hash)},
            {"graphics_api", static_cast<int>(window->rendererInterface()->graphicsApi())},
            {"software_adapter_requested", qEnvironmentVariable("QSG_RHI_PREFER_SOFTWARE_RENDERER") == "1"},
            {"depth_probe_verified", depth_probe ? QJsonValue(depth_probe_verified) : QJsonValue(QJsonValue::Null)},
            {"stable", changed == 0 && !geometry_changed && saved && first_geometry > 0 && depth_probe_verified}};
        receipt.insert("passed",
                       receipt.value("stable").toBool() && (seconds < 30 || receipt.value("cadence_met").toBool()));
        QFile report(output + ".json");
        const bool written = report.open(QIODevice::WriteOnly) && report.write(QJsonDocument(receipt).toJson()) > 0;
        qInfo().noquote() << QJsonDocument(receipt).toJson(QJsonDocument::Compact);
        app.exit(written && receipt.value("passed").toBool() ? 0 : 3);
    });
    timer.start(350);
    return app.exec();
}
