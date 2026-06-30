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
    // -- Status icon label (fixed 30x30) --
    // Background/border are set per-severity in updateStatusIcon() (VG-8).
    status_icon_label_ = new QLabel(this);
    status_icon_label_->setFixedSize(30, 30);
    status_icon_label_->setAlignment(Qt::AlignCenter);

    // -- Title label --
    title_label_ = new QLabel(this);
    {
        QFont f = title_label_->font();
        f.setPixelSize(13);
        f.setWeight(QFont::DemiBold);
        title_label_->setFont(f);
        title_label_->setStyleSheet(QString::fromLatin1("color: ") +
                                    QString::fromUtf8(exosnap::ui::theme::ActiveTheme().ink) + QLatin1Char(';'));
    }

    // -- Body label --
    body_label_ = new QLabel(this);
    body_label_->setWordWrap(true);
    {
        QFont f = body_label_->font();
        f.setPixelSize(12);
        body_label_->setFont(f);
        body_label_->setStyleSheet(QString::fromLatin1("color: ") +
                                   QString::fromUtf8(exosnap::ui::theme::ActiveTheme().mut) + QLatin1Char(';'));
    }

    // -- Time label (inline in title row, VG-5) --
    time_label_ = new QLabel(this);
    time_label_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    {
        QFont f;
        f.setFamily(QStringLiteral("IBM Plex Mono"));
        f.setPixelSize(10);
        time_label_->setFont(f);
        time_label_->setStyleSheet(QString::fromLatin1("color: ") +
                                   QString::fromUtf8(exosnap::ui::theme::ActiveTheme().dim) + QLatin1Char(';'));
    }

    // -- Unread dot (6x6) --
    unread_dot_ = new QWidget(this);
    unread_dot_->setFixedSize(6, 6);
    unread_dot_->setStyleSheet(QString::fromLatin1("background: ") +
                               QString::fromUtf8(exosnap::ui::theme::ActiveTheme().success) +
                               QStringLiteral("; border-radius: 3px;"));
    unread_dot_->setVisible(false);

    // -- Title row: [dot] [title] [time] inline (VG-5) --
    auto* title_row = new QHBoxLayout;
    title_row->setContentsMargins(0, 0, 0, 0);
    title_row->setSpacing(8);
    title_row->addWidget(unread_dot_, 0, Qt::AlignVCenter);
    title_row->addWidget(title_label_, 1);
    title_row->addWidget(time_label_, 0, Qt::AlignVCenter);

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

    updateStatusIcon();
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

    // Insert before the trailing stretch
    const int stretch_idx = actions_layout_->count() - 1;
    actions_layout_->insertWidget(stretch_idx, btn);
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
    unread_dot_->setVisible(unread_);
}

} // namespace exosnap::ui::widgets
