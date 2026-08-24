#include "ProgressRing.h"

#include <QAccessible>
#include <QAccessibleValueChangeEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QRectF>
#include <algorithm>
#include <cmath>

#include "UpdaterTheme.h"

namespace exosnap::updater {

namespace {
constexpr int kSize = 120;
constexpr double kArc = 8.0; // arc/track stroke width

int WholePercent(double value01) {
    return static_cast<int>(std::round(value01 * 100.0));
}
} // namespace

ProgressRing::ProgressRing(QWidget* parent) : QWidget(parent) {
    setFixedSize(kSize, kSize);
}

void ProgressRing::setValue(double value01) {
    const double next = std::clamp(value01, 0.0, 1.0);
    // Announced per whole percent, not per call: a download reports bytes many
    // times a second and a screen reader that repeats "42 percent" for every
    // one of them drowns out everything else on the surface.
    const bool announce = WholePercent(next) != WholePercent(value_);
    value_ = next;
    if (announce && QAccessible::isActive()) {
        QAccessibleValueChangeEvent event(this, WholePercent(value_));
        QAccessible::updateAccessibility(&event);
    }
    update();
}

void ProgressRing::setVariant(TerminalVariant variant) {
    variant_ = variant;
    update();
}

void ProgressRing::setIndeterminate(bool indeterminate) {
    if (indeterminate_ == indeterminate)
        return;
    indeterminate_ = indeterminate;
    update();
}

double ProgressRing::value() const {
    return value_;
}

bool ProgressRing::isIndeterminate() const {
    return indeterminate_;
}

QString ProgressRing::progressDescription() const {
    switch (variant_) {
    case TerminalVariant::Amber:
        return QStringLiteral("Update didn't complete");
    case TerminalVariant::Red:
        return QStringLiteral("Update failed");
    case TerminalVariant::Green:
    case TerminalVariant::RebootRequired:
    case TerminalVariant::Success:
        return QStringLiteral("Update complete");
    case TerminalVariant::None:
        break;
    }
    return indeterminate_ ? QStringLiteral("Preparing update, progress not measurable yet")
                          : QStringLiteral("%1 percent").arg(WholePercent(value_));
}

QSize ProgressRing::sizeHint() const {
    return {kSize, kSize};
}

void ProgressRing::paintEvent(QPaintEvent*) {
    using namespace theme;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const double inset = kArc / 2.0 + 1.0;
    const QRectF arcRect(inset, inset, width() - 2 * inset, height() - 2 * inset);

    const bool terminal = variant_ != TerminalVariant::None;

    // Faint full-circle track.
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(ringTrack(), kArc, Qt::SolidLine, Qt::RoundCap));
    p.drawArc(arcRect, 0, 360 * 16);

    // Resolve the tone for the progress arc / terminal glyph.
    QColor tone = mint();
    switch (variant_) {
    case TerminalVariant::Amber:
        tone = caution();
        break;
    case TerminalVariant::Red:
        tone = error();
        break;
    case TerminalVariant::Green:
        tone = success();
        break;
    case TerminalVariant::RebootRequired:
        tone = success();
        break;
    case TerminalVariant::Success:
        tone = mint();
        break;
    case TerminalVariant::None:
        tone = mint();
        break;
    }

    if (terminal) {
        // Full ring in the tone, then a tinted glyph pill in the centre.
        p.setPen(QPen(tone, kArc, Qt::SolidLine, Qt::RoundCap));
        p.drawArc(arcRect, 0, 360 * 16);

        const double pill = 56.0;
        const QRectF pillRect((width() - pill) / 2.0, (height() - pill) / 2.0, pill, pill);
        p.setPen(QPen(statusBorder(tone), 1.0));
        p.setBrush(statusDim(tone));
        p.drawEllipse(pillRect);
        p.setBrush(Qt::NoBrush);

        const QRectF g = pillRect.adjusted(15, 15, -15, -15);
        if (variant_ == TerminalVariant::Amber)
            paintWarning(p, g, tone, 2.4);
        else if (variant_ == TerminalVariant::Red)
            paintCross(p, g, tone, 2.6);
        else
            paintCheck(p, g, tone, 2.8);
        return;
    }

    // Pre-flight: the track alone. Nothing has been measured, so a number here
    // would be invented and an arc would claim a fraction that does not exist.
    // The status line under the ring says what the updater is doing instead.
    if (indeterminate_)
        return;

    // In-progress: mint arc from the top (−90°), clockwise, plus percent text.
    if (value_ > 0.0) {
        const int span = -static_cast<int>(std::round(value_ * 360.0 * 16.0));
        p.setPen(QPen(tone, kArc, Qt::SolidLine, Qt::RoundCap));
        p.drawArc(arcRect, 90 * 16, span);
    }

    const int pct = static_cast<int>(std::round(value_ * 100.0));
    p.setPen(ink());
    QFont numFont = mono(34, QFont::DemiBold);
    p.setFont(numFont);
    const QRectF numRect(0, 0, width(), height());
    QRectF numArea = numRect.adjusted(0, height() * 0.30, 0, -height() * 0.34);
    p.drawText(numArea, Qt::AlignHCenter | Qt::AlignVCenter, QString::number(pct));

    p.setPen(dim());
    p.setFont(mono(10, QFont::Medium));
    QRectF capArea = numRect.adjusted(0, height() * 0.58, 0, -height() * 0.16);
    p.drawText(capArea, Qt::AlignHCenter | Qt::AlignVCenter, QStringLiteral("percent"));
}

} // namespace exosnap::updater