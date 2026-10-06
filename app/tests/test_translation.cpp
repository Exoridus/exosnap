#include "i18n/Language.h"
#include "notifications/NotificationEvent.h"

#include <QFile>
#include <QRegularExpression>
#include <QTranslator>
#include <QXmlStreamReader>

#include <gtest/gtest.h>

namespace {
QStringList Placeholders(const QString& text) {
    static const QRegularExpression pattern(QStringLiteral("%(?:L?[1-9][0-9]*|n)"));
    QStringList placeholders;
    auto matches = pattern.globalMatch(text);
    while (matches.hasNext())
        placeholders.append(matches.next().captured());
    placeholders.sort();
    return placeholders;
}

QStringList ValidateCatalogue(const QByteArray& bytes, int* message_count = nullptr) {
    QXmlStreamReader xml(bytes);
    QStringList failures;
    int count = 0;
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement() || xml.name() != QLatin1String("message"))
            continue;
        const bool numerus = xml.attributes().value(QLatin1String("numerus")) == QLatin1String("yes");
        QString source;
        QStringList translations;
        bool unfinished = false;
        while (xml.readNextStartElement()) {
            if (xml.name() == QLatin1String("source")) {
                source = xml.readElementText();
            } else if (xml.name() == QLatin1String("translation")) {
                unfinished = xml.attributes().value(QLatin1String("type")) == QLatin1String("unfinished");
                if (numerus) {
                    while (xml.readNextStartElement()) {
                        if (xml.name() == QLatin1String("numerusform"))
                            translations.append(xml.readElementText());
                        else
                            xml.skipCurrentElement();
                    }
                } else {
                    translations.append(xml.readElementText());
                }
            } else {
                xml.skipCurrentElement();
            }
        }
        ++count;
        if (unfinished || translations.size() != (numerus ? 2 : 1))
            failures.append(source + QStringLiteral(": missing translation or plural forms"));
        for (const QString& translation : translations) {
            if (translation.isEmpty() || Placeholders(source) != Placeholders(translation))
                failures.append(source + QStringLiteral(": empty translation or changed placeholders"));
        }
    }
    if (xml.hasError())
        failures.append(xml.errorString());
    if (message_count != nullptr)
        *message_count = count;
    return failures;
}

class TranslationTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        if (QCoreApplication::instance() == nullptr) {
            static int argc = 1;
            static char name[] = "translation_tests";
            static char* argv[] = {name, nullptr};
            static QCoreApplication application(argc, argv);
        }
    }
};
} // namespace

TEST_F(TranslationTest, SystemSupportsGermanLocalesAndFallsBackToEnglish) {
    using exosnap::i18n::EffectiveLanguage;
    EXPECT_EQ(EffectiveLanguage(QStringLiteral("system"), QLocale(QStringLiteral("de_DE"))), QStringLiteral("de"));
    EXPECT_EQ(EffectiveLanguage(QStringLiteral("system"), QLocale(QStringLiteral("de_AT"))), QStringLiteral("de"));
    EXPECT_EQ(EffectiveLanguage(QStringLiteral("system"), QLocale(QStringLiteral("fr_FR"))), QStringLiteral("en"));
    EXPECT_EQ(EffectiveLanguage(QStringLiteral("en"), QLocale(QStringLiteral("de_DE"))), QStringLiteral("en"));
    EXPECT_EQ(EffectiveLanguage(QStringLiteral("de"), QLocale(QStringLiteral("en_US"))), QStringLiteral("de"));
    EXPECT_EQ(exosnap::i18n::NormalizeLanguage(QStringLiteral("Deutsch")), QStringLiteral("system"));
}

TEST_F(TranslationTest, EmbeddedGermanCatalogueCoversProductSurfaces) {
    QTranslator translator;
    ASSERT_TRUE(translator.load(QStringLiteral(":/exosnap/i18n/exosnap_de.qm")));
    const char* contexts[] = {"RecordPage",       "SettingsAppearanceSection",
                              "DeviceAdapter",    "LogsPage",
                              "AboutPage",        "EditOverlay",
                              "RecoveryOverlay",  "NotificationHub",
                              "OverlayRecording", "exosnap::quick::TrayAdapter",
                              "UpdaterController"};
    QFile file(QStringLiteral(":/exosnap/i18n/exosnap_de.ts"));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const QByteArray catalogue = file.readAll();
    for (const char* context : contexts)
        EXPECT_TRUE(catalogue.contains(QByteArray("<name>") + context + "</name>")) << context;
    ASSERT_TRUE(QCoreApplication::installTranslator(&translator));
    EXPECT_EQ(QCoreApplication::translate("SettingsAppearanceSection", "Language"), QStringLiteral("Sprache"));
    EXPECT_EQ(QCoreApplication::translate("SettingsAppearanceSection", "Appearance"), QStringLiteral("Darstellung"));
    EXPECT_EQ(QCoreApplication::translate("UnknownDiagnostic", "nvenc_submit_failed"),
              QStringLiteral("nvenc_submit_failed"));
    QCoreApplication::removeTranslator(&translator);
}

TEST_F(TranslationTest, GermanCatalogueHasCompletePlaceholdersAndPluralForms) {
    QFile file(QStringLiteral(":/exosnap/i18n/exosnap_de.ts"));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    int count = 0;
    const QStringList failures = ValidateCatalogue(file.readAll(), &count);
    EXPECT_GE(count, 1000);
    EXPECT_TRUE(failures.isEmpty()) << failures.join(QLatin1Char('\n')).toStdString();
}

TEST_F(TranslationTest, CoverageValidationRejectsBrokenCatalogues) {
    EXPECT_FALSE(ValidateCatalogue("<TS><context><message><source>%1</source><translation>Wert</translation>"
                                   "</message></context></TS>")
                     .isEmpty());
    EXPECT_FALSE(ValidateCatalogue("<TS><context><message numerus='yes'><source>%n</source><translation>"
                                   "<numerusform>%n</numerusform></translation></message></context></TS>")
                     .isEmpty());
    EXPECT_FALSE(ValidateCatalogue("<TS><context><message><source>Ready</source><translation type='unfinished'>"
                                   "</translation></message></context></TS>")
                     .isEmpty());
    EXPECT_FALSE(ValidateCatalogue("<TS><context>").isEmpty());
}

TEST_F(TranslationTest, GermanNotificationCopyUsesWholeSentencePluralForms) {
    QTranslator translator;
    ASSERT_TRUE(translator.load(QStringLiteral(":/exosnap/i18n/exosnap_de.qm")));
    ASSERT_TRUE(QCoreApplication::installTranslator(&translator));
    using exosnap::engine::AudioSourceKind;
    using exosnap::engine::AudioSourceKindBit;
    const auto one = exosnap::notifications::MakeAudioSourceDegradedEvent(1, AudioSourceKindBit(AudioSourceKind::Mic));
    const auto many = exosnap::notifications::MakeAudioSourceDegradedEvent(
        2, AudioSourceKindBit(AudioSourceKind::Mic) | AudioSourceKindBit(AudioSourceKind::Sys));
    EXPECT_EQ(one.title, QStringLiteral("Mikrofon liefert kein Audio mehr"));
    EXPECT_TRUE(one.body.contains(QStringLiteral("diese Audioquelle weiter")));
    EXPECT_TRUE(many.body.contains(QStringLiteral("diese Audioquellen weiter")));
    EXPECT_EQ(QCoreApplication::translate("ExoNotice", "Success"), QStringLiteral("Erfolg"));
    EXPECT_EQ(QCoreApplication::translate("OverlayRecording", "health"), QStringLiteral("Status"));
    EXPECT_EQ(QCoreApplication::translate("Logs", "no search"), QStringLiteral("keine Suche"));
    EXPECT_EQ(QCoreApplication::translate("Diagnostics", "driver %1").arg(QStringLiteral("581.29")),
              QStringLiteral("Treiber 581.29"));
    EXPECT_EQ(QCoreApplication::translate("Diagnostics", "%1 things to fix before recording").arg(2),
              QStringLiteral("Vor der Aufnahme sind 2 Probleme zu beheben"));
    QCoreApplication::removeTranslator(&translator);
}