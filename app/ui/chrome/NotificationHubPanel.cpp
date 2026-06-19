#include "NotificationHubPanel.h"

#include "../theme/ExoSnapMetrics.h"
#include "../theme/ExoSnapPalette.h"
#include "../theme/LucideIcon.h"
#include "../widgets/AdvisoryItem.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

namespace exosnap::ui::chrome {

using P = ui::theme::ExoSnapPalette;
using M = ui::theme::ExoSnapMetrics;

NotificationHubPanel::NotificationHubPanel(QWidget* parent) : QFrame(parent) {
    setObjectName("notificationHubPanel");
    setWindowFlags(Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setFixedWidth(380);
    setFrameShape(QFrame::NoFrame);

    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);

    // Header
    auto* header = new QWidget(this);
    header->setObjectName("hubHeader");
    header->setFixedHeight(48);
    auto* header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(16, 0, 16, 0);
    header_layout->setSpacing(8);

    auto* bell_icon_lbl = new QLabel(header);
    const QPixmap bell_pix = ui::theme::lucidePixmap(QStringLiteral("bell"), QString::fromLatin1(P::kText0), 15);
    bell_icon_lbl->setPixmap(bell_pix);
    bell_icon_lbl->setFixedSize(15, 15);

    auto* title_lbl = new QLabel(QStringLiteral("Notifications"), header);
    title_lbl->setObjectName("hubTitle");

    mark_all_read_label_ = new QLabel(QStringLiteral("Mark all read"), header);
    mark_all_read_label_->setObjectName("hubMarkAllRead");
    mark_all_read_label_->hide();

    header_layout->addWidget(bell_icon_lbl, 0, Qt::AlignVCenter);
    header_layout->addWidget(title_lbl, 1, Qt::AlignVCenter);
    header_layout->addWidget(mark_all_read_label_, 0, Qt::AlignVCenter);

    // Divider below header
    auto* header_divider = new QFrame(this);
    header_divider->setFrameShape(QFrame::HLine);
    header_divider->setObjectName("hubHeaderDivider");
    header_divider->setFixedHeight(1);

    // Scroll area
    scroll_ = new QScrollArea(this);
    scroll_->setObjectName("hubScrollArea");
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);

    auto* scroll_content = new QWidget();
    scroll_content->setObjectName("hubScrollContent");
    list_layout_ = new QVBoxLayout(scroll_content);
    list_layout_->setContentsMargins(0, 6, 0, 6);
    list_layout_->setSpacing(0);

    // Empty state
    empty_state_ = new QWidget(scroll_content);
    empty_state_->setObjectName("hubEmptyState");
    auto* empty_layout = new QVBoxLayout(empty_state_);
    empty_layout->setContentsMargins(24, 40, 24, 40);
    empty_layout->setAlignment(Qt::AlignHCenter);

    auto* empty_icon_container = new QWidget(empty_state_);
    empty_icon_container->setFixedSize(44, 44);
    empty_icon_container->setObjectName("hubEmptyIcon");

    auto* empty_icon_lbl = new QLabel(empty_icon_container);
    const QPixmap check_pix =
        ui::theme::lucidePixmap(QStringLiteral("check-circle"), QString::fromLatin1(P::kText3), 20);
    empty_icon_lbl->setPixmap(check_pix);
    auto* empty_icon_inner = new QHBoxLayout(empty_icon_container);
    empty_icon_inner->setContentsMargins(0, 0, 0, 0);
    empty_icon_inner->addWidget(empty_icon_lbl, 0, Qt::AlignCenter);

    auto* empty_title = new QLabel(QStringLiteral("You’re all caught up"), empty_state_);
    empty_title->setObjectName("hubEmptyTitle");
    empty_title->setAlignment(Qt::AlignHCenter);

    auto* empty_body =
        new QLabel(QStringLiteral("Advisories about settings, updates and disk space land here."), empty_state_);
    empty_body->setObjectName("hubEmptyBody");
    empty_body->setAlignment(Qt::AlignHCenter);
    empty_body->setWordWrap(true);

    empty_layout->addWidget(empty_icon_container, 0, Qt::AlignHCenter);
    empty_layout->addSpacing(12);
    empty_layout->addWidget(empty_title);
    empty_layout->addSpacing(4);
    empty_layout->addWidget(empty_body);

    list_layout_->addWidget(empty_state_);
    list_layout_->addStretch(1);

    list_container_ = scroll_content;
    scroll_->setWidget(scroll_content);

    root_layout->addWidget(header);
    root_layout->addWidget(header_divider);
    root_layout->addWidget(scroll_);

    setMinimumHeight(80);
    setMaximumHeight(520);

    refreshEmptyState();
}

void NotificationHubPanel::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    QPainterPath path;
    path.addRoundedRect(rect(), M::kRadiusLg, M::kRadiusLg);

    // Fill
    p.fillPath(path, QColor(P::kBg1));

    // Border — ~kLine2 (rgba(255,255,255,0.12))
    p.setPen(QPen(QColor(255, 255, 255, 31), 1.0));
    p.drawPath(path);
}

void NotificationHubPanel::addAdvisory(const QString& id, const QString& status, const QString& title,
                                       const QString& body, const QString& time_label, bool unread,
                                       const QString& action_id, const QString& action_label, bool is_deep_link) {
    auto* item = new ui::widgets::AdvisoryItem(list_container_);
    item->setStatus(status);
    item->setTitle(title);
    item->setBody(body);
    if (!time_label.isEmpty())
        item->setTimeLabel(time_label);
    item->setUnread(unread);
    if (!action_id.isEmpty())
        item->addAction(action_id, action_label, is_deep_link);

    if (is_deep_link) {
        const QString captured_id = id;
        connect(item, &ui::widgets::AdvisoryItem::deepLinkRequested, this,
                [this, captured_id]() { emit deepLinkRequested(captured_id); });
    }

    addAdvisoryWidget(item);
}

void NotificationHubPanel::addAdvisoryWidget(ui::widgets::AdvisoryItem* item) {
    // Insert before the trailing stretch (last item in list_layout_).
    const int stretch_index = list_layout_->count() - 1;
    if (advisory_count_ > 0) {
        // Add a 1px divider before this item.
        auto* divider = new QFrame(list_container_);
        divider->setFrameShape(QFrame::HLine);
        divider->setObjectName("hubItemDivider");
        divider->setFixedHeight(1);
        list_layout_->insertWidget(stretch_index, divider);
    }
    list_layout_->insertWidget(stretch_index + (advisory_count_ > 0 ? 1 : 0), item);
    ++advisory_count_;
    refreshEmptyState();
}

void NotificationHubPanel::clearAdvisories() {
    // Remove everything except the empty_state_ and the final stretch.
    for (int i = list_layout_->count() - 1; i >= 0; --i) {
        QLayoutItem* li = list_layout_->itemAt(i);
        if (!li)
            continue;
        if (li->spacerItem())
            continue;
        QWidget* w = li->widget();
        if (w == empty_state_)
            continue;
        list_layout_->removeItem(li);
        if (w)
            w->deleteLater();
        delete li;
    }
    advisory_count_ = 0;
    refreshEmptyState();
}

void NotificationHubPanel::setDemoAdvisories(bool enabled) {
    clearAdvisories();
    if (!enabled)
        return;

    addAdvisory(QStringLiteral("update"), QStringLiteral("info"), QStringLiteral("Update available — 0.6.0"),
                QStringLiteral("Signature verified. Installs after your next recording."), QStringLiteral("now"),
                /*unread=*/true, QStringLiteral("update-view"), QStringLiteral("View in About"),
                /*is_deep_link=*/false);

    addAdvisory(QStringLiteral("audio-suboptimal"), QStringLiteral("caution"),
                QStringLiteral("Setting not optimal for your GPU"),
                QStringLiteral("Recording a Window with system-wide audio — switch to App audio to isolate it."),
                QStringLiteral("3m"),
                /*unread=*/true, QStringLiteral("audio-settings"), QStringLiteral("Open Audio setting"),
                /*is_deep_link=*/true);
}

void NotificationHubPanel::anchorToPoint(const QPoint& globalPos) {
    // Place so our top-right corner is at globalPos.
    move(globalPos.x() - width(), globalPos.y());
}

void NotificationHubPanel::refreshEmptyState() {
    const bool is_empty = (advisory_count_ == 0);
    empty_state_->setVisible(is_empty);
    if (mark_all_read_label_)
        mark_all_read_label_->setVisible(!is_empty);
}

} // namespace exosnap::ui::chrome
