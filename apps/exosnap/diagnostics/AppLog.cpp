#include "AppLog.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QTextStream>

namespace exosnap::diagnostics {
namespace {

static QMutex g_log_mutex;
static QString g_log_path;

void writeLineUnlocked(const QString& message) {
    if (g_log_path.isEmpty())
        return;
    QFile file(g_log_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;
    QTextStream stream(&file);
    stream << QDateTime::currentDateTime().toString(Qt::ISODateWithMs) << "  " << message << '\n';
    stream.flush();
}

void qtMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& msg) {
    Q_UNUSED(context);
    if (type == QtCriticalMsg || type == QtFatalMsg) {
        const QString prefix = (type == QtFatalMsg) ? QStringLiteral("[fatal] ") : QStringLiteral("[critical] ");
        QMutexLocker lock(&g_log_mutex);
        writeLineUnlocked(prefix + msg);
    }
    // Re-print to default output as well
    if (type == QtFatalMsg)
        abort();
}

} // namespace

void AppLogInit() {
    QMutexLocker lock(&g_log_mutex);
    if (!g_log_path.isEmpty())
        return;

    const QString data_dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    const QString log_dir = data_dir + QStringLiteral("/logs");
    QDir().mkpath(log_dir);
    g_log_path = log_dir + QStringLiteral("/exosnap.log");

    writeLineUnlocked(QStringLiteral("[startup] --- ExoSnap session start ---"));
    writeLineUnlocked(QStringLiteral("[startup] log path: ") + g_log_path);

    qInstallMessageHandler(qtMessageHandler);
}

void AppLog(const QString& message) {
    QMutexLocker lock(&g_log_mutex);
    writeLineUnlocked(message);
}

QString LogFilePath() {
    QMutexLocker lock(&g_log_mutex);
    return g_log_path;
}

} // namespace exosnap::diagnostics
