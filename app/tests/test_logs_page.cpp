#include <gtest/gtest.h>

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QThread>

#include "diagnostics/AppLog.h"
#include "pages/LogsPage.h"
#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
#include "visual_tests/VisualScenario.h"
#endif

#include <atomic>
#include <memory>
#include <thread>

namespace exosnap {
namespace {

using diagnostics::AppLog;
using diagnostics::LogEntry;
using diagnostics::LogSeverity;

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;

    static int argc = 1;
    static char app_name[] = "logs_page_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

QDateTime BaseTime() {
    return QDateTime(QDate(2026, 6, 8), QTime(14, 22, 31, 123));
}

void ProcessEvents(int rounds = 6) {
    for (int i = 0; i < rounds; ++i)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

void ResetLog(int capacity = AppLog::kDefaultMaxEntries) {
    AppLog::resetForTesting(capacity);
    auto counter = std::make_shared<std::atomic<int>>(0);
    AppLog::setTimestampProviderForTesting(
        [counter]() { return BaseTime().addMSecs(counter->fetch_add(1, std::memory_order_relaxed)); });
}

void SeedAllLevels() {
    AppLog::debug(QStringLiteral("Preview"), QStringLiteral("debug detail"));
    AppLog::info(QStringLiteral("Record"), QStringLiteral("recording started"));
    AppLog::warning(QStringLiteral("Webcam"), QStringLiteral("device disconnected"));
    AppLog::error(QStringLiteral("Encoder"), QStringLiteral("encoder failed"));
}

QString ReadUtf8File(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readAll());
}

QTextBlock BlockAt(QPlainTextEdit* viewer, int index) {
    return viewer->document()->findBlockByNumber(index);
}

class LogsPageTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }

    void SetUp() override {
        ResetLog();
    }

    void TearDown() override {
        AppLog::resetForTesting();
        ProcessEvents();
    }
};

TEST_F(LogsPageTest, ContainedViewerAndCoreActionsExist) {
    LogsPage page;

    auto* viewer = page.findChild<QPlainTextEdit*>(QStringLiteral("logViewer"));
    ASSERT_NE(viewer, nullptr);
    EXPECT_TRUE(viewer->isReadOnly());
    EXPECT_EQ(viewer->lineWrapMode(), QPlainTextEdit::WidgetWidth);

    // D3: Refresh/Clear/OpenFolder removed; Copy and Export… remain in right cluster.
    EXPECT_EQ(page.findChild<QPushButton*>(QStringLiteral("logRefreshBtn")), nullptr);
    EXPECT_EQ(page.findChild<QPushButton*>(QStringLiteral("logOpenFolderBtn")), nullptr);
    EXPECT_EQ(page.findChild<QPushButton*>(QStringLiteral("logClearBtn")), nullptr);
    EXPECT_NE(page.findChild<QPushButton*>(QStringLiteral("logCopyBtn")), nullptr);
    EXPECT_NE(page.findChild<QPushButton*>(QStringLiteral("logExportBtn")), nullptr);
    EXPECT_NE(page.findChild<QPushButton*>(QStringLiteral("logFilterAllBtn")), nullptr);
    EXPECT_NE(page.findChild<QPushButton*>(QStringLiteral("logFilterInfoBtn")), nullptr);
    EXPECT_NE(page.findChild<QPushButton*>(QStringLiteral("logFilterIssuesBtn")), nullptr);
    // D3: footer folder link present
    auto* folder_lnk = page.findChild<QLabel*>(QStringLiteral("logFolderLink"));
    EXPECT_NE(folder_lnk, nullptr);
}

TEST_F(LogsPageTest, SeverityIsStoredStructurally) {
    AppLog::warning(QStringLiteral("Webcam"), QStringLiteral("device disconnected"));

    const QVector<LogEntry> history = AppLog::history();
    ASSERT_EQ(history.size(), 1);
    EXPECT_EQ(history.front().severity, LogSeverity::Warning);
    EXPECT_FALSE(AppLog::formatEntry(history.front()).contains(QStringLiteral("[warning]")));
}

TEST_F(LogsPageTest, TimestampCategoryAndMessageRoundTrip) {
    AppLog::error(QStringLiteral("Encoder"), QStringLiteral("failed to initialize"));

    const QVector<LogEntry> history = AppLog::history();
    ASSERT_EQ(history.size(), 1);
    EXPECT_EQ(history.front().timestamp, BaseTime());
    EXPECT_EQ(history.front().category, QStringLiteral("Encoder"));
    EXPECT_EQ(history.front().message, QStringLiteral("failed to initialize"));
}

TEST_F(LogsPageTest, ThreadSafeAppendStoresAllEntries) {
    ResetLog(1000);
    std::vector<std::thread> workers;
    for (int t = 0; t < 8; ++t) {
        workers.emplace_back([t]() {
            for (int i = 0; i < 25; ++i) {
                AppLog::info(QStringLiteral("worker"), QStringLiteral("worker %1 entry %2").arg(t).arg(i));
            }
        });
    }
    for (auto& worker : workers)
        worker.join();

    EXPECT_EQ(AppLog::history().size(), 200);
}

TEST_F(LogsPageTest, IncrementalNotificationBatchesAppends) {
    int notifications = 0;
    int received = 0;
    QObject receiver;
    QObject::connect(&AppLog::instance(), &AppLog::entriesAppended, &receiver,
                     [&](QVector<LogEntry> entries, int evicted_count) {
                         ++notifications;
                         received += entries.size();
                         EXPECT_EQ(evicted_count, 0);
                     });

    AppLog::info(QStringLiteral("Record"), QStringLiteral("one"));
    AppLog::info(QStringLiteral("Record"), QStringLiteral("two"));
    ProcessEvents();

    EXPECT_EQ(notifications, 1);
    EXPECT_EQ(received, 2);
}

TEST_F(LogsPageTest, BufferEvictsOldestEntries) {
    ResetLog(3);
    AppLog::info(QStringLiteral("Buffer"), QStringLiteral("one"));
    AppLog::info(QStringLiteral("Buffer"), QStringLiteral("two"));
    AppLog::info(QStringLiteral("Buffer"), QStringLiteral("three"));
    AppLog::info(QStringLiteral("Buffer"), QStringLiteral("four"));

    const QVector<LogEntry> history = AppLog::history();
    ASSERT_EQ(history.size(), 3);
    EXPECT_EQ(history.front().message, QStringLiteral("two"));
    EXPECT_EQ(history.back().message, QStringLiteral("four"));
}

TEST_F(LogsPageTest, ClearResetsBufferAndLoggingAfterClearWorks) {
    AppLog::info(QStringLiteral("Record"), QStringLiteral("before"));
    AppLog::clear();
    EXPECT_TRUE(AppLog::history().isEmpty());

    AppLog::info(QStringLiteral("Record"), QStringLiteral("after"));
    const QVector<LogEntry> history = AppLog::history();
    ASSERT_EQ(history.size(), 1);
    EXPECT_EQ(history.front().message, QStringLiteral("after"));
}

TEST_F(LogsPageTest, FiltersImplementAllInfoAndIssuesSemantics) {
    SeedAllLevels();
    ProcessEvents();
    LogsPage page;

    page.setSeverityFilter(LogsPage::SeverityFilter::All);
    EXPECT_EQ(page.visibleEntryCount(), 4);

    page.setSeverityFilter(LogsPage::SeverityFilter::Info);
    ASSERT_EQ(page.visibleEntryCount(), 1);
    EXPECT_TRUE(page.copyText().contains(QStringLiteral("[INFO]")));
    EXPECT_FALSE(page.copyText().contains(QStringLiteral("[DEBUG]")));

    page.setSeverityFilter(LogsPage::SeverityFilter::Issues);
    EXPECT_EQ(page.visibleEntryCount(), 2);
    EXPECT_TRUE(page.copyText().contains(QStringLiteral("[WARNING]")));
    EXPECT_TRUE(page.copyText().contains(QStringLiteral("[ERROR]")));
    EXPECT_FALSE(page.copyText().contains(QStringLiteral("[DEBUG]")));
    EXPECT_FALSE(page.copyText().contains(QStringLiteral("[INFO]")));
}

TEST_F(LogsPageTest, SearchIsCaseInsensitiveAndMatchesCategoryAndMessage) {
    AppLog::info(QStringLiteral("Webcam"), QStringLiteral("frame ready"));
    AppLog::warning(QStringLiteral("Preview"), QStringLiteral("WEBCAM overlay unavailable"));
    AppLog::error(QStringLiteral("Encoder"), QStringLiteral("failed"));
    ProcessEvents();
    LogsPage page;

    page.setSearchQuery(QStringLiteral("webcam"));
    EXPECT_EQ(page.visibleEntryCount(), 2);
    EXPECT_TRUE(page.copyText().contains(QStringLiteral("[Webcam]")));
    EXPECT_TRUE(page.copyText().contains(QStringLiteral("WEBCAM overlay")));
}

TEST_F(LogsPageTest, SearchCombinesWithSeverity) {
    AppLog::info(QStringLiteral("Webcam"), QStringLiteral("frame ready"));
    AppLog::warning(QStringLiteral("Webcam"), QStringLiteral("device disconnected"));
    AppLog::error(QStringLiteral("Encoder"), QStringLiteral("webcam encoder path failed"));
    ProcessEvents();
    LogsPage page;

    page.setSeverityFilter(LogsPage::SeverityFilter::Issues);
    page.setSearchQuery(QStringLiteral("webcam"));
    EXPECT_EQ(page.visibleEntryCount(), 2);
    EXPECT_FALSE(page.copyText().contains(QStringLiteral("[INFO]")));
}

TEST_F(LogsPageTest, CopyUsesCurrentlyVisibleEntries) {
    SeedAllLevels();
    ProcessEvents();
    LogsPage page;
    page.setSeverityFilter(LogsPage::SeverityFilter::Issues);

    const QString copied = page.copyText();
    EXPECT_TRUE(copied.contains(QStringLiteral("[WARNING] [Webcam] device disconnected")));
    EXPECT_TRUE(copied.contains(QStringLiteral("[ERROR] [Encoder] encoder failed")));
    EXPECT_FALSE(copied.contains(QStringLiteral("[INFO]")));
}

TEST_F(LogsPageTest, CopyButtonWritesVisibleEntriesToClipboard) {
    SeedAllLevels();
    ProcessEvents();
    LogsPage page;
    page.setSeverityFilter(LogsPage::SeverityFilter::Info);

    auto* copy = page.findChild<QPushButton*>(QStringLiteral("logCopyBtn"));
    ASSERT_NE(copy, nullptr);

    const QString previous_clipboard = QGuiApplication::clipboard()->text();
    copy->click();

    EXPECT_EQ(QGuiApplication::clipboard()->text(), page.copyText());
    QGuiApplication::clipboard()->setText(previous_clipboard);
}

TEST_F(LogsPageTest, ExportUsesCompleteHistoryAndDeterministicUtf8Formatting) {
    AppLog::info(QStringLiteral("Record"), QString::fromUtf8("caf\xC3\xA9 ready"));
    AppLog::warning(QStringLiteral("Webcam"), QStringLiteral("device disconnected"));
    ProcessEvents();
    LogsPage page;
    page.setSeverityFilter(LogsPage::SeverityFilter::Info);
    ASSERT_EQ(page.visibleEntryCount(), 1);

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.path() + QStringLiteral("/export.txt");
    QString error;
    ASSERT_TRUE(page.exportToFile(path, &error)) << error.toStdString();

    const QString exported = ReadUtf8File(path);
    EXPECT_TRUE(exported.contains(QStringLiteral("2026-06-08T14:22:31.123 [INFO] [Record]")));
    EXPECT_TRUE(exported.contains(QString::fromUtf8("caf\xC3\xA9 ready")));
    EXPECT_TRUE(exported.contains(QStringLiteral("[WARNING] [Webcam] device disconnected")));
}

TEST_F(LogsPageTest, WarningAndErrorStylingPropertiesAreSet) {
    AppLog::warning(QStringLiteral("Webcam"), QStringLiteral("device disconnected"));
    AppLog::error(QStringLiteral("Encoder"), QStringLiteral("encoder failed"));
    ProcessEvents();
    LogsPage page;

    auto* viewer = page.findChild<QPlainTextEdit*>(QStringLiteral("logViewer"));
    ASSERT_NE(viewer, nullptr);
    EXPECT_EQ(BlockAt(viewer, 0).blockFormat().property(LogsPage::kSeverityBlockProperty).toString(),
              QStringLiteral("warning"));
    EXPECT_EQ(BlockAt(viewer, 1).blockFormat().property(LogsPage::kSeverityBlockProperty).toString(),
              QStringLiteral("error"));
}

TEST_F(LogsPageTest, AutoScrollRemainsAtBottomWhenEnabled) {
    ResetLog(300);
    for (int i = 0; i < 120; ++i)
        AppLog::info(QStringLiteral("Record"), QStringLiteral("entry %1").arg(i));
    ProcessEvents();

    LogsPage page;
    page.resize(720, 360);
    page.show();
    ProcessEvents();
    page.setAutoScrollEnabled(true);

    AppLog::info(QStringLiteral("Record"), QStringLiteral("tail"));
    ProcessEvents();

    auto* viewer = page.findChild<QPlainTextEdit*>(QStringLiteral("logViewer"));
    ASSERT_NE(viewer, nullptr);
    EXPECT_EQ(viewer->verticalScrollBar()->value(), viewer->verticalScrollBar()->maximum());
    page.hide();
}

TEST_F(LogsPageTest, AutoScrollDisabledDoesNotForcePosition) {
    ResetLog(300);
    for (int i = 0; i < 120; ++i)
        AppLog::info(QStringLiteral("Record"), QStringLiteral("entry %1").arg(i));
    ProcessEvents();

    LogsPage page;
    page.resize(720, 360);
    page.show();
    ProcessEvents();
    page.setAutoScrollEnabled(false);

    auto* viewer = page.findChild<QPlainTextEdit*>(QStringLiteral("logViewer"));
    ASSERT_NE(viewer, nullptr);
    viewer->verticalScrollBar()->setValue(0);
    AppLog::info(QStringLiteral("Record"), QStringLiteral("tail"));
    ProcessEvents();

    EXPECT_EQ(viewer->verticalScrollBar()->value(), 0);
    page.hide();
}

TEST_F(LogsPageTest, BurstLoggingDoesNotRebuildCompleteWidgetPerEntry) {
    LogsPage page;
    const int rebuilds_before = page.fullRebuildCountForTesting();
    for (int i = 0; i < 200; ++i)
        AppLog::info(QStringLiteral("Burst"), QStringLiteral("entry %1").arg(i));
    ProcessEvents();

    EXPECT_EQ(page.fullRebuildCountForTesting(), rebuilds_before);
    EXPECT_EQ(page.incrementalAppendCountForTesting(), 1);
    EXPECT_EQ(page.visibleEntryCount(), 200);
}

TEST_F(LogsPageTest, WorkerThreadLoggingReachesUiSafely) {
    LogsPage page;
    std::thread worker([]() { AppLog::warning(QStringLiteral("Worker"), QStringLiteral("worker warning")); });
    worker.join();
    ProcessEvents();

    EXPECT_TRUE(page.copyText().contains(QStringLiteral("[WARNING] [Worker] worker warning")));
}

TEST_F(LogsPageTest, ShutdownWithPendingLogsIsSafe) {
    auto* page = new LogsPage();
    AppLog::info(QStringLiteral("Record"), QStringLiteral("pending"));
    delete page;
    ProcessEvents();
    SUCCEED();
}

TEST_F(LogsPageTest, ExportFailureDoesNotRecurseIntoLogHistory) {
    AppLog::info(QStringLiteral("Record"), QStringLiteral("before export"));
    ProcessEvents();
    LogsPage page;

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString missing_parent = dir.path() + QStringLiteral("/missing/export.txt");
    const int before = AppLog::history().size();
    QString error;
    EXPECT_FALSE(page.exportToFile(missing_parent, &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(AppLog::history().size(), before);
}

TEST_F(LogsPageTest, PendingSnapshotEntriesAreNotDuplicatedWhenPageSubscribes) {
    AppLog::info(QStringLiteral("Startup"), QStringLiteral("queued before page"));
    LogsPage page;
    ProcessEvents();

    EXPECT_EQ(page.visibleEntryCount(), 1);
    EXPECT_EQ(page.totalEntryCount(), 1);
}

#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
TEST_F(LogsPageTest, VisualTestEntriesNeverPersist) {
    LogsPage page;
    visual::VisualScenario scenario;
    scenario.id = QStringLiteral("logs-all-levels");
    scenario.page = visual::VisualPage::Logs;
    scenario.log_filter = visual::VisualLogFilter::All;

    page.applyVisualScenario(scenario);

    EXPECT_EQ(page.totalEntryCount(), 4);
    EXPECT_TRUE(AppLog::history().isEmpty());
}
#endif

} // namespace
} // namespace exosnap
