#include "StatusPill.h"

#include "../theme/ExoSnapPalette.h"

#include <QFontMetrics>
#include <QPaintEvent>
#include <QPainter>
#include <QTimer>

namespace exosnap::ui::widgets {
namespace {

struct ToneColors {
    QColor text;
    QColor border;
    QColor background;
    QColor dot;
};

ToneColors ColorsFor(StatusPill::Tone tone) {
    using exosnap::ui::theme::ExoSnapPalette;

    switch (tone) {
    case StatusPill::Tone::Ready:
        return {QColor(ExoSnapPalette::kOk), QColor(120, 218, 149, 110), QColor(120, 218, 149, 28),
                QColor(ExoSnapPalette::kOk)};
    case StatusPill::Tone::Recording:
        return {QColor(ExoSnapPalette::kErr), QColor(240, 91, 84, 122), QColor(240, 91, 84, 36),
                QColor(ExoSnapPalette::kErr)};
    case StatusPill::Tone::Warn:
        return {QColor(ExoSnapPalette::kWarn), QColor(194, 150, 83, 118), QColor(194, 150, 83, 24),
                QColor(ExoSnapPalette::kWarn)};
    case StatusPill::Tone::Blocked:
        return {QColor(ExoSnapPalette::kErr), QColor(240, 91, 84, 128), QColor(240, 91, 84, 42),
                QColor(ExoSnapPalette::kErr)};
    case StatusPill::Tone::Neutral:
    default:
        return {QColor(ExoSnapPalette::kText1), QColor(ExoSnapPalette::kLine2), QColor(50, 46, 43, 0),
                QColor(ExoSnapPalette::kText2)};
    }
}

} // namespace

StatusPill::StatusPill(QWidget* parent) : QWidget(parent), text_(QStringLiteral("READY")) {
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    QFont mono_font = font();
    mono_font.setFamilies(
        {QStringLiteral("JetBrains Mono"), QStringLiteral("Cascadia Mono"), QStringLiteral("Consolas")});
    mono_font.setPointSizeF(10.5);
    setFont(mono_font);
    blink_timer_ = new QTimer(this);
    blink_timer_->setInterval(700);
    connect(blink_timer_, &QTimer::timeout, this, &StatusPill::advanceBlinkFrame);
    updateBlinkState();
}

void StatusPill::setTone(Tone tone) {
    if (tone_ == tone)
        return;
    tone_ = tone;
    updateBlinkState();
    update();
}

StatusPill::Tone StatusPill::tone() const noexcept {
    return tone_;
}

void StatusPill::setText(const QString& text) {
    if (text_ == text)
        return;
    text_ = text;
    updateGeometry();
    update();
}

const QString& StatusPill::text() const {
    return text_;
}

void StatusPill::setDotVisible(bool visible) {
    if (dot_visible_ == visible)
        return;
    dot_visible_ = visible;
    updateBlinkState();
    updateGeometry();
    update();
}

bool StatusPill::isDotVisible() const noexcept {
    return dot_visible_;
}

QSize StatusPill::sizeHint() const {
    QFont mono_font = font();
    mono_font.setLetterSpacing(QFont::AbsoluteSpacing, 0.8);
    QFontMetrics fm(mono_font);
    const int text_width = fm.horizontalAdvance(text_.isEmpty() ? QStringLiteral("STATE") : text_);
    const int dot_width = dot_visible_ ? 9 : 0;
    return {text_width + dot_width + 14, 20};
}

QSize StatusPill::minimumSizeHint() const {
    return sizeHint();
}

void StatusPill::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const auto colors = ColorsFor(tone_);
    const QRectF bounds = rect().adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(QPen(colors.border, 1.0));
    painter.setBrush(colors.background);
    painter.drawRoundedRect(bounds, 7.0, 7.0);

    QFont mono_font = font();
    mono_font.setLetterSpacing(QFont::AbsoluteSpacing, 0.8);
    painter.setFont(mono_font);
    painter.setPen(colors.text);

    int x = 6;
    if (dot_visible_) {
        painter.save();
        painter.setOpacity(dot_opacity_);
        painter.setPen(Qt::NoPen);
        painter.setBrush(colors.dot);
        painter.drawEllipse(QRectF(6.0, (height() - 5.0) * 0.5, 5.0, 5.0));
        painter.restore();
        x += 9;
        painter.setPen(colors.text);
    }

    painter.drawText(QRect(x, 0, width() - x - 5, height()), Qt::AlignVCenter | Qt::AlignLeft, text_);
}

void StatusPill::updateBlinkState() {
    const bool should_blink = dot_visible_ && tone_ == Tone::Recording;
    if (should_blink) {
        if (!blink_timer_->isActive()) {
            blink_low_phase_ = false;
            dot_opacity_ = 1.0;
            blink_timer_->start();
        }
        return;
    }

    if (blink_timer_->isActive())
        blink_timer_->stop();
    blink_low_phase_ = false;
    if (dot_opacity_ != 1.0) {
        dot_opacity_ = 1.0;
        update();
    }
}

void StatusPill::advanceBlinkFrame() {
    blink_low_phase_ = !blink_low_phase_;
    dot_opacity_ = blink_low_phase_ ? 0.35 : 1.0;
    update();
}

} // namespace exosnap::ui::widgets
