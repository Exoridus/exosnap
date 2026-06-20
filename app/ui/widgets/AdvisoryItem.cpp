#include "AdvisoryItem.h"

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "../theme/ExoSnapPalette.h"
#include "../theme/ExoSnapTheme.h"
#include "../theme/LucideIcon.h"

namespace exosnap::ui::widgets {

using namespace exosnap::ui::theme;

AdvisoryItem::AdvisoryItem(QWidget* parent) : QWidget(parent) {
    // -- Status icon label (fixed 30x30) --
    status_icon_label_ = new QLabel(this);
    status_icon_label_->setFixedSize(30, 30);
    status_icon_label_->setAlignment(Qt::AlignCenter);
    status_icon_label_->setStyleSheet(QStringLiteral("background: %1; border-radius: 9px;")
                                          .arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().line)));

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

    // -- Time label --
    time_label_ = new QLabel(this);
    time_label_->setAlignment(Qt::AlignRight | Qt::AlignTop);
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

    // -- Text VBox (title + body) --
    auto* text_box = new QVBoxLayout;
    text_box->setContentsMargins(0, 0, 0, 0);
    text_box->setSpacing(2);
    text_box->addWidget(title_label_);
    text_box->addWidget(body_label_);

    // -- Right VBox (time + dot) --
    auto* right_box = new QVBoxLayout;
    right_box->setContentsMargins(0, 0, 0, 0);
    right_box->setSpacing(4);
    right_box->addWidget(time_label_);
    right_box->addWidget(unread_dot_, 0, Qt::AlignRight);
    right_box->addStretch();

    // -- Row 1 --
    auto* row1 = new QHBoxLayout;
    row1->setContentsMargins(0, 0, 0, 0);
    row1->setSpacing(10);
    row1->addWidget(status_icon_label_, 0, Qt::AlignTop);
    row1->addLayout(text_box, 1);
    row1->addLayout(right_box, 0);

    // -- Actions container (row 2, optional) --
    actions_container_ = new QWidget(this);
    actions_layout_ = new QHBoxLayout(actions_container_);
    actions_layout_->setContentsMargins(0, 0, 0, 0);
    actions_layout_->setSpacing(6);
    actions_layout_->addStretch();
    actions_container_->setVisible(false);

    // -- Main VBox --
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(0, 0, 0, 0);
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
    updateUnreadDot();
}

void AdvisoryItem::addAction(const QString& id, const QString& label, bool isDeepLink) {
    actions_.append({id, label, isDeepLink});

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
    QString icon_name;
    QString icon_color;

    if (status_ == QStringLiteral("success")) {
        icon_name = QStringLiteral("check-circle");
        icon_color = QString::fromUtf8(exosnap::ui::theme::ActiveTheme().success);
    } else if (status_ == QStringLiteral("caution")) {
        icon_name = QStringLiteral("alert-triangle");
        icon_color = QString::fromUtf8(exosnap::ui::theme::ActiveTheme().caution);
    } else if (status_ == QStringLiteral("error")) {
        icon_name = QStringLiteral("x-circle");
        icon_color = QString::fromUtf8(exosnap::ui::theme::ActiveTheme().error);
    } else {
        // "info" and anything else
        icon_name = QStringLiteral("info");
        icon_color = QString::fromUtf8(exosnap::ui::theme::ActiveTheme().ac);
    }

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
