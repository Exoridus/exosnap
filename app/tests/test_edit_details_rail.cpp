#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QLabel>
#include <QWidget>

#include "ui/widgets/EditDetailsRail.h"

namespace exosnap::ui::widgets {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "edit_details_rail_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class EditDetailsRailTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

TEST_F(EditDetailsRailTest, CarriesItsObjectName) {
    QWidget host;
    EditDetailsRail rail(&host);
    EXPECT_EQ(rail.objectName(), QStringLiteral("editDetailsRail"));
}

// The 280 px column width belongs to the host, which insets this card with its
// own margins — a fixed width here would push the card past that column.
TEST_F(EditDetailsRailTest, DoesNotImposeItsOwnFixedWidth) {
    QWidget host;
    EditDetailsRail rail(&host);
    EXPECT_NE(rail.minimumWidth(), rail.maximumWidth());
}

TEST_F(EditDetailsRailTest, TitleCarriesItsObjectNameAndText) {
    QWidget host;
    EditDetailsRail rail(&host);
    auto* title = rail.findChild<QLabel*>(QStringLiteral("editDetailsRailTitle"));
    ASSERT_NE(title, nullptr);
    EXPECT_EQ(title->text(), QStringLiteral("Details"));
}

TEST_F(EditDetailsRailTest, AllSevenValueLabelsExistAndDefaultToTheEmptyGlyph) {
    QWidget host;
    EditDetailsRail rail(&host);
    const QString em_dash = QStringLiteral("\xe2\x80\x94");

    for (const QString& name :
         {QStringLiteral("factDurationValue"), QStringLiteral("factSizeValue"), QStringLiteral("factResolutionValue"),
          QStringLiteral("factFpsValue"), QStringLiteral("factVideoValue"), QStringLiteral("factAudioValue"),
          QStringLiteral("factContainerValue")}) {
        auto* label = rail.findChild<QLabel*>(name);
        ASSERT_NE(label, nullptr) << name.toStdString();
        EXPECT_EQ(label->text(), em_dash) << name.toStdString();
    }
}

TEST_F(EditDetailsRailTest, SetFacts_FillsAllSevenRows) {
    QWidget host;
    EditDetailsRail rail(&host);

    EditDetailsRail::Facts facts;
    facts.duration = QStringLiteral("00:04:18");
    facts.size = QStringLiteral("812.4 MB");
    facts.resolution = QStringLiteral("2560 x 1440");
    facts.fps = QStringLiteral("60 fps CFR");
    facts.video_codec = QStringLiteral("AV1");
    facts.audio_codec = QStringLiteral("Opus");
    facts.container = QStringLiteral("MKV");
    rail.setFacts(facts);

    EXPECT_EQ(rail.findChild<QLabel*>(QStringLiteral("factDurationValue"))->text(), facts.duration);
    EXPECT_EQ(rail.findChild<QLabel*>(QStringLiteral("factSizeValue"))->text(), facts.size);
    EXPECT_EQ(rail.findChild<QLabel*>(QStringLiteral("factResolutionValue"))->text(), facts.resolution);
    EXPECT_EQ(rail.findChild<QLabel*>(QStringLiteral("factFpsValue"))->text(), facts.fps);
    EXPECT_EQ(rail.findChild<QLabel*>(QStringLiteral("factVideoValue"))->text(), facts.video_codec);
    EXPECT_EQ(rail.findChild<QLabel*>(QStringLiteral("factAudioValue"))->text(), facts.audio_codec);
    EXPECT_EQ(rail.findChild<QLabel*>(QStringLiteral("factContainerValue"))->text(), facts.container);
}

TEST_F(EditDetailsRailTest, SetFacts_EmptyFieldsFallBackToTheEmptyGlyph) {
    QWidget host;
    EditDetailsRail rail(&host);

    EditDetailsRail::Facts facts; // all fields default-empty
    rail.setFacts(facts);

    const QString em_dash = QStringLiteral("\xe2\x80\x94");
    EXPECT_EQ(rail.findChild<QLabel*>(QStringLiteral("factDurationValue"))->text(), em_dash);
    EXPECT_EQ(rail.findChild<QLabel*>(QStringLiteral("factContainerValue"))->text(), em_dash);
}

TEST_F(EditDetailsRailTest, ValueLabelsAreRightAligned) {
    QWidget host;
    EditDetailsRail rail(&host);
    auto* value = rail.findChild<QLabel*>(QStringLiteral("factDurationValue"));
    ASSERT_NE(value, nullptr);
    EXPECT_TRUE(value->alignment() & Qt::AlignRight);
}

TEST_F(EditDetailsRailTest, ApplyThemeStyles_DoesNotCrash) {
    QWidget host;
    EditDetailsRail rail(&host);
    rail.applyThemeStyles();
    rail.applyThemeStyles(); // idempotent — re-applying on a theme switch must be safe
}

} // namespace
} // namespace exosnap::ui::widgets
