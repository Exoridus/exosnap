#pragma once

#include <QString>
#include <QTranslator>

namespace exosnap::quick {

// Controlled text expansion for layout regression testing (QCR-511).
//
// The 860x700 minimum window has to stay fully usable, and "usable with the
// current English strings" is a weaker statement than it looks: German and
// Finnish routinely run 30-40 % longer than English for short UI labels, and
// the places that break are the ones with no room to give — the title band's
// five destinations beside three window buttons, the transport dock's row of
// actions, a settings row's label/control pair.
//
// Rather than a localization stack or a test-seam property per label, this
// rides the machinery `qsTr()` already uses: an installed QTranslator is asked
// for every translatable string in QML and C++ alike, so one subclass expands
// all of them and nothing in the frontend has to know it exists. It is armed by
// `--pseudo-localize` on the command line and by nothing else — there is no
// environment variable and no build flag, because a shipped binary that could
// wander into this mode by accident would be worse than not having it.
//
// The expansion is deliberately readable rather than a mangled alphabet: the
// point is to measure LAYOUT, so the padding has to wrap and elide the way real
// words do. A run of one repeated character has no break opportunity and would
// measure something the product never encounters.

// Wraps `source` in guillemets and pads it to roughly 140 % of its length.
//
// The brackets are the classic pseudo-localization marker and they carry real
// information here: a capture in which the closing "»" is missing is a capture
// in which something truncated, which is exactly what a text-expansion pass is
// looking for.
//
// Empty and whitespace-only strings pass through untouched — expanding those
// would add visible text where the product deliberately draws none.
[[nodiscard]] QString ExpandForPseudoLocalization(const QString& source);

class PseudoLocalizationTranslator : public QTranslator {
    Q_OBJECT

  public:
    using QTranslator::QTranslator;

    [[nodiscard]] QString translate(const char* context, const char* source_text, const char* disambiguation = nullptr,
                                    int n = -1) const override;

    // Qt skips an "empty" translator entirely, so this has to answer false for
    // the override above to ever be reached.
    [[nodiscard]] bool isEmpty() const override;
};

} // namespace exosnap::quick
