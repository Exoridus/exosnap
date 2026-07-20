#include "AdvisoryItem.h"

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

#include "../theme/ExoSnapPalette.h"
#include "../theme/ExoSnapTheme.h"
#include "../theme/LucideIcon.h"

namespace exosnap::ui::widgets {

using namespace exosnap::ui::theme;

AdvisoryItem::AdvisoryItem(QWidget* parent) : QWidget(parent) {
    // A bare QWidget does not paint its QSS background unless it opts in; without
    // this the [advisoryUnread="true"] rule silently never renders (VG-6).
    setAttribute(Qt::WA_StyledBackground, true);

    // -- Status icon label (fixed 30x30) --
    // Background/border are set per-severity in updateStatusIcon() (VG-8).
    status_icon_label_ = new QLabel(this);
    status_icon_label_->setFixedSize(30, 30);
    status_icon_label_->setAlignment(Qt::AlignCenter);

    // -- Title label --
    title_label_ = new QLabel(this);
    title_label_->setObjectName(QStringLiteral("advisoryTitle"));
    {
        QFont f = title_label_->font();
        f.setPixelSize(13);
        f.setWeight(QFont::DemiBold);
        title_label_->setFont(f);
    }

    // -- Body label --
    body_label_ = new QLabel(this);
    body_label_->setObjectName(QStringLiteral("advisoryBody"));
    body_label_->setWordWrap(true);
    {
        QFont f = body_label_->font();
        f.setPixelSize(12);
        body_label_->setFont(f);
    }

    // -- Time label (inline in title row, VG-5) --
    time_label_ = new QLabel(this);
    time_label_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    {
        QFont f;
        f.setFamily(QStringLiteral("IBM Plex Mono"));
        f.setPixelSize(10);
        time_label_->setFont(f);
    }

    // -- Unread dot (6x6) --
    // Kept permanently in the layout (its colour, not its visibility, tracks the
    // unread state) so its slot never collapses. A leading dot used to sit before
    // the title and shift it right only on unread rows, leaving the title ragged
    // against its own body and against read rows; anchoring it as a trailing,
    // always-reserved slot keeps every title and timestamp on a consistent edge.
    unread_dot_ = new QWidget(this);
    unread_dot_->setFixedSize(6, 6);

    // -- Title row: [title] [time] [unread dot] inline (VG-5) --
    // Title takes the stretch so its left edge always aligns with the body below;
    // the timestamp and the reserved unread-dot slot trail on the right.
    auto* title_row = new QHBoxLayout;
    title_row->setContentsMargins(0, 0, 0, 0);
    title_row->setSpacing(8);
    title_row->addWidget(title_label_, 1);
    title_row->addWidget(time_label_, 0, Qt::AlignVCenter);
    title_row->addWidget(unread_dot_, 0, Qt::AlignVCenter);

    // -- Text VBox (title row + body) --
    auto* text_box = new QVBoxLayout;
    text_box->setContentsMargins(0, 0, 0, 0);
    text_box->setSpacing(2);
    text_box->addLayout(title_row);
    text_box->addWidget(body_label_);

    // -- Row 1 --
    auto* row1 = new QHBoxLayout;
    row1->setContentsMargins(0, 0, 0, 0);
    row1->setSpacing(10);
    row1->addWidget(status_icon_label_, 0, Qt::AlignTop);
    row1->addLayout(text_box, 1);

    // -- Actions container (row 2, optional) --
    actions_container_ = new QWidget(this);
    actions_layout_ = new QHBoxLayout(actions_container_);
    actions_layout_->setContentsMargins(0, 0, 0, 0);
    actions_layout_->setSpacing(6);
    actions_layout_->addStretch();
    actions_container_->setVisible(false);

    // -- Main VBox --
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(15, 13, 15, 13); // VG-6: padding 13px 15px
    main_layout->setSpacing(8);
    main_layout->addLayout(row1);
    main_layout->addWidget(actions_container_);

    // Text colours, the severity status icon, and the unread dot all re-derive on a
    // theme switch (applyTheme runs once now and on every ReapplyTheme).
    ui::theme::OnThemeChanged(this, [this]() { applyTheme(); });
}

void AdvisoryItem::applyTheme() {
    const auto& t = exosnap::ui::theme::ActiveTheme();
    title_label_->setStyleSheet(QStringLiteral("color: %1;").arg(QString::fromUtf8(t.ink)));
    body_label_->setStyleSheet(QStringLiteral("color: %1;").arg(QString::fromUtf8(t.mut)));
    time_label_->setStyleSheet(QStringLiteral("color: %1;").arg(QString::fromUtf8(t.dim)));
    updateStatusIcon();
    updateUnreadDot();
}

void AdvisoryItem::setStatus(const QString& status) {
    if (status_ == status)
        return;
    status_ = status;
    updateStatusIcon();
    updateUnreadDot();
}

void AdvisoryItem::setTitle(const QString& title) {
    title_label_->setText(title);
}

void AdvisoryItem::setBody(const QString& body) {
    body_label_->setText(body);
}

void AdvisoryItem::setTimeLabel(const QString& time) {
    time_label_->setText(time);
}

void AdvisoryItem::setUnread(bool unread) {
    if (unread_ == unread)
        return;
    unread_ = unread;
    // VG-6: drive the [advisoryUnread="true"] QSS rule for the subtle unread background.
    setProperty("advisoryUnread", unread_);
    style()->unpolish(this);
    style()->polish(this);
    updateUnreadDot();
}

void AdvisoryItem::addAction(const QString& id, const QString& label, bool isDeepLink) {
    const QString display_label = isDeepLink ? label + QStringLiteral(" ›") : label;
    auto* btn = new QPushButton(display_label, actions_container_);
    btn->setFlat(true);
    btn->setProperty("advisoryAction", true);

    connect(btn, &QPushButton::clicked, this, [this, id, isDeepLink]() {
        emit actionTriggered(id);
        if (isDeepLink)
            emit deepLinkRequested();
    });

    // The constructor seeds actions_layout_ with a single LEADING stretch
    // (addStretch() before any buttons exist), so every button appended after
    // it lands to the right of that stretch — the buttons hug the row's right
    // edge, consistent with trailing-action alignment elsewhere in the app.
    // (Previously this inserted each button just before the stretch, which
    // left the stretch trailing and left-aligned the buttons instead.)
    actions_layout_->addWidget(btn);
    actions_container_->setVisible(true);
}

void AdvisoryItem::updateStatusIcon() {
    // VG-8: severity-based icon bg (dim) and border (b) per Canon STATUS map.
    const auto& t = exosnap::ui::theme::ActiveTheme();
    const bool dark = (t.kind == exosnap::ui::theme::ThemeKind::Dark);
    const double s_dim = dark ? 0.13 : 0.12;
    const double s_b = dark ? 0.44 : 0.42;
    const double ac_dim = dark ? 0.14 : 0.12;
    const double ac_b2 = dark ? 0.60 : 0.52;

    QString icon_name;
    QString icon_color;
    QString bg_color;
    QString border_color;

    if (status_ == QStringLiteral("success")) {
        icon_name = QStringLiteral("check-circle");
        icon_color = QString::fromUtf8(t.success);
        const QColor base = exosnap::ui::theme::ParseThemeColor(t.success);
        bg_color = exosnap::ui::theme::ThemeRgba(base, s_dim);
        border_color = exosnap::ui::theme::ThemeRgba(base, s_b);
    } else if (status_ == QStringLiteral("caution")) {
        icon_name = QStringLiteral("alert-triangle");
        icon_color = QString::fromUtf8(t.caution);
        const QColor base = exosnap::ui::theme::ParseThemeColor(t.caution);
        bg_color = exosnap::ui::theme::ThemeRgba(base, s_dim);
        border_color = exosnap::ui::theme::ThemeRgba(base, s_b);
    } else if (status_ == QStringLiteral("error")) {
        icon_name = QStringLiteral("x-circle");
        icon_color = QString::fromUtf8(t.error);
        const QColor base = exosnap::ui::theme::ParseThemeColor(t.error);
        bg_color = exosnap::ui::theme::ThemeRgba(base, s_dim);
        border_color = exosnap::ui::theme::ThemeRgba(base, s_b);
    } else {
        // "info" and anything else
        icon_name = QStringLiteral("info");
        icon_color = QString::fromUtf8(t.ac);
        const QColor base = exosnap::ui::theme::ParseThemeColor(t.ac);
        bg_color = exosnap::ui::theme::ThemeRgba(base, ac_dim);
        border_color = exosnap::ui::theme::ThemeRgba(base, ac_b2);
    }

    status_icon_label_->setStyleSheet(
        QStringLiteral("background: %1; border: 1px solid %2; border-radius: 9px;").arg(bg_color, border_color));

    const qreal dpr = devicePixelRatioF();
    const QPixmap px = lucidePixmap(icon_name, icon_color, 18, dpr);
    status_icon_label_->setPixmap(px);
}

void AdvisoryItem::updateUnreadDot() {
    // The dot keeps its layout slot in every state (see the title-row comment);
    // read rows simply paint it transparent so nothing shifts between states.
    if (!unread_) {
        unread_dot_->setStyleSheet(QStringLiteral("background: transparent; border-radius: 3px;"));
        return;
    }
    // Dot color follows status color
    QString dot_color;
    if (status_ == QStringLiteral("success")) {
        dot_color = QString::fromUtf8(exosnap::ui::theme::ActiveTheme().success);
    } else if (status_ == QStringLiteral("caution")) {
        dot_color = QString::fromUtf8(exosnap::ui::theme::ActiveTheme().caution);
    } else if (status_ == QStringLiteral("error")) {
        dot_color = QString::fromUtf8(exosnap::ui::theme::ActiveTheme().error);
    } else {
        dot_color = QString::fromUtf8(exosnap::ui::theme::ActiveTheme().ac);
    }
    unread_dot_->setStyleSheet(QString::fromLatin1("background: ") + dot_color +
                               QStringLiteral("; border-radius: 3px;"));
}

} // namespace exosnap::ui::widgets
