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
    // Kept open for the lifetime of the process (or until rotation) instead of
    // being opened and closed on every single line: long sessions were paying an
    // open/write/flush/close syscall round trip per log line for no durability
    // benefit, since flush() alone already pushes each line to the OS.
    QFile log_file;
    qint64 log_file_size = 0;
    std::optional<qint64> max_log_file_bytes_override;
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
    // SETTINGS-HONESTY-R1: nullopt = "Off" (record nothing); otherwise the minimum
    // severity (inclusive) that gets recorded. Default = record everything, so
    // behavior is unchanged until something explicitly narrows the filter.
    std::optional<LogSeverity> min_severity = LogSeverity::Debug;
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

bool passesMinSeverity(LogSeverity severity) {
    QMutexLocker lock(&state().mutex);
    const auto& min = state().min_severity;
    if (!min.has_value())
        return false; // "Off": nothing recorded
    return static_cast<int>(severity) >= static_cast<int>(*min);
}

qint64 maxLogFileBytesUnlocked() {
    auto& s = state();
    return s.max_log_file_bytes_override.value_or(AppLog::kMaxLogFileBytes);
}

// Backup path for rotation slot `index`: 0 is the live file (exosnap.log), 1 is
// exosnap.log.1 (previous), up to kMaxLogFileCount - 1 (oldest kept backup).
QString rotationPathUnlocked(int index) {
    auto& s = state();
    return index == 0 ? s.log_path : s.log_path + QStringLiteral(".%1").arg(index);
}

// Closes the live file, drops the oldest backup, shifts the remaining backups
// up by one slot, then reopens a fresh, empty exosnap.log. Windows refuses to
// rename a file that is still open, so the handle must be closed first; nothing
// else in the process holds the log file open (the Logs page reads the
// in-memory history, not the file), so this is safe without extra locking
// beyond the existing state mutex.
void rotateLogFileUnlocked() {
    auto& s = state();
    s.log_file.close();

    QFile::remove(rotationPathUnlocked(AppLog::kMaxLogFileCount - 1));
    for (int index = AppLog::kMaxLogFileCount - 2; index >= 0; --index) {
        const QString from = rotationPathUnlocked(index);
        if (QFile::exists(from))
            QFile::rename(from, rotationPathUnlocked(index + 1));
    }

    s.log_file.setFileName(s.log_path);
    s.log_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
    s.log_file_size = 0;
}

bool ensureLogFileOpenUnlocked() {
    auto& s = state();
    if (s.log_file.isOpen())
        return true;

    s.log_file.setFileName(s.log_path);
    if (!s.log_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return false;

    // A previous run (or process restart) may have left an oversized file
    // behind, since the file name is stable across sessions; rotate immediately
    // so this run's first line lands in a fresh file rather than appending
    // indefinitely to whatever was already there.
    s.log_file_size = s.log_file.size();
    if (s.log_file_size >= maxLogFileBytesUnlocked())
        rotateLogFileUnlocked();

    return true;
}

bool writeLineUnlocked(const LogEntry& entry) {
    auto& s = state();
    if (s.log_path.isEmpty())
        return true;

    if (!ensureLogFileOpenUnlocked())
        return false;

    const QByteArray line = (AppLog::formatEntry(entry) + QLatin1Char('\n')).toUtf8();
    const qint64 written = s.log_file.write(line);
    if (written < 0)
        return false;

    // Crash-safety: this is the support-log channel, so every line is flushed
    // individually (a qFatal -> abort() must not lose the lines leading up to
    // it) even though the handle now stays open across writes.
    s.log_file.flush();
    s.log_file_size += written;

    if (s.log_file_size >= maxLogFileBytesUnlocked())
        rotateLogFileUnlocked();

    return true;
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
    s.log_file.close();
    s.log_file.setFileName(QString());
    s.log_file_size = 0;
    s.max_log_file_bytes_override.reset();
    s.initialized = false;
    s.timestamp_provider = nullptr;
    s.min_severity = LogSeverity::Debug;
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
    if (!passesMinSeverity(severity))
        return;

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

void AppLog::setMaxLogFileBytesForTesting(std::optional<qint64> max_bytes) {
    QMutexLocker lock(&state().mutex);
    state().max_log_file_bytes_override = max_bytes;
}

void AppLog::setMinSeverity(std::optional<LogSeverity> min_severity) {
    QMutexLocker lock(&state().mutex);
    state().min_severity = min_severity;
}

std::optional<LogSeverity> AppLog::minSeverity() {
    QMutexLocker lock(&state().mutex);
    return state().min_severity;
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
