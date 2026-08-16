// The text-expansion rule the 860x700 layout gate renders with (QCR-511).
//
// Small on purpose: the value of the harness is in what the RENDERS show, and
// what has to be pinned here is only that the expansion is deterministic, that
// it actually grows, and that it leaves alone the two things a layout pass must
// not have perturbed — an empty string and a translated string's placeholders.

#include "PseudoLocalization.h"

#include <gtest/gtest.h>

#include <QString>

using exosnap::quick::ExpandForPseudoLocalization;
using exosnap::quick::PseudoLocalizationTranslator;

namespace {

constexpr double kGrowth = 0.4;

} // namespace

TEST(PseudoLocalizationTest, EmptyAndWhitespaceOnlyStringsPassThrough) {
    // The product deliberately draws nothing for these — a separator that is
    // only ever a space, a label bound to an empty adapter property. Padding
    // them would put text on screen the application never puts there and would
    // report a layout failure that does not exist.
    EXPECT_EQ(ExpandForPseudoLocalization(QString()), QString());
    EXPECT_EQ(ExpandForPseudoLocalization(QStringLiteral("")), QStringLiteral(""));
    EXPECT_EQ(ExpandForPseudoLocalization(QStringLiteral("   ")), QStringLiteral("   "));
}

TEST(PseudoLocalizationTest, ExpandedTextIsBracketedAtBothEnds) {
    const QString expanded = ExpandForPseudoLocalization(QStringLiteral("Diagnostics"));
    EXPECT_TRUE(expanded.startsWith(QStringLiteral("«"))) << expanded.toStdString();
    // The closing bracket is the evidence: a capture missing it is a capture in
    // which the layout truncated the string.
    EXPECT_TRUE(expanded.endsWith(QStringLiteral("»"))) << expanded.toStdString();
}

TEST(PseudoLocalizationTest, EverySourceGrowsByAtLeastTheStatedFraction) {
    // The short ones matter most: the title band's five destinations and the
    // transport's action labels are all under a dozen characters, and they
    // share the tightest row in the product.
    for (const QString& source :
         {QStringLiteral("Logs"), QStringLiteral("Record"), QStringLiteral("Diagnostics"),
          QStringLiteral("Change source"), QStringLiteral("Merge with above"),
          QStringLiteral("Recording start is blocked by a diagnostic blocker.")}) {
        const QString expanded = ExpandForPseudoLocalization(source);
        EXPECT_GE(expanded.size(), source.size() + static_cast<int>(source.size() * kGrowth))
            << source.toStdString() << " -> " << expanded.toStdString();
    }
}

TEST(PseudoLocalizationTest, PlaceholdersSurviveVerbatim) {
    // qsTr("%1: %2").arg(...) is everywhere in this frontend. The expansion only
    // ever wraps and appends, so the placeholders stay where the caller's own
    // .arg() will find them — an expansion that reordered or escaped them would
    // silently turn every formatted string into its own literal.
    const QString expanded = ExpandForPseudoLocalization(QStringLiteral("%1: %2"));
    EXPECT_TRUE(expanded.contains(QStringLiteral("%1")));
    EXPECT_TRUE(expanded.contains(QStringLiteral("%2")));

    const QString formatted = expanded.arg(QStringLiteral("SCREEN")).arg(QStringLiteral("Display 1"));
    EXPECT_TRUE(formatted.contains(QStringLiteral("SCREEN: Display 1"))) << formatted.toStdString();
    EXPECT_FALSE(formatted.contains(QStringLiteral("%1")));
}

TEST(PseudoLocalizationTest, TheRuleIsDeterministic) {
    // Two --visual-test captures of the same scenario must be comparable, so
    // the padding may not depend on anything but the source string.
    EXPECT_EQ(ExpandForPseudoLocalization(QStringLiteral("Settings")),
              ExpandForPseudoLocalization(QStringLiteral("Settings")));
    EXPECT_NE(ExpandForPseudoLocalization(QStringLiteral("Settings")),
              ExpandForPseudoLocalization(QStringLiteral("Diagnostics")));
}

TEST(PseudoLocalizationTest, TheTranslatorIsNeverSkippedAsEmpty) {
    // Qt asks isEmpty() first and skips a translator that answers true, which
    // would leave translate() unreached and the whole harness silently inert.
    PseudoLocalizationTranslator translator;
    EXPECT_FALSE(translator.isEmpty());
    EXPECT_EQ(translator.translate("Any", "Record"), ExpandForPseudoLocalization(QStringLiteral("Record")));
    // A null source is what Qt hands over for a lookup with no source text.
    EXPECT_EQ(translator.translate("Any", nullptr), QString());
}
