#include "EditSessionAdapter.h"
#include "visual_tests/EditVisualMedia.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>

#include <gtest/gtest.h>

#include <filesystem>

namespace exosnap::quick {
namespace {

void EnsureProjectTestApplication() {
    if (QCoreApplication::instance())
        return;
    static int argc = 1;
    static char name[] = "edit_project_test";
    static char* argv[]{name, nullptr};
    static QCoreApplication application(argc, argv);
}

template <typename Condition> bool WaitForProject(Condition done, qint64 timeout_ms = 5000) {
    QElapsedTimer deadline;
    deadline.start();
    while (!done() && deadline.elapsed() < timeout_ms) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return done();
}

TEST(EditSessionProject, SaveOpenAndRecoveryKeepSettingsAndTransactionalReplacement) {
    EnsureProjectTestApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto url = QUrl::fromLocalFile(directory.filePath(QStringLiteral("project.exosnap")));
    const auto draft = directory.filePath(QStringLiteral("editor-recovery/draft.exosnap"));

    {
        EditSessionAdapter session;
        session.enableProjectRecovery(directory.path());
        ASSERT_TRUE(session.setTrackGain(session.workspace().tracks().back().id, -6));
        session.setRipple(true);
        ASSERT_TRUE(session.hasUnsavedEdits());
        session.saveProject(url);
        ASSERT_TRUE(WaitForProject([&] { return !session.projectBusy(); }));
        ASSERT_TRUE(session.projectHasPath());
        EXPECT_FALSE(session.hasUnsavedEdits());
        session.setSnapping(false);
        ASSERT_TRUE(session.hasUnsavedEdits());
        ASSERT_TRUE(WaitForProject([&] { return QFile::exists(draft); }, 10000));
    }

    EditSessionAdapter reopened;
    reopened.enableProjectRecovery(directory.path());
    ASSERT_TRUE(reopened.projectRecoveryAvailable());
    reopened.openProject(url, reopened.closeGuardRevision());
    ASSERT_TRUE(WaitForProject([&] { return !reopened.projectBusy(); }));
    EXPECT_TRUE(reopened.rippleEnabled());
    EXPECT_TRUE(reopened.snappingEnabled());
    EXPECT_EQ(reopened.workspace().tracks().back().gain_db, -6.0);
    EXPECT_FALSE(reopened.hasUnsavedEdits());
    ASSERT_TRUE(reopened.projectRecoveryAvailable());

    reopened.openProject(QUrl::fromLocalFile(directory.filePath(QStringLiteral("missing.exosnap"))),
                         reopened.closeGuardRevision());
    ASSERT_TRUE(WaitForProject([&] { return !reopened.projectBusy(); }));
    EXPECT_TRUE(reopened.projectHasPath());
    EXPECT_TRUE(reopened.rippleEnabled());
    EXPECT_FALSE(reopened.projectMessage().isEmpty());

    reopened.recoverProject(reopened.closeGuardRevision());
    ASSERT_TRUE(WaitForProject([&] { return !reopened.projectBusy(); }));
    EXPECT_FALSE(reopened.projectHasPath());
    EXPECT_FALSE(reopened.projectRecoveryAvailable());
    EXPECT_TRUE(reopened.rippleEnabled());
    EXPECT_FALSE(reopened.snappingEnabled());
    EXPECT_TRUE(reopened.hasUnsavedEdits());
    EXPECT_FALSE(reopened.workspace().canUndo());
    reopened.finishProjectRecovery(true);

    reopened.openProject(url, reopened.closeGuardRevision());
    ASSERT_TRUE(WaitForProject([&] { return !reopened.projectBusy(); }));
    EXPECT_EQ(reopened.workspace().tracks().back().gain_db, -6.0);
    reopened.newProject(reopened.closeGuardRevision());
    EXPECT_EQ(reopened.workspace().tracks().back().gain_db, 0.0);
    EXPECT_FALSE(reopened.projectHasPath());
    EXPECT_FALSE(reopened.hasUnsavedEdits());
}

TEST(EditSessionProject, ReopeningRequiresOriginalBytesAndSupportsExplicitRelink) {
    EnsureProjectTestApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto original = directory.filePath(QStringLiteral("original.wav"));
    const auto source = directory.filePath(QStringLiteral("recording.wav"));
    const auto other_source = directory.filePath(QStringLiteral("second-recording.wav"));
    const auto project = QUrl::fromLocalFile(directory.filePath(QStringLiteral("edited.exosnap")));
    std::string error;
    ASSERT_TRUE(visual_tests::WriteEditVisualAudioFixture(std::filesystem::path(original.toStdWString()), 17, error))
        << error;
    ASSERT_TRUE(
        visual_tests::WriteEditVisualAudioFixture(std::filesystem::path(other_source.toStdWString()), 18, error))
        << error;
    ASSERT_TRUE(QFile::copy(original, source));
    edit::Id asset = 0;
    edit::Id second_asset = 0;
    std::vector<edit::Clip> clips;
    {
        EditSessionAdapter session;
        session.importMediaBatch({QUrl::fromLocalFile(source), QUrl::fromLocalFile(other_source)}, true);
        ASSERT_TRUE(WaitForProject([&] { return !session.importPending(); }));
        ASSERT_EQ(session.workspace().assets().size(), 2U);
        asset = session.workspace().assets().front().id;
        second_asset = session.workspace().assets().back().id;
        ASSERT_TRUE(WaitForProject([&] { return session.sourceProof(asset) && session.sourceProof(second_asset); }));
        clips = session.workspace().clips();
        ASSERT_FALSE(clips.empty());
        session.saveProject(project);
        ASSERT_TRUE(WaitForProject([&] { return !session.projectBusy(); }));
        ASSERT_TRUE(session.projectHasPath());
    }
    {
        EditSessionAdapter session;
        session.openProject(project, session.closeGuardRevision());
        ASSERT_TRUE(WaitForProject([&] { return !session.projectBusy(); }));
        ASSERT_TRUE(session.projectHasPath()) << session.projectMessage().toStdString();
        ASSERT_TRUE(WaitForProject([&] { return session.sourceProof(asset) && session.sourceProof(second_asset); }));
        EXPECT_EQ(session.workspace().clips(), clips);
        EXPECT_EQ(session.workspace().asset(asset)->state, edit::AssetState::Available);
    }
    ASSERT_TRUE(QFile::remove(source));
    ASSERT_TRUE(visual_tests::WriteEditVisualAudioFixture(std::filesystem::path(source.toStdWString()), 19, error))
        << error;
    EditSessionAdapter session;
    session.openProject(project, session.closeGuardRevision());
    // Verification walks the saved library in order. The second proof witnesses
    // that the changed first file has already been checked.
    ASSERT_TRUE(WaitForProject([&] { return bool(session.sourceProof(second_asset)); }));
    EXPECT_EQ(session.workspace().clips(), clips);
    ASSERT_NE(session.workspace().asset(asset), nullptr);
    EXPECT_EQ(session.workspace().asset(asset)->state, edit::AssetState::Missing);
    EXPECT_FALSE(session.sourceAccess(asset));
    session.beginSourceReplacement(asset, QUrl::fromLocalFile(original));
    ASSERT_TRUE(WaitForProject([&] { return session.sourceReplacement().value(QStringLiteral("canApply")).toBool(); }))
        << session.sourceReplacement().value(QStringLiteral("reason")).toString().toStdString();
    ASSERT_TRUE(session.applySourceReplacement());
    ASSERT_TRUE(WaitForProject([&] { return bool(session.sourceProof(asset)); }));
    auto relinked = session.workspace().clips();
    ASSERT_EQ(relinked.size(), clips.size());
    for (size_t i = 0; i < clips.size(); ++i)
        if (relinked[i].asset == asset) {
            ASSERT_TRUE(relinked[i].stream && clips[i].stream);
            relinked[i].stream->stream_identity = clips[i].stream->stream_identity;
        }
    EXPECT_EQ(relinked, clips);
    EXPECT_TRUE(session.hasUnsavedEdits());
}

} // namespace
} // namespace exosnap::quick
