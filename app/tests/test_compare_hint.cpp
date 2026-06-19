#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QObject>
#include <QString>
#include <QToolButton>
#include <vector>

#include "models/SettingsCompareData.h"
#include "ui/widgets/CompareHint.h"

namespace exosnap {
namespace {

// ── QApplication fixture (same pattern as test_info_hint_icon.cpp) ──────────

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;

    static int argc = 1;
    static char app_name[] = "compare_hint_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class CompareHintTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

// Manual signal recorder — avoids Qt6::Test / QSignalSpy dependency
// (same pattern as test_audio_device_notifier.cpp).
struct OptionSelectedSink {
    std::vector<QString> calls;

    void connect(ui::widgets::CompareHint* hint) {
        QObject::connect(hint, &ui::widgets::CompareHint::optionSelected,
                         [this](const QString& value) { calls.push_back(value); });
    }

    int count() const {
        return static_cast<int>(calls.size());
    }
    const QString& last() const {
        return calls.back();
    }
};

// ── SettingsCompareData tests ────────────────────────────────────────────────

TEST_F(CompareHintTest, Data_Container_HasThreeOptions) {
    const auto* d = ui::compare::compareData(QStringLiteral("container"));
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->options.size(), 3);
}

TEST_F(CompareHintTest, Data_Container_OrderIsMkvWebmMp4) {
    const auto* d = ui::compare::compareData(QStringLiteral("container"));
    ASSERT_NE(d, nullptr);
    ASSERT_EQ(d->options.size(), 3);
    EXPECT_EQ(d->options[0].value, QStringLiteral("MKV"));
    EXPECT_EQ(d->options[1].value, QStringLiteral("WebM"));
    EXPECT_EQ(d->options[2].value, QStringLiteral("MP4"));
}

TEST_F(CompareHintTest, Data_Container_MkvIsRecommended) {
    const auto* d = ui::compare::compareData(QStringLiteral("container"));
    ASSERT_NE(d, nullptr);
    EXPECT_TRUE(d->options[0].recommended) << "MKV must be recommended";
    EXPECT_FALSE(d->options[1].recommended) << "WebM must NOT be recommended";
    EXPECT_FALSE(d->options[2].recommended) << "MP4 must NOT be recommended";
}

TEST_F(CompareHintTest, Data_AudioCodec_HasFourOptions) {
    const auto* d = ui::compare::compareData(QStringLiteral("audioCodec"));
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->options.size(), 4);
}

TEST_F(CompareHintTest, Data_AudioCodec_PcmAndFlacHaveTier06) {
    const auto* d = ui::compare::compareData(QStringLiteral("audioCodec"));
    ASSERT_NE(d, nullptr);
    bool foundPcm = false;
    bool foundFlac = false;
    for (const auto& opt : d->options) {
        if (opt.value == QLatin1String("PCM")) {
            EXPECT_EQ(opt.tier, QStringLiteral("0.6")) << "PCM tier must be '0.6'";
            foundPcm = true;
        }
        if (opt.value == QLatin1String("FLAC")) {
            EXPECT_EQ(opt.tier, QStringLiteral("0.6")) << "FLAC tier must be '0.6'";
            foundFlac = true;
        }
    }
    EXPECT_TRUE(foundPcm) << "PCM must be present in audioCodec options";
    EXPECT_TRUE(foundFlac) << "FLAC must be present in audioCodec options";
}

TEST_F(CompareHintTest, Data_UnknownKey_ReturnsNullptr) {
    EXPECT_EQ(ui::compare::compareData(QStringLiteral("unknownKey")), nullptr);
    EXPECT_EQ(ui::compare::compareData(QString()), nullptr);
}

TEST_F(CompareHintTest, Data_Container_MkvEffectVerbatim) {
    const auto* d = ui::compare::compareData(QStringLiteral("container"));
    ASSERT_NE(d, nullptr);
    ASSERT_FALSE(d->options.isEmpty());
    // Verbatim check — "\xC2\xB7" is U+00B7 MIDDLE DOT.
    EXPECT_EQ(d->options[0].effect, QStringLiteral("Crash-resistant \xC2\xB7 most codec support"))
        << "MKV effect line must match design verbatim";
}

// ── CompareHint widget tests ──────────────────────────────────────────────────

TEST_F(CompareHintTest, Widget_CompareKey_MatchesConstructor) {
    ui::widgets::CompareHint hint(QStringLiteral("container"), QStringLiteral("MKV"));
    EXPECT_EQ(hint.compareKey(), QStringLiteral("container"));
}

TEST_F(CompareHintTest, Widget_CurrentValue_MatchesConstructor) {
    ui::widgets::CompareHint hint(QStringLiteral("container"), QStringLiteral("MKV"));
    EXPECT_EQ(hint.currentValue(), QStringLiteral("MKV"));
}

TEST_F(CompareHintTest, Widget_SetCurrentValue_UpdatesValue) {
    ui::widgets::CompareHint hint(QStringLiteral("container"), QStringLiteral("MKV"));
    hint.setCurrentValue(QStringLiteral("MP4"));
    EXPECT_EQ(hint.currentValue(), QStringLiteral("MP4"));
}

TEST_F(CompareHintTest, Widget_UnknownKey_DoesNotCrash) {
    EXPECT_NO_FATAL_FAILURE({
        ui::widgets::CompareHint hint(QStringLiteral("unknownKey"), QStringLiteral("Foo"));
        EXPECT_EQ(hint.compareKey(), QStringLiteral("unknownKey"));
        EXPECT_EQ(hint.currentValue(), QStringLiteral("Foo"));
    });
}

TEST_F(CompareHintTest, Widget_IsQToolButton) {
    ui::widgets::CompareHint hint(QStringLiteral("container"), QStringLiteral("MKV"));
    EXPECT_NE(qobject_cast<QToolButton*>(&hint), nullptr);
}

TEST_F(CompareHintTest, Widget_AutoRaise_IsTrue) {
    ui::widgets::CompareHint hint(QStringLiteral("container"), QStringLiteral("MKV"));
    EXPECT_TRUE(hint.autoRaise());
}

TEST_F(CompareHintTest, Widget_FocusPolicy_IsTabFocus) {
    ui::widgets::CompareHint hint(QStringLiteral("container"), QStringLiteral("MKV"));
    EXPECT_EQ(hint.focusPolicy(), Qt::TabFocus);
}

TEST_F(CompareHintTest, Widget_HasIcon_AtConstruction) {
    ui::widgets::CompareHint hint(QStringLiteral("container"), QStringLiteral("MKV"));
    EXPECT_FALSE(hint.icon().isNull()) << "CompareHint must have an icon at construction";
}

TEST_F(CompareHintTest, Widget_OptionSelected_EmittedViaSignalSink) {
    ui::widgets::CompareHint hint(QStringLiteral("container"), QStringLiteral("MKV"));
    OptionSelectedSink sink;
    sink.connect(&hint);

    // Emit directly to verify the signal is wired correctly.
    emit hint.optionSelected(QStringLiteral("WebM"));
    ASSERT_EQ(sink.count(), 1);
    EXPECT_EQ(sink.last(), QStringLiteral("WebM"));
}

TEST_F(CompareHintTest, Widget_OptionSelected_SetCurrentValueUpdatesInternally) {
    ui::widgets::CompareHint hint(QStringLiteral("container"), QStringLiteral("MKV"));
    OptionSelectedSink sink;
    sink.connect(&hint);

    // Simulate the internal path that the option row click triggers:
    // setCurrentValue + emit optionSelected.
    hint.setCurrentValue(QStringLiteral("MP4"));
    emit hint.optionSelected(QStringLiteral("MP4"));

    ASSERT_EQ(sink.count(), 1);
    EXPECT_EQ(sink.last(), QStringLiteral("MP4"));
    EXPECT_EQ(hint.currentValue(), QStringLiteral("MP4"));
}

TEST_F(CompareHintTest, Widget_AccessibleName_ContainsTitle) {
    ui::widgets::CompareHint hint(QStringLiteral("container"), QStringLiteral("MKV"));
    const auto* d = ui::compare::compareData(QStringLiteral("container"));
    ASSERT_NE(d, nullptr);
    EXPECT_TRUE(hint.accessibleName().contains(d->title)) << "Accessible name must include the compare data title";
}

} // namespace
} // namespace exosnap
