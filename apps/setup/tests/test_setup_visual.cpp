#include <gtest/gtest.h>

#include <QDir>
#include <QImage>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>

#include "SetupPreviewPath.h"

TEST(SetupVisualTest, EveryFinalStateRendersWithoutQmlErrorsAtItsDeclaredSize) {
    const QStringList states{
        QStringLiteral("checking"),  QStringLiteral("install"),        QStringLiteral("manage"),
        QStringLiteral("uninstall"), QStringLiteral("uninstall-data"), QStringLiteral("progress"),
        QStringLiteral("protected"), QStringLiteral("success"),        QStringLiteral("reboot"),
        QStringLiteral("failure"),   QStringLiteral("removed"),        QStringLiteral("cancelled")};
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    for (const QString& language : {QStringLiteral("en"), QStringLiteral("de")}) {
        for (const QString& state : states) {
            SCOPED_TRACE(state.toStdString());
            SCOPED_TRACE(language.toStdString());
            const QString path = QDir(directory.path()).filePath(state + QStringLiteral(".png"));
            QProcess process;
            auto environment = QProcessEnvironment::systemEnvironment();
            environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
            environment.insert(QStringLiteral("QT_QUICK_BACKEND"), QStringLiteral("software"));
            environment.insert(QStringLiteral("QT_SCALE_FACTOR"), QStringLiteral("1"));
            process.setProcessEnvironment(environment);
            process.start(QString::fromUtf8(EXOSNAP_SETUP_PREVIEW_EXE),
                          {QStringLiteral("--preview-state"), state, QStringLiteral("--ui-language"), language,
                           QStringLiteral("--screenshot"), path});
            ASSERT_TRUE(process.waitForStarted(10000));
            const bool finished = process.waitForFinished(15000);
            if (!finished) {
                process.kill();
                process.waitForFinished(5000);
            }
            ASSERT_TRUE(finished);
            const QByteArray errors = process.readAllStandardError();
            ASSERT_EQ(process.exitCode(), 0) << errors.constData();
            EXPECT_FALSE(errors.contains("ReferenceError")) << errors.constData();
            EXPECT_FALSE(errors.contains("Binding loop")) << errors.constData();
            EXPECT_FALSE(errors.contains("failed to load")) << errors.constData();
            const QImage image(path);
            ASSERT_FALSE(image.isNull());
            EXPECT_EQ(image.width(), 480);
            const bool decision = state == QStringLiteral("install") || state == QStringLiteral("manage") ||
                                  state.startsWith(QStringLiteral("uninstall"));
            EXPECT_EQ(image.height(), decision ? 300 : 176);
        }
    }
}
