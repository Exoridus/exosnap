#include "i18n/Language.h"

#include <gtest/gtest.h>

TEST(LanguageFallbackTest, MissingEmbeddedCatalogueKeepsEnglishAndStartupUsable) {
    static int argc = 1;
    static char name[] = "language_fallback_tests";
    static char* argv[] = {name, nullptr};
    QCoreApplication application(argc, argv);
    EXPECT_EQ(exosnap::i18n::InstallLanguage(application, QStringLiteral("de")), QStringLiteral("en"));
    EXPECT_EQ(exosnap::i18n::RunningLanguage(), QStringLiteral("en"));
    EXPECT_EQ(QCoreApplication::translate("UpdaterController", "Update complete"), QStringLiteral("Update complete"));
}
