#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QVector>

#include <functional>

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

    // Test support: resets process-local state without writing synthetic entries.
    static void resetForTesting(int max_entries = kDefaultMaxEntries);
    static void setTimestampProviderForTesting(std::function<QDateTime()> provider);

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
