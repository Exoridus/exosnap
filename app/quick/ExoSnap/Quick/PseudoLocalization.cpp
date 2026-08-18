#include "PseudoLocalization.h"

#include <QStringList>

#include <algorithm>
#include <cmath>

namespace exosnap::quick {
namespace {

// Real words with real break opportunities and a few diacritics, so the padded
// string wraps like prose and exercises the ascender/descender box the shipped
// faces actually draw for a translated UI.
const QStringList& fillerWords() {
    static const QStringList words = {
        QStringLiteral("lörem"), QStringLiteral("ïpsüm"),       QStringLiteral("dölör"),   QStringLiteral("sït"),
        QStringLiteral("ämët"),  QStringLiteral("cönsëctëtür"), QStringLiteral("ädïpïsc"),
    };
    return words;
}

// 40 %: the upper end of the usual English -> German/Finnish growth for short
// UI strings, and the number the acceptance for this item is stated in.
constexpr double kGrowth = 0.4;
// A two-character label would otherwise grow by nothing at all, and the short
// labels are exactly the ones sharing the tightest band in the product.
constexpr int kMinimumExtra = 4;

} // namespace

QString ExpandForPseudoLocalization(const QString& source) {
    if (source.trimmed().isEmpty()) {
        return source;
    }

    const int extra = std::max(kMinimumExtra, static_cast<int>(std::ceil(source.size() * kGrowth)));

    QString padding;
    padding.reserve(extra + 12);
    for (int i = 0; padding.size() < extra; ++i) {
        if (!padding.isEmpty()) {
            padding += QLatin1Char(' ');
        }
        padding += fillerWords().at(i % fillerWords().size());
    }

    return QStringLiteral("«") + source + QLatin1Char(' ') + padding + QStringLiteral("»");
}

QString PseudoLocalizationTranslator::translate(const char* context, const char* source_text,
                                                const char* disambiguation, int n) const {
    Q_UNUSED(context);
    Q_UNUSED(disambiguation);
    Q_UNUSED(n);
    if (source_text == nullptr) {
        return {};
    }
    return ExpandForPseudoLocalization(QString::fromUtf8(source_text));
}

bool PseudoLocalizationTranslator::isEmpty() const {
    return false;
}

} // namespace exosnap::quick
