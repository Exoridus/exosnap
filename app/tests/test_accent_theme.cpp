// ACCENT-PICKER-R1 (0.5.0-B): tests for accent id resolution, derived token
// generation, and theme-string injection helpers.
//
// These tests are intentionally headless (no QApplication required) — they
// only exercise pure logic in the accent/theme layer.

#include <gtest/gtest.h>

#include <QColor>
#include <QString>
#include <QStringList>

#include "ui/theme/ExoSnapAccents.h"
#include "ui/theme/ExoSnapPalette.h"

namespace exosnap {
namespace {

// ---------------------------------------------------------------------------
// Accent table structure tests
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

// ---------------------------------------------------------------------------
// Accent id resolution by id string
// ---------------------------------------------------------------------------

TEST(AccentThemeTest, FindAccentById_Mint_ReturnsEntry) {
    const auto& accents = exosnap::ui::theme::kExoSnapAccents;
    bool found = false;
    for (const auto& a : accents) {
        if (QString::fromUtf8(a.id) == QStringLiteral("mint")) {
            found = true;
            EXPECT_STREQ(a.name, "Studio Mint");
            EXPECT_STREQ(a.base, "#9BD9D2");
            break;
        }
    }
    EXPECT_TRUE(found) << "Expected 'mint' accent in kExoSnapAccents.";
}

TEST(AccentThemeTest, FindAccentById_Azure_ReturnsCorrectBase) {
    const auto& accents = exosnap::ui::theme::kExoSnapAccents;
    bool found = false;
    for (const auto& a : accents) {
        if (QString::fromUtf8(a.id) == QStringLiteral("azure")) {
            found = true;
            EXPECT_STREQ(a.base, "#63B5F2");
            break;
        }
    }
    EXPECT_TRUE(found) << "Expected 'azure' accent in kExoSnapAccents.";
}

TEST(AccentThemeTest, FindAccentById_Violet_ReturnsCorrectBase) {
    const auto& accents = exosnap::ui::theme::kExoSnapAccents;
    bool found = false;
    for (const auto& a : accents) {
        if (QString::fromUtf8(a.id) == QStringLiteral("violet")) {
            found = true;
            EXPECT_STREQ(a.base, "#AB9DF2");
            break;
        }
    }
    EXPECT_TRUE(found) << "Expected 'violet' accent in kExoSnapAccents.";
}

TEST(AccentThemeTest, AllAccentIds_AreUnique) {
    const auto& accents = exosnap::ui::theme::kExoSnapAccents;
    QStringList ids;
    for (const auto& a : accents)
        ids.append(QString::fromUtf8(a.id));

    QStringList deduped = ids;
    deduped.removeDuplicates();
    EXPECT_EQ(ids.size(), deduped.size()) << "All accent ids must be unique.";
}

TEST(AccentThemeTest, AllAccentIds_AreNonEmpty) {
    for (const auto& a : exosnap::ui::theme::kExoSnapAccents) {
        EXPECT_FALSE(QString::fromUtf8(a.id).isEmpty())
            << "Accent id must not be empty (accent name: " << a.name << ").";
    }
}

TEST(AccentThemeTest, AllAccentBases_AreValidHexColors) {
    for (const auto& a : exosnap::ui::theme::kExoSnapAccents) {
        const QColor c(QString::fromUtf8(a.base));
        EXPECT_TRUE(c.isValid()) << "Accent base color is not a valid QColor: " << a.base << " (id: " << a.id << ").";
    }
}

TEST(AccentThemeTest, AllAccentInks_AreValidHexColors) {
    for (const auto& a : exosnap::ui::theme::kExoSnapAccents) {
        const QColor c(QString::fromUtf8(a.ink));
        EXPECT_TRUE(c.isValid()) << "Accent ink color is not a valid QColor: " << a.ink << " (id: " << a.id << ").";
    }
}

// ---------------------------------------------------------------------------
// Semantic color isolation — confirm coral/green/amber are NOT in accent table
// ---------------------------------------------------------------------------

TEST(AccentThemeTest, SemanticColors_AreNotPresentAsAccentBases) {
    // kErr = coral (#E0786C), kOk = green (#84CBA2), kWarn = amber (#E6C57C)
    // None of these should appear as a curated accent base (they are semantic).
    const QString kErrHex = QString::fromUtf8(exosnap::ui::theme::ExoSnapPalette::kErr);
    const QString kOkHex = QString::fromUtf8(exosnap::ui::theme::ExoSnapPalette::kOk);
    const QString kWarnHex = QString::fromUtf8(exosnap::ui::theme::ExoSnapPalette::kWarn);

    for (const auto& a : exosnap::ui::theme::kExoSnapAccents) {
        const QString base = QString::fromUtf8(a.base);
        EXPECT_NE(base.toLower(), kErrHex.toLower())
            << "Semantic coral color must not be a curated accent base (id: " << a.id << ").";
        EXPECT_NE(base.toLower(), kOkHex.toLower())
            << "Semantic green color must not be a curated accent base (id: " << a.id << ").";
        EXPECT_NE(base.toLower(), kWarnHex.toLower())
            << "Semantic amber color must not be a curated accent base (id: " << a.id << ").";
    }
}

} // namespace
} // namespace exosnap
