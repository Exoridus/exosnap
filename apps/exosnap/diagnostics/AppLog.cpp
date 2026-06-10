#include "AppLog.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QStringConverter>
#include <QTextStream>
#include <QThread>

#include "settings/ConfigPaths.h"

#include <algorithm>
#include <deque>
#include <utility>

namespace exosnap::diagnostics {
namespace {

struct LogState {
    QMutex mutex;
    QString log_path;
    std::deque<LogEntry> history;
    QVector<LogEntry> pending_entries;
    int pending_evicted_count = 0;
    int max_entries = AppLog::kDefaultMaxEntries;
    quint64 next_sequence = 1;
    bool initialized = false;
    bool delivery_scheduled = false;
    bool delivery_enabled = true;
    bool qt_handler_installed = false;
    QtMessageHandler previous_qt_handler = nullptr;
    std::function<QDateTime()> timestamp_provider;
};

LogState& state() {
    static LogState s;
    return s;
}

QDateTime currentTimestamp() {
    QMutexLocker lock(&state().mutex);
    if (state().timestamp_provider)
        return state().timestamp_provider();
    return QDateTime::currentDateTime();
}

QString normalizedCategory(const QString& category) {
    return category.trimmed();
}

QString normalizedMessage(const QString& message) {
    return message.trimmed();
}

bool writeLineUnlocked(const LogEntry& entry) {
    auto& s = state();
    if (s.log_path.isEmpty())
        return true;

    QFile file(s.log_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return false;

    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    stream << AppLog::formatEntry(entry) << '\n';
    stream.flush();
    return stream.status() == QTextStream::Ok;
}

LogSeverity severityFromQtMessage(QtMsgType type) {
    switch (type) {
    case QtDebugMsg:
        return LogSeverity::Debug;
    case QtInfoMsg:
        return LogSeverity::Info;
    case QtWarningMsg:
        return LogSeverity::Warning;
    case QtCriticalMsg:
    case QtFatalMsg:
        return LogSeverity::Error;
    }
    return LogSeverity::Info;
}

void qtMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& msg) {
    const QString category = context.category != nullptr && *context.category != '\0'
                                 ? QString::fromUtf8(context.category)
                                 : QStringLiteral("Qt");
    AppLog::write(severityFromQtMessage(type), category, msg);

    QtMessageHandler previous = nullptr;
    {
        QMutexLocker lock(&state().mutex);
        previous = state().previous_qt_handler;
    }
    if (previous != nullptr)
        previous(type, context, msg);

    if (type == QtFatalMsg)
        abort();
}

void appendToHistoryUnlocked(const LogEntry& entry, int* evicted_count) {
    auto& s = state();
    s.history.push_back(entry);
    while (static_cast<int>(s.history.size()) > s.max_entries) {
        s.history.pop_front();
        if (evicted_count != nullptr)
            ++(*evicted_count);
    }
}

void resetUnlocked(int max_entries) {
    auto& s = state();
    s.history.clear();
    s.pending_entries.clear();
    s.pending_evicted_count = 0;
    s.delivery_scheduled = false;
    s.delivery_enabled = true;
    s.max_entries = std::max(1, max_entries);
    s.next_sequence = 1;
    s.log_path.clear();
    s.initialized = false;
    s.timestamp_provider = nullptr;
}

} // namespace

AppLog::AppLog(QObject* parent) : QObject(parent) {
}

AppLog& AppLog::instance() {
    static AppLog* log = new AppLog();
    return *log;
}

void AppLog::init() {
    AppLog& log = instance();
    Q_UNUSED(log);

    qRegisterMetaType<LogEntry>("exosnap::diagnostics::LogEntry");
    qRegisterMetaType<QVector<LogEntry>>("QVector<exosnap::diagnostics::LogEntry>");

    bool write_startup = false;
    QString path;
    {
        QMutexLocker lock(&state().mutex);
        auto& s = state();
        if (s.initialized)
            return;

        const QString data_dir = settings::ResolveAppDataDir();
        const QString log_dir = data_dir + QStringLiteral("/logs");
        QDir().mkpath(log_dir);
        s.log_path = log_dir + QStringLiteral("/exosnap.log");
        s.initialized = true;
        s.delivery_enabled = true;
        path = s.log_path;
        write_startup = true;
    }

    if (QCoreApplication* app = QCoreApplication::instance()) {
        QObject::connect(app, &QCoreApplication::aboutToQuit, &instance(), []() {
            QMutexLocker lock(&state().mutex);
            state().delivery_enabled = false;
            state().pending_entries.clear();
            state().pending_evicted_count = 0;
            state().delivery_scheduled = false;
        });
    }

    {
        QMutexLocker lock(&state().mutex);
        auto& s = state();
        if (!s.qt_handler_installed) {
            s.previous_qt_handler = qInstallMessageHandler(qtMessageHandler);
            s.qt_handler_installed = true;
        }
    }

    if (write_startup) {
        info(QStringLiteral("startup"), QStringLiteral("--- ExoSnap session start ---"));
        info(QStringLiteral("startup"), QStringLiteral("log path: %1").arg(path));
    }
}

void AppLog::debug(const QString& category, const QString& message) {
    write(LogSeverity::Debug, category, message);
}

void AppLog::info(const QString& category, const QString& message) {
    write(LogSeverity::Info, category, message);
}

void AppLog::warning(const QString& category, const QString& message) {
    write(LogSeverity::Warning, category, message);
}

void AppLog::error(const QString& category, const QString& message) {
    write(LogSeverity::Error, category, message);
}

void AppLog::write(LogSeverity severity, const QString& category, const QString& message) {
    LogEntry entry{
        0, currentTimestamp(), severity, normalizedCategory(category), normalizedMessage(message),
    };

    bool schedule_delivery = false;
    {
        QMutexLocker lock(&state().mutex);
        auto& s = state();

        entry.sequence = s.next_sequence++;
        int evicted_count = 0;
        appendToHistoryUnlocked(entry, &evicted_count);
        s.pending_entries.push_back(entry);
        s.pending_evicted_count += evicted_count;

        writeLineUnlocked(entry);

        if (!s.delivery_scheduled && s.delivery_enabled && QCoreApplication::instance() != nullptr) {
            s.delivery_scheduled = true;
            schedule_delivery = true;
        }
    }

    if (schedule_delivery) {
        QMetaObject::invokeMethod(&instance(), []() { AppLog::instance().deliverPending(); }, Qt::QueuedConnection);
    }
}

QVector<LogEntry> AppLog::history() {
    QMutexLocker lock(&state().mutex);
    QVector<LogEntry> out;
    out.reserve(static_cast<qsizetype>(state().history.size()));
    for (const LogEntry& entry : state().history)
        out.push_back(entry);
    return out;
}

int AppLog::maxEntries() {
    QMutexLocker lock(&state().mutex);
    return state().max_entries;
}

void AppLog::clear() {
    {
        QMutexLocker lock(&state().mutex);
        auto& s = state();
        s.history.clear();
        s.pending_entries.clear();
        s.pending_evicted_count = 0;
        s.delivery_scheduled = false;
    }

    AppLog& log = instance();
    if (QThread::currentThread() == log.thread()) {
        emit log.cleared();
    } else {
        QMetaObject::invokeMethod(&log, []() { emit AppLog::instance().cleared(); }, Qt::QueuedConnection);
    }
}

QString AppLog::logFilePath() {
    QMutexLocker lock(&state().mutex);
    return state().log_path;
}

QString AppLog::severityLabel(LogSeverity severity) {
    switch (severity) {
    case LogSeverity::Debug:
        return QStringLiteral("DEBUG");
    case LogSeverity::Info:
        return QStringLiteral("INFO");
    case LogSeverity::Warning:
        return QStringLiteral("WARNING");
    case LogSeverity::Error:
        return QStringLiteral("ERROR");
    }
    return QStringLiteral("INFO");
}

QString AppLog::severityKey(LogSeverity severity) {
    switch (severity) {
    case LogSeverity::Debug:
        return QStringLiteral("debug");
    case LogSeverity::Info:
        return QStringLiteral("info");
    case LogSeverity::Warning:
        return QStringLiteral("warning");
    case LogSeverity::Error:
        return QStringLiteral("error");
    }
    return QStringLiteral("info");
}

QString AppLog::formatEntry(const LogEntry& entry) {
    QString line = entry.timestamp.toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzz"));
    line += QStringLiteral(" [") + severityLabel(entry.severity) + QStringLiteral("]");
    const QString category = normalizedCategory(entry.category);
    if (!category.isEmpty())
        line += QStringLiteral(" [") + category + QStringLiteral("]");
    line += QStringLiteral(" ") + entry.message;
    return line;
}

bool AppLog::exportHistoryToFile(const QString& path, QString* error) {
    if (path.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("No export path was selected.");
        return false;
    }

    const QVector<LogEntry> entries = history();
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral("Could not write log export: %1").arg(file.errorString());
        return false;
    }

    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    for (const LogEntry& entry : entries)
        stream << formatEntry(entry) << '\n';
    stream.flush();

    if (stream.status() != QTextStream::Ok) {
        if (error)
            *error = QStringLiteral("Could not finish log export.");
        return false;
    }

    return true;
}

void AppLog::resetForTesting(int max_entries) {
    QtMessageHandler previous_handler = nullptr;
    bool restore_qt_handler = false;
    {
        QMutexLocker lock(&state().mutex);
        previous_handler = state().previous_qt_handler;
        restore_qt_handler = state().qt_handler_installed;
        resetUnlocked(max_entries);
        state().previous_qt_handler = nullptr;
        state().qt_handler_installed = false;
    }
    if (restore_qt_handler)
        qInstallMessageHandler(previous_handler);
    emit instance().cleared();
}

void AppLog::setTimestampProviderForTesting(std::function<QDateTime()> provider) {
    QMutexLocker lock(&state().mutex);
    state().timestamp_provider = std::move(provider);
}

void AppLog::deliverPending() {
    QVector<LogEntry> entries;
    int evicted_count = 0;
    {
        QMutexLocker lock(&state().mutex);
        auto& s = state();
        entries = std::move(s.pending_entries);
        s.pending_entries.clear();
        evicted_count = s.pending_evicted_count;
        s.pending_evicted_count = 0;
        s.delivery_scheduled = false;
        if (!s.delivery_enabled || entries.isEmpty())
            return;
    }

    emit entriesAppended(entries, evicted_count);
}

} // namespace exosnap::diagnostics
