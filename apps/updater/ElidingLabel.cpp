#include "ElidingLabel.h"

#include <QFontMetrics>
#include <QResizeEvent>

namespace {
// The one character that makes a shortened string read as deliberate.
const QString& Ellipsis() {
    static const QString kEllipsis = QStringLiteral("…");
    return kEllipsis;
}
} // namespace

ElidingLabel::ElidingLabel(QWidget* parent) : QLabel(parent) {
    setWordWrap(false);
    setTextFormat(Qt::PlainText);
}

void ElidingLabel::setElideMode(Qt::TextElideMode mode) {
    if (elide_mode_ == mode)
        return;
    elide_mode_ = mode;
    applyElision();
}

Qt::TextElideMode ElidingLabel::elideMode() const {
    return elide_mode_;
}

void ElidingLabel::setFullText(const QString& text) {
    full_text_ = text;
    // The accessible name is the untruncated string: a screen reader has to
    // read the version the pill stands for, not the ellipsis the eye is given.
    setAccessibleName(text);
    applyElision();
    updateGeometry();
}

QString ElidingLabel::fullText() const {
    return full_text_;
}

bool ElidingLabel::isElided() const {
    return elided_;
}

// Derived from Qt rather than from the style sheet's numbers: the difference
// between what QLabel asks for and what its own text measures IS the padding +
// border the current style sheet applies. Reading it back this way keeps the
// pill's 12 px inset defined in exactly one place (the style sheet) instead of
// in two that can drift.
int ElidingLabel::styleInset() const {
    const int painted = fontMetrics().size(Qt::TextSingleLine, QLabel::text()).width();
    return qMax(0, QLabel::sizeHint().width() - painted);
}

int ElidingLabel::textAreaWidth() const {
    return qMax(0, width() - styleInset());
}

QSize ElidingLabel::sizeHint() const {
    QSize hint = QLabel::sizeHint();
    hint.setWidth(styleInset() + fontMetrics().size(Qt::TextSingleLine, full_text_).width());
    return hint;
}

QSize ElidingLabel::minimumSizeHint() const {
    QSize hint = QLabel::sizeHint();
    hint.setWidth(styleInset() + fontMetrics().size(Qt::TextSingleLine, Ellipsis()).width());
    return hint;
}

void ElidingLabel::resizeEvent(QResizeEvent* event) {
    QLabel::resizeEvent(event);
    applyElision();
}

void ElidingLabel::applyElision() {
    const int available = textAreaWidth();
    // Before the first layout pass there is no width to elide against, and the
    // full string is the right answer then -- resizeEvent runs this again with a
    // real width. sizeHint() is a function of full_text_ only, so re-eliding on
    // every resize cannot oscillate.
    const QString shown =
        available <= 0 ? full_text_ : fontMetrics().elidedText(full_text_, elide_mode_, available);
    elided_ = shown != full_text_;
    setToolTip(elided_ ? full_text_ : QString());
    if (QLabel::text() != shown)
        QLabel::setText(shown);
}
