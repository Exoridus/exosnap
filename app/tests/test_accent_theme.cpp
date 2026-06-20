// test_accent_theme.cpp — tests for the theme system (THEME-SLICE-1).
// Covers: legacy ExoSnapAccents table + new ExoTheme table + palette cross-checks.
//
// These tests are intentionally headless (no QApplication required) — they
// only exercise pure data in the accent/theme layer.

#include <gtest/gtest.h>

#include <QColor>
#include <QString>
#include <QStringList>

#include "ui/theme/ExoSnapAccents.h"
#include "ui/theme/ExoSnapPalette.h"
#include "ui/theme/ExoSnapThemes.h"

namespace exosnap {
namespace {

// ---------------------------------------------------------------------------
// Legacy accent table tests (kept for backward compat)
// ---------------------------------------------------------------------------

TEST(AccentThemeTest, DefaultAccentId_IsMint) {
    EXPECT_STREQ(exosnap::ui::theme::kDefaultAccentId, "mint");
}

TEST(AccentThemeTest, DefaultAccentEntry_IsFirstInTable) {
    EXPECT_STREQ(exosnap::ui::theme::kExoSnapAccents.front().id, "mint");
    EXPECT_STREQ(exosnap::ui::theme::kExoSnapAccents.front().name, "Studio Mint");
}

TEST(AccentThemeTest, DefaultAccentBase_MatchesPaletteKAccent) {
    EXPECT_STREQ(exosnap::ui::theme::kExoSnapAccents.front().base, exosnap::ui::theme::ExoSnapPalette::kAccent);
}

TEST(AccentThemeTest, DefaultAccentInk_MatchesPaletteKAccentInk) {
    EXPECT_STREQ(exosnap::ui::theme::kExoSnapAccents.front().ink, exosnap::ui::theme::ExoSnapPalette::kAccentInk);
}

TEST(AccentThemeTest, AccentTable_HasSevenEntries) {
    EXPECT_EQ(static_cast<int>(exosnap::ui::theme::kExoSnapAccents.size()), 7);
}

TEST(AccentThemeTest, AllAccentIds_AreUnique) {
    const auto& accents = exosnap::ui::theme::kExoSnapAccents;
    QStringList ids;
    for (const auto& a : accents)
        ids.append(QString::fromUtf8(a.id));
    QStringList deduped = ids;
    deduped.removeDuplicates();
    EXPECT_EQ(ids.size(), deduped.size());
}

TEST(AccentThemeTest, AllAccentBases_AreValidHexColors) {
    for (const auto& a : exosnap::ui::theme::kExoSnapAccents) {
        const QColor c(QString::fromUtf8(a.base));
        EXPECT_TRUE(c.isValid()) << "Accent base invalid: " << a.base;
    }
}

TEST(AccentThemeTest, SemanticColors_AreNotPresentAsAccentBases) {
    const QString kErrHex = QString::fromUtf8(exosnap::ui::theme::ExoSnapPalette::kErr);
    const QString kOkHex = QString::fromUtf8(exosnap::ui::theme::ExoSnapPalette::kOk);
    const QString kWarnHex = QString::fromUtf8(exosnap::ui::theme::ExoSnapPalette::kWarn);
    for (const auto& a : exosnap::ui::theme::kExoSnapAccents) {
        const QString base = QString::fromUtf8(a.base);
        EXPECT_NE(base.toLower(), kErrHex.toLower());
        EXPECT_NE(base.toLower(), kOkHex.toLower());
        EXPECT_NE(base.toLower(), kWarnHex.toLower());
    }
}

// ---------------------------------------------------------------------------
// New theme table tests (THEME-SLICE-1)
// ---------------------------------------------------------------------------

TEST(ExoThemeTest, ThemeTable_HasFourEntries) {
    EXPECT_EQ(static_cast<int>(exosnap::ui::theme::kExoThemes.size()), 4);
}

TEST(ExoThemeTest, DefaultThemeId_IsDarkDefault) {
    EXPECT_STREQ(exosnap::ui::theme::kDefaultThemeId, "dark-default");
    EXPECT_STREQ(exosnap::ui::theme::kExoThemes.front().id, "dark-default");
}

TEST(ExoThemeTest, AllThemeIds_AreUnique) {
    QStringList ids;
    for (const auto& t : exosnap::ui::theme::kExoThemes)
        ids.append(QString::fromUtf8(t.id));
    QStringList deduped = ids;
    deduped.removeDuplicates();
    EXPECT_EQ(ids.size(), deduped.size());
}

TEST(ExoThemeTest, DarkDefault_MatchesLegacyPaletteValues) {
    const auto& t = exosnap::ui::theme::kExoThemes.front();
    using P = exosnap::ui::theme::ExoSnapPalette;
    EXPECT_STREQ(t.bg, P::kBg0);
    EXPECT_STREQ(t.surf, P::kBg1);
    EXPECT_STREQ(t.surf2, P::kBg2);
    EXPECT_STREQ(t.raise, P::kBg3);
    EXPECT_STREQ(t.ink, P::kText0);
    EXPECT_STREQ(t.mut, P::kText2);
    EXPECT_STREQ(t.dim, P::kText3);
    EXPECT_STREQ(t.ac, P::kAccent);
    EXPECT_STREQ(t.ac_ink, P::kAccentInk);
    EXPECT_STREQ(t.success, P::kOk);
    EXPECT_STREQ(t.caution, P::kWarn);
    EXPECT_STREQ(t.error, P::kErr);
}

TEST(ExoThemeTest, DarkDefault_ExplicitOverrides_MatchPalette) {
    const auto& t = exosnap::ui::theme::kExoThemes.front();
    using P = exosnap::ui::theme::ExoSnapPalette;
    ASSERT_NE(t.bg4_override, nullptr);
    EXPECT_STREQ(t.bg4_override, P::kBg4);
    ASSERT_NE(t.line3_override, nullptr);
    EXPECT_STREQ(t.line3_override, P::kLine3);
    ASSERT_NE(t.text1_override, nullptr);
    EXPECT_STREQ(t.text1_override, P::kText1);
}

TEST(ExoThemeTest, AllThemeBgs_AreValidHexColors) {
    for (const auto& t : exosnap::ui::theme::kExoThemes) {
        const QColor c(QString::fromUtf8(t.bg));
        EXPECT_TRUE(c.isValid()) << "bg invalid for theme: " << t.id;
    }
}

TEST(ExoThemeTest, AllThemeAcs_AreValidColors) {
    for (const auto& t : exosnap::ui::theme::kExoThemes) {
        const QColor c(QString::fromUtf8(t.ac));
        EXPECT_TRUE(c.isValid()) << "ac invalid for theme: " << t.id;
    }
}

TEST(ExoThemeTest, DarkThemes_HaveDarkKind) {
    for (const auto& t : exosnap::ui::theme::kExoThemes) {
        if (QString::fromUtf8(t.group) == QStringLiteral("Dark"))
            EXPECT_EQ(t.kind, exosnap::ui::theme::ThemeKind::Dark) << "Dark group theme has wrong kind: " << t.id;
        else
            EXPECT_EQ(t.kind, exosnap::ui::theme::ThemeKind::Light) << "Light group theme has wrong kind: " << t.id;
    }
}

TEST(ExoThemeTest, LogPalettes_AreNonEmpty) {
    for (const auto& t : exosnap::ui::theme::kExoThemes) {
        EXPECT_NE(t.log.cat, nullptr) << "log.cat null for theme: " << t.id;
        EXPECT_GT(strlen(t.log.cat), 0u) << "log.cat empty for theme: " << t.id;
    }
}

TEST(ExoThemeTest, LightThemes_HaveNullptrDerivedOverrides) {
    // Only dark-default uses overrides; other themes derive from the formulae.
    for (std::size_t i = 1; i < exosnap::ui::theme::kExoThemes.size(); ++i) {
        const auto& t = exosnap::ui::theme::kExoThemes[i];
        EXPECT_EQ(t.bg4_override, nullptr) << "Non-default theme should not have bg4_override: " << t.id;
        EXPECT_EQ(t.line3_override, nullptr) << "Non-default theme should not have line3_override: " << t.id;
        EXPECT_EQ(t.text1_override, nullptr) << "Non-default theme should not have text1_override: " << t.id;
    }
}

} // namespace
} // namespace exosnap
