#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QVector>

#include <functional>
#include <optional>

namespace exosnap::diagnostics {

enum class LogSeverity {
    Debug,
    Info,
    Warning,
    Error,
};

struct LogEntry {
    quint64 sequence = 0;
    QDateTime timestamp;
    LogSeverity severity = LogSeverity::Info;
    QString category;
    QString message;
};

class AppLog final : public QObject {
    Q_OBJECT

  public:
    static constexpr int kDefaultMaxEntries = 5000;

    // Size-based rotation for the on-disk session log file (exosnap.log).
    //
    // 5 MiB holds several hours of a chatty (Debug-level) session as plain text
    // while staying trivial to attach to a support request; 3 files (current +
    // 2 backups, exosnap.log[.1][.2], ~15 MiB worst case) keep enough history to
    // diagnose a problem reported after the fact without the file growing
    // unbounded across a long-running or multi-day session. Not user-configurable:
    // this is an implementation bound, not a product setting.
    static constexpr qint64 kMaxLogFileBytes = 5 * 1024 * 1024;
    static constexpr int kMaxLogFileCount = 3;

    static AppLog& instance();

    // One-time init: creates the log directory and writes the startup banner.
    // Safe to call before QApplication::exec() but requires QCoreApplication to exist.
    static void init();

    static void debug(const QString& category, const QString& message);
    static void info(const QString& category, const QString& message);
    static void warning(const QString& category, const QString& message);
    static void error(const QString& category, const QString& message);
    static void write(LogSeverity severity, const QString& category, const QString& message);

    [[nodiscard]] static QVector<LogEntry> history();
    [[nodiscard]] static int maxEntries();
    static void clear();

    // Returns the absolute path to the session log file.
    // Returns an empty string if init() has not been called.
    [[nodiscard]] static QString logFilePath();

    [[nodiscard]] static QString severityLabel(LogSeverity severity);
    [[nodiscard]] static QString severityKey(LogSeverity severity);
    [[nodiscard]] static QString formatEntry(const LogEntry& entry);
    [[nodiscard]] static bool exportHistoryToFile(const QString& path, QString* error = nullptr);

    // SETTINGS-HONESTY-R1: developer log-level filter (Settings > Advanced > Developer
    // card). Controls which severities are recorded into the in-app Logs history and the
    // session log file. nullopt means "Off" (nothing is recorded); otherwise the value is
    // the minimum severity (inclusive) that gets recorded. Default (and resetForTesting())
    // is LogSeverity::Debug, i.e. record everything -- unaffected until MainWindow applies
    // the persisted developer log-level at startup. Independent of Qt's message-handler
    // forwarding to any previously-installed handler (and the QtFatalMsg abort), which
    // always still runs regardless of this filter.
    static void setMinSeverity(std::optional<LogSeverity> min_severity);
    [[nodiscard]] static std::optional<LogSeverity> minSeverity();

    // Test support: resets process-local state without writing synthetic entries.
    static void resetForTesting(int max_entries = kDefaultMaxEntries);
    static void setTimestampProviderForTesting(std::function<QDateTime()> provider);

    // Test support: overrides the rotation threshold so rotation tests don't need
    // to write megabytes of lines. Pass std::nullopt to restore kMaxLogFileBytes.
    static void setMaxLogFileBytesForTesting(std::optional<qint64> max_bytes);

  signals:
    void entriesAppended(QVector<exosnap::diagnostics::LogEntry> entries, int evicted_count);
    void cleared();

  private:
    explicit AppLog(QObject* parent = nullptr);

    void deliverPending();
};

} // namespace exosnap::diagnostics

Q_DECLARE_METATYPE(exosnap::diagnostics::LogEntry)
Q_DECLARE_METATYPE(QVector<exosnap::diagnostics::LogEntry>)
