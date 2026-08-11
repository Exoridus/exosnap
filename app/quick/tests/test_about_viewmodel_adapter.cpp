#include "AboutViewModelAdapter.h"

#include <gtest/gtest.h>

#include <QClipboard>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>

namespace exosnap::quick {
namespace {

QGuiApplication* ensureApplication() {
    if (auto* existing = qobject_cast<QGuiApplication*>(QCoreApplication::instance()))
        return existing;

    static int argc = 1;
    static char app_name[] = "about_viewmodel_qml_adapter_tests";
    static char* argv[] = {app_name, nullptr};
    static QGuiApplication app(argc, argv);
    return &app;
}

template <typename Predicate> bool pumpUntil(Predicate&& predicate, int timeout_ms = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate()) {
        if (timer.hasExpired(timeout_ms))
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

models::AboutInfo makeInfo() {
    models::AboutInfo info;
    info.version = QStringLiteral("0.9.0-dev");
    info.commit_short = QStringLiteral("abc1234");
    info.commit_full = QStringLiteral("abc123456789");
    info.build_timestamp_utc = QStringLiteral("2026-08-09T12:00:00Z");
    info.built_display = QStringLiteral("2026-08-09 12:00 UTC");
    info.build_id = QStringLiteral("42");
    info.configuration = QStringLiteral("Debug");
    info.install_mode_label = QStringLiteral("Portable");
    info.channel = QStringLiteral("Preview");
    info.author = QStringLiteral("Exoridus");
    info.description = QStringLiteral("Description");
    info.github_url = QStringLiteral("https://github.com/Exoridus/exosnap");
    info.author_url = QStringLiteral("https://github.com/Exoridus");
    info.release_notes_url = QStringLiteral("https://github.com/Exoridus/exosnap/releases");
    info.commit_url = QStringLiteral("https://github.com/Exoridus/exosnap/commit/abc123456789");
    info.official_build = false;
    info.dirty_source_tree = true;
    info.debug_build = true;
    return info;
}

class AboutViewModelAdapterTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        ensureApplication();
    }
};

TEST_F(AboutViewModelAdapterTest, ExposesCanonicalAboutSnapshot) {
    AboutViewModelAdapter adapter(makeInfo());

    EXPECT_EQ(adapter.version(), QStringLiteral("0.9.0-dev"));
    EXPECT_EQ(adapter.commitShort(), QStringLiteral("abc1234"));
    EXPECT_EQ(adapter.builtDisplay(), QStringLiteral("2026-08-09 12:00 UTC"));
    EXPECT_EQ(adapter.installMode(), QStringLiteral("Portable"));
    EXPECT_EQ(adapter.channel(), QStringLiteral("Preview"));
    EXPECT_TRUE(adapter.commitAvailable());
    EXPECT_TRUE(adapter.unofficialBuild());
    EXPECT_TRUE(adapter.debugBuild());
    EXPECT_TRUE(adapter.dirtySourceTree());
}

TEST_F(AboutViewModelAdapterTest, MissingCommitUrlDisablesCommitAction) {
    models::AboutInfo info = makeInfo();
    info.commit_url.clear();

    AboutViewModelAdapter adapter(std::move(info));

    EXPECT_FALSE(adapter.commitAvailable());
}

TEST_F(AboutViewModelAdapterTest, CopyDetailsHashesTheExecutableAndPublishesCanonicalText) {
    AboutViewModelAdapter adapter(makeInfo());
    QString copied_text;
    int copied_count = 0;
    QObject::connect(&adapter, &AboutViewModelAdapter::detailsCopied, &adapter,
                     [&copied_text, &copied_count](const QString& text) {
                         copied_text = text;
                         ++copied_count;
                     });

    adapter.copyDetails();

    ASSERT_TRUE(pumpUntil([&adapter]() { return !adapter.copying(); }));
    ASSERT_TRUE(pumpUntil([&copied_count]() { return copied_count == 1; }));
    EXPECT_TRUE(copied_text.contains(QStringLiteral("Version: 0.9.0-dev")));
    EXPECT_TRUE(copied_text.contains(QStringLiteral("Update channel: Preview")));
    EXPECT_TRUE(copied_text.contains(QStringLiteral("Executable SHA-256: ")));
    EXPECT_EQ(QGuiApplication::clipboard()->text(), copied_text);
    EXPECT_EQ(adapter.copyStatusText(), QStringLiteral("Details copied."));

    adapter.copyDetails();

    EXPECT_EQ(copied_count, 2);
    EXPECT_FALSE(adapter.copying());
}

} // namespace
} // namespace exosnap::quick
