#include "StatusPill.h"

#include "../theme/ExoSnapTheme.h"

#include <QFontMetrics>
#include <QPaintEvent>
#include <QPainter>
#include <QTimer>

namespace exosnap::ui::widgets {
namespace {

struct ToneColors {
    QColor text;   // text + dot share the tone hue
    QColor border; // outline (only painted when `outlined`)
    bool outlined; // calm tones (Ready/Neutral) draw no outline; active/alert tones do
};

ToneColors ColorsFor(StatusPill::Tone tone) {
    const auto& t = exosnap::ui::theme::ActiveTheme();
    const QColor ok(QString::fromUtf8(t.success));
    const QColor err(QString::fromUtf8(t.error));
    const QColor warn(QString::fromUtf8(t.caution));
    const QColor mut(QString::fromUtf8(t.mut));
    const QColor info(QString::fromUtf8(t.ac));

    // DESIGN-FIDELITY: the titlebar status pill follows the newest Mappe
    // (suite-kit.jsx:73-91 StatusPill). The recipe is calm: NO background fill on ANY
    // state; a 1px outline (tone @ ~0.44 → alpha 112) appears only on the active/alert
    // tones (Recording · Warn · Blocked · Info). Ready/Neutral are a bare coloured dot +
    // coloured text with no outline at all. Only the outline visibility differs by tone.
    // (The previous fill-0.13 + border-0.40 recipe on every tone came from the archived
    // spec.jsx — replaced.)
    auto outline = [](QColor c) {
        c.setAlpha(112); // ≈ 0.44, the frozen status-border alpha (themes.jsx sB)
        return c;
    };

    switch (tone) {
    case StatusPill::Tone::Ready:
        return {ok, QColor(), false};
    case StatusPill::Tone::Recording:
        return {err, outline(err), true};
    case StatusPill::Tone::Warn:
        return {warn, outline(warn), true};
    case StatusPill::Tone::Blocked:
        return {err, outline(err), true};
    case StatusPill::Tone::Info:
        return {info, outline(info), true};
    case StatusPill::Tone::Neutral:
    default:
        return {mut, QColor(), false};
    }
}

} // namespace

StatusPill::StatusPill(QWidget* parent) : QWidget(parent), text_(QStringLiteral("Ready")) {
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    // DESIGN-FIDELITY: the Mappe pill is UI sans (HT.ui ≈ Hanken Grotesk), weight 500 —
    // NOT a mono code badge. Inherit the app's default UI family and only set size/weight.
    QFont ui_font = font();
    ui_font.setPointSizeF(10.0);
    ui_font.setWeight(QFont::Medium);
    setFont(ui_font);
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
    QFontMetrics fm(font());
    const int text_width = fm.horizontalAdvance(text_.isEmpty() ? QStringLiteral("State") : text_);
    const int dot_width = dot_visible_ ? 7 + 7 : 0; // dot (7) + gap (7)
    // ~11px horizontal padding each side (Mappe outlined padding 4px 11px) + a little slack.
    return {text_width + dot_width + 22, 22};
}

QSize StatusPill::minimumSizeHint() const {
    return sizeHint();
}

void StatusPill::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const auto colors = ColorsFor(tone_);

    // Outline only on active/alert tones; NO background fill on any state (Mappe recipe).
    if (colors.outlined) {
        const QRectF bounds = rect().adjusted(0.5, 0.5, -0.5, -0.5);
        const qreal radius = bounds.height() / 2.0; // full pill (Mappe borderRadius 999)
        painter.setPen(QPen(colors.border, 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(bounds, radius, radius);
    }

    painter.setFont(font());

    int x = 11; // left padding (matches the Mappe's 11px inset)
    if (dot_visible_) {
        painter.save();
        painter.setOpacity(dot_opacity_);
        painter.setPen(Qt::NoPen);
        painter.setBrush(colors.text); // dot shares the tone hue
        painter.drawEllipse(QRectF(x, (height() - 7.0) * 0.5, 7.0, 7.0));
        painter.restore();
        x += 7 + 7; // dot + gap
    }

    painter.setPen(colors.text);
    painter.drawText(QRect(x, 0, width() - x - 8, height()), Qt::AlignVCenter | Qt::AlignLeft, text_);
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
