#include "EditSessionAdapter.h"
#include "visual_tests/EditVisualMedia.h"

#include <exosnap/engine/edit_timeline_export.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>

#include <gtest/gtest.h>

#include <functional>

namespace exosnap::quick {
namespace {

void EnsureVisualApplication() {
    if (QCoreApplication::instance())
        return;
    static int argc = 1;
    static char name[] = "visual-seeds";
    static char* argv[] = {name, nullptr};
    static QCoreApplication app(argc, argv);
}

bool WaitForVisual(const std::function<bool()>& ready) {
    QElapsedTimer deadline;
    deadline.start();
    while (!ready() && deadline.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    return ready();
}

TEST(EditVisualSeeds, TinyAudioContainerAdmitsARealNamedSourceChoice) {
    EnsureVisualApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("choice.mkv"));
    std::string error;
    ASSERT_TRUE(visual_tests::WriteEditVisualAudioFixture(std::filesystem::path(path.toStdWString()), 17, error))
        << error;
    EditSessionAdapter session;
    session.importMedia(QUrl::fromLocalFile(path), true);
    ASSERT_TRUE(WaitForVisual([&] { return !session.importPending(); }));
    ASSERT_EQ(session.workspace().assets().size(), 1u);
    const auto asset = session.workspace().assets().front().id;
    ASSERT_TRUE(WaitForVisual([&] { return bool(session.sourceProof(asset)); }));
    const auto metadata = session.sourceMetadata(asset);
    ASSERT_NE(metadata, nullptr);
    ASSERT_EQ(metadata->audio_metadata.actual_tracks.size(), 2u);
    EXPECT_EQ(metadata->duration_us, 2000000);
    EXPECT_EQ(session.sourceProof(asset)->Source(), session.sourceAccess(asset));
    ASSERT_TRUE(session.beginSourceAudioChoice(asset));
    const auto choice = session.sourceAudioChoice();
    EXPECT_TRUE(choice.value(QStringLiteral("active")).toBool());
    ASSERT_EQ(choice.value(QStringLiteral("tracks")).toList().size(), 2);
    const auto track = choice.value(QStringLiteral("tracks")).toList().front().toMap();
    EXPECT_FALSE(track.value(QStringLiteral("name")).toString().isEmpty());
    session.setSourceAudioTrack(track.value(QStringLiteral("id")).toString(), true);
    EXPECT_TRUE(session.sourceAudioChoice().value(QStringLiteral("canApply")).toBool());
    session.cancelSourceAudioChoice();
    EXPECT_FALSE(session.sourceAudioChoice().value(QStringLiteral("active")).toBool());
}

TEST(EditVisualSeeds, TinyReplacementReachesTheActualProofBoundReview) {
    EnsureVisualApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto original = directory.filePath(QStringLiteral("original.mkv"));
    const auto replacement = directory.filePath(QStringLiteral("replacement.mkv"));
    std::string error;
    ASSERT_TRUE(visual_tests::WriteEditVisualAudioFixture(std::filesystem::path(original.toStdWString()), 21, error))
        << error;
    ASSERT_TRUE(visual_tests::WriteEditVisualAudioFixture(std::filesystem::path(replacement.toStdWString()), 22, error))
        << error;
    EditSessionAdapter session;
    session.importMedia(QUrl::fromLocalFile(original), true);
    ASSERT_TRUE(WaitForVisual([&] { return !session.importPending(); }));
    ASSERT_EQ(session.workspace().assets().size(), 1u);
    const auto asset = session.workspace().assets().front().id;
    ASSERT_TRUE(WaitForVisual([&] { return bool(session.sourceProof(asset)); }));
    const auto revision = session.workspace().revision();
    const auto access = session.sourceAccess(asset);
    session.beginSourceReplacement(asset, QUrl::fromLocalFile(replacement), true);
    ASSERT_TRUE(WaitForVisual([&] {
        const auto draft = session.sourceReplacement();
        return draft.value(QStringLiteral("active")).toBool() && !draft.value(QStringLiteral("busy")).toBool();
    }));
    const auto draft = session.sourceReplacement();
    EXPECT_TRUE(draft.value(QStringLiteral("active")).toBool());
    EXPECT_TRUE(draft.value(QStringLiteral("reason")).toString().isEmpty());
    EXPECT_EQ(draft.value(QStringLiteral("tracks")).toList().size(), 2);
    EXPECT_EQ(session.workspace().revision(), revision);
    EXPECT_EQ(session.sourceAccess(asset), access);
    session.cancelSourceReplacement();
    EXPECT_FALSE(session.sourceReplacement().value(QStringLiteral("active")).toBool());
}

} // namespace
} // namespace exosnap::quick
