#pragma once

#include "../diagnostics/AppLog.h"

#include <QTextFormat>
#include <QWidget>

class QButtonGroup;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTimer;

namespace exosnap {

namespace ui::widgets {
class ExoCheckBox;
}

#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
namespace visual {
struct VisualScenario;
}
#endif

class LogsPage : public QWidget {
    Q_OBJECT

  public:
    enum class SeverityFilter {
        All,
        Info,
        Issues,
    };

    static constexpr int kSeverityTextProperty = QTextFormat::UserProperty + 41;
    static constexpr int kSeverityBlockProperty = QTextFormat::UserProperty + 42;

    explicit LogsPage(QWidget* parent = nullptr);

    void setSeverityFilter(SeverityFilter filter);
    [[nodiscard]] SeverityFilter severityFilter() const noexcept;
    [[nodiscard]] QString activeFilterName() const;

    void setSearchQuery(const QString& query);
    [[nodiscard]] const QString& searchQuery() const noexcept;

    void setAutoScrollEnabled(bool enabled);
    [[nodiscard]] bool autoScrollEnabled() const;

    [[nodiscard]] int totalEntryCount() const;
    [[nodiscard]] int visibleEntryCount() const;
    [[nodiscard]] QString oldestVisibleSeverityName() const;
    [[nodiscard]] QString newestVisibleSeverityName() const;
    [[nodiscard]] QString copyText() const;
    [[nodiscard]] bool exportToFile(const QString& path, QString* error = nullptr) const;
    void clearLogs();

    [[nodiscard]] int fullRebuildCountForTesting() const noexcept;
    [[nodiscard]] int incrementalAppendCountForTesting() const noexcept;

#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
    void applyVisualScenario(const visual::VisualScenario& scenario);
#endif

  private:
    void onRefresh();
    void onOpenFolder();
    void onCopy();
    void onExport();
    void onClear();
    void onFilterClicked(int id);
    void onSearchTextChanged(const QString& text);
    void applyPendingSearch();
    void onEntriesAppended(QVector<diagnostics::LogEntry> entries, int evicted_count);
    void onLogCleared();

    void rebuildVisibleEntries();
    void appendMatchingEntries(const QVector<diagnostics::LogEntry>& entries);
    void insertEntry(const diagnostics::LogEntry& entry);
    void replaceDocumentFromVisibleEntries();
    void updateActionState();
    void updateStatusLabel(const QString& feedback = {});
    [[nodiscard]] bool matchesActiveFilters(const diagnostics::LogEntry& entry) const;
    [[nodiscard]] QTextCharFormat formatForSeverity(diagnostics::LogSeverity severity) const;

#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
    void setSyntheticEntriesForVisualTest(QVector<diagnostics::LogEntry> entries);
#endif

    QPlainTextEdit* log_viewer_ = nullptr;
    QButtonGroup* severity_group_ = nullptr;
    QLineEdit* search_edit_ = nullptr;
    ui::widgets::ExoCheckBox* auto_scroll_check_ = nullptr;
    // refresh_btn_, open_folder_btn_, clear_btn_ removed (D3: cut from toolbar).
    QPushButton* copy_btn_ = nullptr;
    QPushButton* export_btn_ = nullptr;
    QLabel* folder_link_ = nullptr;
    QLabel* status_label_ = nullptr;
    QTimer* search_debounce_timer_ = nullptr;

    QVector<diagnostics::LogEntry> entries_;
    QVector<diagnostics::LogEntry> visible_entries_;
    SeverityFilter filter_ = SeverityFilter::All;
    QString search_query_;
    QString pending_search_query_;
    bool synthetic_mode_ = false;
    quint64 last_sequence_seen_ = 0;
    int full_rebuild_count_ = 0;
    int incremental_append_count_ = 0;
};

} // namespace exosnap
