#include "OperationalTitleBar.h"

#include "../brand/BrandMarkWidget.h"
#include "../theme/ExoSnapPalette.h"
#include "../widgets/StatusPill.h"

#include <QAbstractButton>
#include <QApplication>
#include <QButtonGroup>
#include <QColor>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayoutItem>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStyle>
#include <QWindow>

namespace exosnap::ui::chrome {

OperationalTitleBar::OperationalTitleBar(QWidget* parent) : QWidget(parent) {
    setObjectName("operationalTitleBar");
    setFixedHeight(kHeight);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setProperty("recording", false);

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Brand: aperture mark + lowercase two-tone wordmark (exo neutral · snap accent).
    auto* brand_slot = new QWidget(this);
    brand_slot->setObjectName("titlebarBrandSlot");
    auto* brand_layout = new QHBoxLayout(brand_slot);
    brand_layout->setContentsMargins(16, 0, 8, 0);
    brand_layout->setSpacing(8);

    brand_mark_ = new ui::brand::BrandMarkWidget(brand_slot);
    brand_mark_->setFixedSize(20, 20);

    auto* wordmark = new QLabel(brand_slot);
    wordmark->setProperty("labelRole", "titlebarWordmark");
    wordmark->setTextFormat(Qt::RichText);
    wordmark->setText(QStringLiteral("<span style=\"color:%1;\">exo</span><span style=\"color:%2;\">snap</span>")
                          .arg(QString::fromLatin1(theme::ExoSnapPalette::kText0),
                               QString::fromLatin1(theme::ExoSnapPalette::kAccent)));

    brand_layout->addWidget(brand_mark_, 0, Qt::AlignVCenter);
    brand_layout->addWidget(wordmark, 0, Qt::AlignVCenter);

    // Top navigation tabs (populated via setNavItems()).
    auto* nav_container = new QWidget(this);
    nav_container->setObjectName("titlebarNav");
    nav_layout_ = new QHBoxLayout(nav_container);
    nav_layout_->setContentsMargins(14, 0, 0, 0);
    nav_layout_->setSpacing(2);

    nav_group_ = new QButtonGroup(this);
    nav_group_->setExclusive(true);
    connect(nav_group_, &QButtonGroup::idClicked, this, &OperationalTitleBar::navPageRequested);

    // Flexible drag region between nav and status.
    auto* drag_slot = new QWidget(this);
    drag_slot->setObjectName("titlebarDragSlot");
    drag_slot->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    // Compact status pill bound to recording state.
    auto* status_slot = new QWidget(this);
    status_slot->setObjectName("titlebarStatusSlot");
    auto* status_layout = new QHBoxLayout(status_slot);
    status_layout->setContentsMargins(12, 0, 12, 0);
    status_layout->setSpacing(0);

    status_pill_ = new ui::widgets::StatusPill(status_slot);
    status_pill_->setObjectName("titlebarStatusChip");
    status_pill_->setText(QStringLiteral("Ready"));
    status_pill_->setTone(ui::widgets::StatusPill::Tone::Ready);
    status_layout->addWidget(status_pill_, 0, Qt::AlignVCenter);

    // Window controls.
    auto* controls = new QWidget(this);
    controls->setObjectName("titlebarControls");
    controls->setFixedWidth(138);
    auto* controls_layout = new QHBoxLayout(controls);
    controls_layout->setContentsMargins(0, 0, 0, 0);
    controls_layout->setSpacing(0);

    minimize_btn_ = new QPushButton("−", controls); // − MINUS SIGN
    maximize_btn_ = new QPushButton("□", controls); // □ WHITE SQUARE
    close_btn_ = new QPushButton("×", controls);    // × MULTIPLICATION SIGN

    for (QPushButton* button : {minimize_btn_, maximize_btn_, close_btn_}) {
        button->setObjectName("titlebarWindowButton");
        button->setFixedSize(46, kHeight);
        button->setFocusPolicy(Qt::NoFocus);
    }
    close_btn_->setProperty("windowControlRole", "close");

    controls_layout->addWidget(minimize_btn_);
    controls_layout->addWidget(maximize_btn_);
    controls_layout->addWidget(close_btn_);

    root->addWidget(brand_slot);
    root->addWidget(nav_container);
    root->addWidget(drag_slot, 1);
    root->addWidget(status_slot, 0, Qt::AlignVCenter);
    root->addWidget(controls);

    connect(minimize_btn_, &QPushButton::clicked, this, &OperationalTitleBar::minimizeRequested);
    connect(maximize_btn_, &QPushButton::clicked, this, &OperationalTitleBar::maximizeRestoreRequested);
    connect(close_btn_, &QPushButton::clicked, this, &OperationalTitleBar::closeRequested);

    refreshStatusChip();
}

void OperationalTitleBar::setNavItems(const QVector<NavItem>& items) {
    // Idempotent: drop any previously built tabs first.
    const QList<QAbstractButton*> existing = nav_group_->buttons();
    for (QAbstractButton* button : existing)
        nav_group_->removeButton(button);
    while (QLayoutItem* layout_item = nav_layout_->takeAt(0)) {
        if (QWidget* widget = layout_item->widget())
            widget->deleteLater();
        delete layout_item;
    }

    QWidget* nav_container = nav_layout_->parentWidget();
    for (const NavItem& item : items) {
        auto* tab = new QPushButton(item.label, nav_container);
        tab->setObjectName("titlebarNavTab");
        tab->setProperty("titlebarNavTab", true);
        tab->setFocusPolicy(Qt::NoFocus);
        tab->setCursor(Qt::PointingHandCursor);
        tab->setFixedHeight(kHeight);

        if (item.page_index >= 0) {
            tab->setCheckable(true);
            nav_group_->addButton(tab, item.page_index);
        } else {
            // Action item (About): opens a dialog and never stays highlighted.
            tab->setProperty("navAction", true);
            connect(tab, &QPushButton::clicked, this, &OperationalTitleBar::aboutRequested);
        }
        nav_layout_->addWidget(tab);
    }
}

void OperationalTitleBar::setActivePage(int page_index) {
    QAbstractButton* button = nav_group_->button(page_index);
    if (button == nullptr)
        return;
    const QSignalBlocker blocker(nav_group_);
    button->setChecked(true);
}

void OperationalTitleBar::setRecordingActive(bool recording) {
    if (recording_active_ == recording)
        return;
    recording_active_ = recording;
    if (!recording_active_)
        recording_drop_count_ = 0; // DF-11: reset drop count when recording stops
    setProperty("recording", recording_active_);
    if (brand_mark_ != nullptr)
        brand_mark_->setRecording(recording_active_);
    style()->unpolish(this);
    style()->polish(this);
    refreshStatusChip();
    update();
}

bool OperationalTitleBar::isRecordingActive() const noexcept {
    return recording_active_;
}

void OperationalTitleBar::setStatusLabel(const QString& status_text) {
    const QString normalized = status_text.trimmed().toUpper();
    status_label_ = normalized.isEmpty() ? QStringLiteral("READY") : normalized;
    refreshStatusChip();
}

void OperationalTitleBar::setRecordingDropCount(int drops) {
    if (recording_drop_count_ == drops)
        return;
    recording_drop_count_ = drops;
    refreshStatusChip();
}

void OperationalTitleBar::setMaximizedState(bool maximized) {
    maximize_btn_->setText(maximized ? "⧉" : "□"); // ⧉ TWO JOINED SQUARES — same visual weight as □
}

void OperationalTitleBar::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        const QPoint local = mapFromGlobal(event->globalPosition().toPoint());
        if (hitTestWindowButton(local) == WindowButtonHit::None) {
            // Show the move cursor immediately on press.
            QApplication::setOverrideCursor(Qt::SizeAllCursor);
            move_cursor_active_ = true;

            if (window()->isMaximized()) {
                // Defer move until the user actually drags — a bare click on a
                // maximized titlebar must not restore the window.
                drag_press_global_pos_ = event->globalPosition().toPoint();
                tracking_drag_from_max_ = true;
                event->accept();
                return;
            }
            if (QWindow* win = window()->windowHandle()) {
                win->startSystemMove();
                event->accept();
                return;
            }
        }
    }
    tracking_drag_from_max_ = false;
    QWidget::mousePressEvent(event);
}

void OperationalTitleBar::mouseMoveEvent(QMouseEvent* event) {
    if (tracking_drag_from_max_ && window()->isMaximized() && (event->buttons() & Qt::LeftButton)) {
        const QPoint current = event->globalPosition().toPoint();
        if ((current - drag_press_global_pos_).manhattanLength() > 5) {
            tracking_drag_from_max_ = false;

            QWidget* win = window();
            const QRect max_rect = win->geometry();
            const QRect normal_rect = win->normalGeometry();

            win->showNormal();

            // Reposition so the cursor stays at roughly the same relative x in
            // the titlebar, matching the native Windows restore-on-drag behavior.
            if (normal_rect.isValid() && max_rect.width() > 0) {
                const qreal rel_x = static_cast<qreal>(current.x() - max_rect.left()) / max_rect.width();
                const int target_x = current.x() - qRound(win->width() * rel_x);
                win->move(qMax(0, target_x), qMax(0, current.y() - kHeight / 2));
            }

            if (QWindow* handle = win->windowHandle()) {
                handle->startSystemMove();
                event->accept();
                return;
            }
        }
    }
    QWidget::mouseMoveEvent(event);
}

void OperationalTitleBar::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        tracking_drag_from_max_ = false;
        resetDragCursor(); // handles plain click (no drag) — WM_EXITSIZEMOVE handles drag end
    }
    QWidget::mouseReleaseEvent(event);
}

void OperationalTitleBar::resetDragCursor() {
    if (move_cursor_active_) {
        QApplication::restoreOverrideCursor();
        move_cursor_active_ = false;
    }
}

void OperationalTitleBar::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        const QPoint local = mapFromGlobal(event->globalPosition().toPoint());
        if (hitTestWindowButton(local) == WindowButtonHit::None) {
            QWidget* win = window();
            win->isMaximized() ? win->showNormal() : win->showMaximized();
            event->accept();
            return;
        }
    }
    QWidget::mouseDoubleClickEvent(event);
}

bool OperationalTitleBar::isInDragArea(const QPoint& local_pos) const {
    if (!rect().contains(local_pos))
        return false;
    return hitTestWindowButton(local_pos) == WindowButtonHit::None;
}

OperationalTitleBar::WindowButtonHit OperationalTitleBar::hitTestWindowButton(const QPoint& local_pos) const {
    if (close_btn_->rect().contains(close_btn_->mapFrom(this, local_pos)))
        return WindowButtonHit::Close;
    if (maximize_btn_->rect().contains(maximize_btn_->mapFrom(this, local_pos)))
        return WindowButtonHit::MaximizeRestore;
    if (minimize_btn_->rect().contains(minimize_btn_->mapFrom(this, local_pos)))
        return WindowButtonHit::Minimize;
    return WindowButtonHit::None;
}

QRect OperationalTitleBar::maximizeButtonRectInWindow() const {
    if (!window())
        return {};
    return QRect(maximize_btn_->mapTo(window(), QPoint(0, 0)), maximize_btn_->size());
}

void OperationalTitleBar::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);

    QPainter painter(this);
    painter.fillRect(rect(), QColor(theme::ExoSnapPalette::kBg0));
    // Neutral hairline separator — recording state is communicated via the status pill
    // and preview border, not the top-chrome border line.
    painter.setPen(QPen(QColor(255, 255, 255, 18), 1.0));
    painter.drawLine(0, height() - 1, width(), height() - 1);
}

void OperationalTitleBar::refreshStatusChip() {
    const QString status = status_label_.trimmed().toUpper();
    if (status.contains(QStringLiteral("REC"))) {
        status_pill_->setTone(ui::widgets::StatusPill::Tone::Recording);
        status_pill_->setDotVisible(true);
        // DF-11: show dropped frame count inline when any frames have been dropped.
        const QString rec_text = recording_drop_count_ > 0
                                     ? QStringLiteral("Recording · %1↓").arg(recording_drop_count_)
                                     : QStringLiteral("Recording");
        status_pill_->setText(rec_text);
    } else if (status.contains(QStringLiteral("PAUSED"))) {
        status_pill_->setTone(ui::widgets::StatusPill::Tone::Warn);
        status_pill_->setDotVisible(true);
        status_pill_->setText(QStringLiteral("Paused"));
    } else if (status.contains(QStringLiteral("STOPPING"))) {
        status_pill_->setTone(ui::widgets::StatusPill::Tone::Warn);
        status_pill_->setDotVisible(true);
        status_pill_->setText(QStringLiteral("Stopping"));
    } else if (status.contains(QStringLiteral("STARTING"))) {
        // DF-15: azure Info tone for pre-recording states — visually distinct from amber Warn (Paused).
        status_pill_->setTone(ui::widgets::StatusPill::Tone::Info);
        status_pill_->setDotVisible(true);
        status_pill_->setText(QStringLiteral("Starting"));
    } else if (status.contains(QStringLiteral("COUNTDOWN"))) {
        // DF-15: azure Info tone for Countdown — distinct from amber Paused/Warn.
        status_pill_->setTone(ui::widgets::StatusPill::Tone::Info);
        status_pill_->setDotVisible(true);
        status_pill_->setText(QStringLiteral("Countdown"));
    } else if (status.contains(QStringLiteral("CHECK"))) {
        status_pill_->setTone(ui::widgets::StatusPill::Tone::Warn);
        status_pill_->setDotVisible(true);
        status_pill_->setText(QStringLiteral("Checking"));
    } else if (status.contains(QStringLiteral("ERROR"))) {
        status_pill_->setTone(ui::widgets::StatusPill::Tone::Blocked);
        status_pill_->setDotVisible(true);
        status_pill_->setText(QStringLiteral("Error"));
    } else if (status.contains(QStringLiteral("BLOCK"))) {
        status_pill_->setTone(ui::widgets::StatusPill::Tone::Blocked);
        status_pill_->setDotVisible(true);
        status_pill_->setText(QStringLiteral("Blocked"));
    } else if (status.contains(QStringLiteral("SAVED"))) {
        // Completed recording — same green tone as Ready, distinct "Saved" label,
        // shown while the Record result dock is visible.
        status_pill_->setTone(ui::widgets::StatusPill::Tone::Ready);
        status_pill_->setDotVisible(true);
        status_pill_->setText(QStringLiteral("Saved"));
    } else {
        status_pill_->setTone(ui::widgets::StatusPill::Tone::Ready);
        status_pill_->setDotVisible(true);
        status_pill_->setText(QStringLiteral("Ready"));
    }
    status_pill_->setVisible(true);
}

} // namespace exosnap::ui::chrome
