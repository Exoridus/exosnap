// test_theme_token_resolution.cpp
//
// Guard test: an invalid/leftover `${token}` in the QSS crashes the app at
// startup in Debug (Q_ASSERT_X in ExoSnapTheme.cpp) and silently ships a raw
// "${token}" string in Release. That guard has no test exercising the real
// production path across every shipped theme, so a QSS edit that orphans a
// token for a non-default theme (or removes a token definition) can slip
// through review.
//
// This drives the real resolver — ApplyExoSnapTheme() / ReapplyTheme(), which
// load the actual compiled QSS resource and substitute the actual token
// tables — for every curated theme in ExoSnapThemes.h, and asserts the
// resulting stylesheet has no unresolved `${...}` token left in it.

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QRegularExpression>
#include <QRegularExpressionMatchIterator>
#include <QString>
#include <QStringList>

#include "ui/theme/ExoSnapTheme.h"
#include "ui/theme/ExoSnapThemes.h"

namespace exosnap {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "test_theme_token_resolution";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

// Returns every `${...}` token still present in `stylesheet`, deduplicated.
QStringList FindUnresolvedTokens(const QString& stylesheet) {
    static const QRegularExpression kTokenPattern(R"(\$\{([^}]+)\})");
    QStringList unresolved;
    QRegularExpressionMatchIterator it = kTokenPattern.globalMatch(stylesheet);
    while (it.hasNext())
        unresolved.append(it.next().captured(1));
    unresolved.removeDuplicates();
    return unresolved;
}

class ThemeTokenResolutionTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        app_ = EnsureApplication();
        ui::theme::ApplyExoSnapTheme(*app_);
    }

    static QApplication* app_;
};

QApplication* ThemeTokenResolutionTest::app_ = nullptr;

// One case per curated theme so a failure names the offending theme directly
// instead of a single parameterized "some theme failed" report.
TEST_F(ThemeTokenResolutionTest, EveryCuratedTheme_ResolvesAllTokens) {
    ASSERT_GT(ui::theme::kExoThemes.size(), 0u);

    for (const auto& theme : ui::theme::kExoThemes) {
        ui::theme::ReapplyTheme(*app_, QString::fromUtf8(theme.id));
        const QString stylesheet = app_->styleSheet();

        // A resource-load failure would make the "no unresolved tokens" check
        // vacuously pass; guard against that regression too.
        ASSERT_FALSE(stylesheet.isEmpty()) << "Theme '" << theme.id << "' produced an empty stylesheet "
                                           << "(QSS resource failed to load?)";

        const QStringList unresolved = FindUnresolvedTokens(stylesheet);
        EXPECT_TRUE(unresolved.isEmpty())
            << "Theme '" << theme.id << "' left unresolved tokens: " << unresolved.join(", ").toStdString();
    }
}

// dark-default is applied at startup before any ReapplyTheme() call
// (ApplyExoSnapTheme); pin that the very first stylesheet build is also clean.
TEST_F(ThemeTokenResolutionTest, InitialApply_DarkDefault_ResolvesAllTokens) {
    ui::theme::ApplyExoSnapTheme(*app_);
    const QString stylesheet = app_->styleSheet();

    ASSERT_FALSE(stylesheet.isEmpty());
    const QStringList unresolved = FindUnresolvedTokens(stylesheet);
    EXPECT_TRUE(unresolved.isEmpty()) << "dark-default left unresolved tokens: " << unresolved.join(", ").toStdString();
}

} // namespace
} // namespace exosnap
