// The settings automation key table: the contract `settings.get` and
// `settings.set` speak.
//
// Three things are under test, and they are the three ways this could quietly
// stop being a contract:
//
//   * A wire value is never an enumerator ORDINAL. Reordering an enum must not
//     change what a client reads or writes.
//   * A key is never a QML property name. The table is what decouples them, so
//     a renamed property is a compile error here rather than a protocol break.
//   * Every write goes through the SettingsAdapter setter the QML control is
//     bound to, so the product reconciles it. A write the product changes its
//     mind about is still a success, and the read-back is what says so.

#include "SettingsAdapter.h"
#include "SettingsAutomationKeys.h"

#include "models/RecordingPreset.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

namespace exosnap::quick {
namespace {

using settings_automation::AllKeys;
using settings_automation::DescribeKeys;
using settings_automation::FindKey;
using settings_automation::KeyDescriptor;
using settings_automation::ReadKeys;
using settings_automation::ValueType;
using settings_automation::WriteKey;

QCoreApplication* EnsureApplication() {
    if (auto* existing = QCoreApplication::instance())
        return existing;
    static int argc = 1;
    static char app_name[] = "settings_automation_tests";
    static char* argv[] = {app_name, nullptr};
    static QCoreApplication app(argc, argv);
    return &app;
}

// An adapter seeded with the shipped default configuration, which is what a
// fresh install reads.
std::unique_ptr<SettingsAdapter> MakeAdapter() {
    EnsureApplication();
    auto adapter = std::make_unique<SettingsAdapter>();
    adapter->setConfig(MakeDefaultPreset().config);
    adapter->setAppSettings({});
    return adapter;
}

QJsonValue Read(const SettingsAdapter& adapter, const char* key) {
    QString error;
    const QJsonObject json = ReadKeys(adapter, QString::fromLatin1(key), &error);
    EXPECT_TRUE(error.isEmpty()) << error.toStdString();
    return json.value(QStringLiteral("values")).toObject().value(QString::fromLatin1(key));
}

TEST(SettingsAutomationKeys, EveryKeyIsUniqueReadableAndDescribed) {
    const std::unique_ptr<SettingsAdapter> adapter = MakeAdapter();

    QSet<QString> seen;
    for (const KeyDescriptor& descriptor : AllKeys()) {
        EXPECT_FALSE(descriptor.key.isEmpty());
        EXPECT_FALSE(seen.contains(descriptor.key)) << descriptor.key.toStdString();
        seen.insert(descriptor.key);
        EXPECT_FALSE(descriptor.description.isEmpty()) << descriptor.key.toStdString();
        // An enum without values would accept anything; a non-enum with values
        // would advertise a constraint nothing enforces.
        EXPECT_EQ(descriptor.type == ValueType::Enum, !descriptor.allowed.isEmpty()) << descriptor.key.toStdString();
        // Every key answers. A read that came back undefined would mean a getter
        // was wired to nothing.
        EXPECT_FALSE(descriptor.read(*adapter).isUndefined()) << descriptor.key.toStdString();
    }
    EXPECT_GT(seen.size(), 30) << "the table is meant to cover the recording configuration, not a sample of it";

    const QJsonObject described = DescribeKeys();
    EXPECT_EQ(described.value(QStringLiteral("count")).toInt(), AllKeys().size());
    EXPECT_EQ(described.value(QStringLiteral("writeSemantics")).toString(), QStringLiteral("reconciledByProduct"));
}

TEST(SettingsAutomationKeys, KeysArePrefixedByProductAreaAndNeverNamedAfterAQmlProperty) {
    for (const KeyDescriptor& descriptor : AllKeys()) {
        const bool grouped = descriptor.key.startsWith(QStringLiteral("video.")) ||
                             descriptor.key.startsWith(QStringLiteral("audio.")) ||
                             descriptor.key.startsWith(QStringLiteral("split.")) ||
                             descriptor.key.startsWith(QStringLiteral("webcam.")) ||
                             descriptor.key.startsWith(QStringLiteral("app."));
        EXPECT_TRUE(grouped) << descriptor.key.toStdString();
    }
    // The four the Settings surface spells differently from the product: the
    // adapter calls them `cq`, `chroma`, `autoUpdateCheck` and `expertMode`, and
    // the wire says what the product means.
    EXPECT_NE(FindKey(QStringLiteral("video.cq")), nullptr);
    EXPECT_NE(FindKey(QStringLiteral("video.chroma")), nullptr);
    EXPECT_NE(FindKey(QStringLiteral("app.checkUpdatesOnStart")), nullptr);
    EXPECT_EQ(FindKey(QStringLiteral("autoUpdateCheck")), nullptr);
    EXPECT_EQ(FindKey(QStringLiteral("container")), nullptr);
}

TEST(SettingsAutomationKeys, EnumValuesAreProductSpellingsAndNeverOrdinals) {
    const std::unique_ptr<SettingsAdapter> adapter = MakeAdapter();

    EXPECT_EQ(Read(*adapter, "video.container").toString(), QStringLiteral("MKV"));
    EXPECT_EQ(Read(*adapter, "video.videoCodec").toString(), QStringLiteral("AV1"));
    EXPECT_EQ(Read(*adapter, "video.audioCodec").toString(), QStringLiteral("Opus"));
    EXPECT_EQ(Read(*adapter, "video.colorRange").toString(), QStringLiteral("limited"));
    EXPECT_EQ(Read(*adapter, "video.bitDepth").toString(), QStringLiteral("8"));

    // Nothing in the payload is a bare enumerator number.
    for (const KeyDescriptor& descriptor : AllKeys()) {
        if (descriptor.type != ValueType::Enum)
            continue;
        const QJsonValue value = descriptor.read(*adapter);
        EXPECT_TRUE(value.isString() || value.isNull()) << descriptor.key.toStdString();
        if (value.isString())
            EXPECT_TRUE(descriptor.allowed.contains(value.toString())) << descriptor.key.toStdString();
    }
}

TEST(SettingsAutomationKeys, AWriteGoesThroughTheProductAndTheReadBackShowsWhatItDecided) {
    const std::unique_ptr<SettingsAdapter> adapter = MakeAdapter();
    QString error;

    // MKV + AV1 + Opus is the default. Switching the container to MP4 is a
    // request the product reconciles: ADR 0010 says MP4 carries neither AV1 nor
    // Opus, so both codecs move -- and the write is still a success.
    ASSERT_TRUE(WriteKey(*adapter, QStringLiteral("video.container"), QStringLiteral("MP4"), &error))
        << error.toStdString();
    EXPECT_EQ(Read(*adapter, "video.container").toString(), QStringLiteral("MP4"));
    EXPECT_NE(Read(*adapter, "video.videoCodec").toString(), QStringLiteral("AV1"))
        << "MP4 does not carry AV1; the reconciliation has to have run";
    EXPECT_EQ(Read(*adapter, "video.audioCodec").toString(), QStringLiteral("AAC"));
}

TEST(SettingsAutomationKeys, AnUnknownKeyOrValueIsRefusedWithAnExplanation) {
    const std::unique_ptr<SettingsAdapter> adapter = MakeAdapter();
    QString error;

    EXPECT_FALSE(WriteKey(*adapter, QStringLiteral("video.nonsense"), QStringLiteral("x"), &error));
    EXPECT_TRUE(error.contains(QStringLiteral("video.nonsense")));

    error.clear();
    EXPECT_FALSE(WriteKey(*adapter, QStringLiteral("video.container"), QStringLiteral("AVI"), &error));
    // The refusal names the accepted set, so a client does not have to discover
    // it by being refused repeatedly.
    EXPECT_TRUE(error.contains(QStringLiteral("MKV"))) << error.toStdString();

    error.clear();
    EXPECT_FALSE(WriteKey(*adapter, QStringLiteral("app.expertMode"), QStringLiteral("yes"), &error));
    EXPECT_TRUE(error.contains(QStringLiteral("boolean"))) << error.toStdString();

    error.clear();
    EXPECT_FALSE(WriteKey(*adapter, QStringLiteral("video.cq"), QStringLiteral("17"), &error));
    EXPECT_TRUE(error.contains(QStringLiteral("number"))) << error.toStdString();
}

TEST(SettingsAutomationKeys, BooleansAndNumbersRoundTrip) {
    const std::unique_ptr<SettingsAdapter> adapter = MakeAdapter();
    QString error;

    ASSERT_TRUE(WriteKey(*adapter, QStringLiteral("app.expertMode"), true, &error)) << error.toStdString();
    EXPECT_TRUE(Read(*adapter, "app.expertMode").toBool());
    ASSERT_TRUE(WriteKey(*adapter, QStringLiteral("app.expertMode"), false, &error)) << error.toStdString();
    EXPECT_FALSE(Read(*adapter, "app.expertMode").toBool());

    ASSERT_TRUE(WriteKey(*adapter, QStringLiteral("video.cq"), 21, &error)) << error.toStdString();
    EXPECT_EQ(Read(*adapter, "video.cq").toInt(), 21);

    ASSERT_TRUE(WriteKey(*adapter, QStringLiteral("video.encoderPreset"), QStringLiteral("P7"), &error))
        << error.toStdString();
    EXPECT_EQ(Read(*adapter, "video.encoderPreset").toString(), QStringLiteral("P7"));
}

TEST(SettingsAutomationKeys, ReadingWithoutAKeyAnswersEveryKeyAtOnce) {
    const std::unique_ptr<SettingsAdapter> adapter = MakeAdapter();
    QString error;
    const QJsonObject values = ReadKeys(*adapter, QString(), &error).value(QStringLiteral("values")).toObject();
    EXPECT_TRUE(error.isEmpty());
    EXPECT_EQ(values.size(), AllKeys().size());
    for (const KeyDescriptor& descriptor : AllKeys())
        EXPECT_TRUE(values.contains(descriptor.key)) << descriptor.key.toStdString();
}

} // namespace
} // namespace exosnap::quick
