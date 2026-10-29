#include <gtest/gtest.h>

#include <QProcess>
#include <QProcessEnvironment>

#include "SetupPreviewPath.h"

TEST(SetupWindowLifecycleTest, NativeWindowResizesAfterDetectionWithoutTakingFocus) {
    for (const QString& scale : {QStringLiteral("1"), QStringLiteral("1.5"), QStringLiteral("2")}) {
        SCOPED_TRACE(scale.toStdString());
        QProcess process;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("windows"));
        environment.insert(QStringLiteral("QT_QUICK_BACKEND"), QStringLiteral("software"));
        environment.insert(QStringLiteral("QT_SCALE_FACTOR"), scale);
        process.setProcessEnvironment(environment);
        process.start(QString::fromUtf8(EXOSNAP_SETUP_PREVIEW_EXE),
                      {QStringLiteral("--preview-state"), QStringLiteral("lifecycle")});
        ASSERT_TRUE(process.waitForStarted(10000));
        const bool finished = process.waitForFinished(15000);
        if (!finished) {
            process.kill();
            process.waitForFinished(5000);
        }
        ASSERT_TRUE(finished);
        const QByteArray errors = process.readAllStandardError();
        ASSERT_EQ(process.exitCode(), 0) << errors.constData();
        const QByteArray trace = process.readAllStandardOutput();
        EXPECT_TRUE(trace.contains("decision 480x300 min=300 max=300 focused=0")) << trace.constData();
        EXPECT_TRUE(trace.contains("applying 480x176 min=176 max=176 focused=0")) << trace.constData();
        EXPECT_TRUE(trace.contains("success 480x176 min=176 max=176 focused=0")) << trace.constData();
    }
}
