#include "ProgressRing.h"

#include <QPaintEvent>
#include <QPainter>
#include <QRectF>
#include <algorithm>
#include <cmath>

#include "UpdaterTheme.h"

namespace {
constexpr int kSize = 120;
constexpr double kArc = 8.0; // arc/track stroke width
} // namespace

ProgressRing::ProgressRing(QWidget* parent) : QWidget(parent) {
    setFixedSize(kSize, kSize);
}

void ProgressRing::setValue(double value01) {
    value_ = std::clamp(value01, 0.0, 1.0);
    update();
}

void ProgressRing::setVariant(TerminalVariant variant) {
    variant_ = variant;
    update();
}

QSize ProgressRing::sizeHint() const {
    return {kSize, kSize};
}

void ProgressRing::paintEvent(QPaintEvent*) {
    using namespace updater_theme;
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
