// Deterministic offscreen visual evidence for the standalone updater.
//
// The production exosnap-updater.exe renders each canned preview state through
// the real Quick window and writes a PNG through its own `--screenshot` path.
// No live ExoSnap instance, pointer synthesis or desktop capture is involved.
// The state matrix runs once in the shipped default (Dark) appearance and once
// in Light, which the harness sets through the shared theme singleton -- exactly
// the seam a test adapter would use.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include "UpdaterArgs.h"
#include "UpdaterExePath.h"

using namespace exosnap::updater;

namespace {

QString RepositoryRelativeDirectory(const QString& relative) {
    QDir dir(QCoreApplication::applicationDirPath());
    for (int i = 0; i < 12; ++i) {
        if (dir.exists(QStringLiteral(".git")))
            return dir.absoluteFilePath(relative);
        if (!dir.cdUp())
            break;
    }
    return QCoreApplication::applicationDirPath();
}

QString EvidenceDirectory() {
    return RepositoryRelativeDirectory(QStringLiteral(".workspace/live-verify/updater-visual/current"));
}

struct ShotResult {
    bool started = false;
    bool finished = false;
    int exit_code = -1;
    QString path;
};

ShotResult Capture(const QString& state, const QString& appearance, const QString& directory,
                   const QString& language = QStringLiteral("en"), const QString& scale = QStringLiteral("1")) {
    ShotResult result;
    result.path = QDir(directory).filePath(QStringLiteral("%1-%2.png").arg(state, appearance));

    QProcess process;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    env.insert(QStringLiteral("QT_QUICK_BACKEND"), QStringLiteral("software"));
    env.insert(QStringLiteral("QT_SCALE_FACTOR"), scale);
    process.setProcessEnvironment(env);
    process.start(QString::fromUtf8(EXOSNAP_UPDATER_EXE),
                  {QStringLiteral("--preview-state"), state, QStringLiteral("--appearance"), appearance,
                   QStringLiteral("--screenshot"), result.path, QStringLiteral("--ui-language"), language});
    result.started = process.waitForStarted(10000);
    if (!result.started)
        return result;
    result.finished = process.waitForFinished(30000);
    if (!result.finished) {
        process.kill();
        process.waitForFinished(5000);
        return result;
    }
    result.exit_code = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
    return result;
}

} // namespace

TEST(UpdaterVisualProofTest, EveryPreviewStateRendersDarkAndLightEvidence) {
    const QString directory = EvidenceDirectory();
    ASSERT_TRUE(QDir().mkpath(directory)) << directory.toStdString();

    const QStringList states = PreviewStateNames();
    ASSERT_FALSE(states.isEmpty());

    int captured = 0;
    for (const QString& state : states) {
        for (const QString& appearance : {QStringLiteral("dark"), QStringLiteral("light")}) {
            const ShotResult shot = Capture(state, appearance, directory);
            EXPECT_TRUE(shot.started) << state.toStdString();
            ASSERT_TRUE(shot.finished) << state.toStdString();
            EXPECT_EQ(shot.exit_code, 0) << state.toStdString() << " " << appearance.toStdString();
            EXPECT_TRUE(QFileInfo::exists(shot.path)) << shot.path.toStdString();

            const QImage image(shot.path);
            EXPECT_FALSE(image.isNull()) << shot.path.toStdString();
            EXPECT_EQ(image.width(), 520) << shot.path.toStdString();
            EXPECT_EQ(image.height(), 680) << shot.path.toStdString();
            ++captured;
        }
    }
    EXPECT_EQ(captured, states.size() * 2);
}

TEST(UpdaterVisualProofTest, GermanPreviewStatesRenderAtAllSupportedScales) {
    const QStringList states = PreviewStateNames();
    ASSERT_FALSE(states.isEmpty());
    for (const QString& scale :
         {QStringLiteral("1"), QStringLiteral("1.25"), QStringLiteral("1.5"), QStringLiteral("2")}) {
        const QString directory = QDir(EvidenceDirectory()).filePath(QStringLiteral("de-%1").arg(scale));
        ASSERT_TRUE(QDir().mkpath(directory));
        for (const QString& state : states) {
            const ShotResult shot = Capture(state, QStringLiteral("dark"), directory, QStringLiteral("de"), scale);
            ASSERT_TRUE(shot.started) << state.toStdString();
            ASSERT_TRUE(shot.finished) << state.toStdString();
            EXPECT_EQ(shot.exit_code, 0) << state.toStdString() << " scale " << scale.toStdString();
            const QImage image(shot.path);
            EXPECT_FALSE(image.isNull()) << shot.path.toStdString();
            EXPECT_GE(image.width(), 520);
            EXPECT_GE(image.height(), 680);
        }
    }
}