#include "StatusPill.h"

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
    switch (tone) {
    case StatusPill::Tone::Ready:
        return {QColor("#74c08a"), QColor(116, 192, 138, 84), QColor(116, 192, 138, 20), QColor("#74c08a")};
    case StatusPill::Tone::Recording:
        return {QColor("#f1b400"), QColor(241, 180, 0, 96), QColor(241, 180, 0, 28), QColor("#f1b400")};
    case StatusPill::Tone::Warn:
        return {QColor("#e8a14a"), QColor(232, 161, 74, 84), QColor(232, 161, 74, 18), QColor("#e8a14a")};
    case StatusPill::Tone::Blocked:
        return {QColor("#e26a5a"), QColor(226, 106, 90, 86), QColor(226, 106, 90, 18), QColor("#e26a5a")};
    case StatusPill::Tone::Neutral:
    default:
        return {QColor("#c7c0b1"), QColor("#3a342c"), QColor("#1f1c17"), QColor("#8a8275")};
    }
}

} // namespace

StatusPill::StatusPill(QWidget* parent) : QWidget(parent), text_(QStringLiteral("READY")) {
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
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

QString StatusPill::text() const {
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
    mono_font.setLetterSpacing(QFont::AbsoluteSpacing, 1.0);
    QFontMetrics fm(mono_font);
    const int text_width = fm.horizontalAdvance(text_.isEmpty() ? QStringLiteral("STATE") : text_);
    const int dot_width = dot_visible_ ? 11 : 0;
    return {text_width + dot_width + 18, 24};
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
    painter.drawRoundedRect(bounds, 2.0, 2.0);

    QFont mono_font = font();
    mono_font.setLetterSpacing(QFont::AbsoluteSpacing, 1.0);
    painter.setFont(mono_font);
    painter.setPen(colors.text);

    int x = 7;
    if (dot_visible_) {
        painter.save();
        painter.setOpacity(dot_opacity_);
        painter.setPen(Qt::NoPen);
        painter.setBrush(colors.dot);
        painter.drawEllipse(QRectF(7.0, (height() - 6.0) * 0.5, 6.0, 6.0));
        painter.restore();
        x += 11;
        painter.setPen(colors.text);
    }

    painter.drawText(QRect(x, 0, width() - x - 6, height()), Qt::AlignVCenter | Qt::AlignLeft, text_);
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
