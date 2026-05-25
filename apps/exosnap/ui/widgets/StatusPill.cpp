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
        return {QColor("#74c08a"), QColor("#74c08a"), QColor(116, 192, 138, 0), QColor("#74c08a")};
    case StatusPill::Tone::Recording:
        return {QColor("#d7a744"), QColor("#d7a744"), QColor(215, 167, 68, 26), QColor("#d7a744")};
    case StatusPill::Tone::Warn:
        return {QColor("#c99550"), QColor("#c99550"), QColor(201, 149, 80, 0), QColor("#c99550")};
    case StatusPill::Tone::Blocked:
        return {QColor("#e26a5a"), QColor("#e26a5a"), QColor(226, 106, 90, 0), QColor("#e26a5a")};
    case StatusPill::Tone::Neutral:
    default:
        return {QColor("#c7c0b1"), QColor("#353330"), QColor(53, 51, 48, 0), QColor("#8d8880")};
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
    const int dot_width = dot_visible_ ? 7 : 0;
    return {text_width + dot_width + 12, 18};
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
    painter.drawRoundedRect(bounds, 3.0, 3.0);

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
        painter.drawEllipse(QRectF(6.0, (height() - 4.0) * 0.5, 4.0, 4.0));
        painter.restore();
        x += 7;
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
