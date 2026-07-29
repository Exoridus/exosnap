#include "UpdaterWindow.h"

#include <QCloseEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLayout>
#include <QLayoutItem>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPixmap>
#include <QPointF>
#include <QPolygonF>
#include <QPushButton>
#include <QRectF>
#include <QResource>
#include <QVBoxLayout>
#include <QWindow>

#include "ProgressRing.h"
#include "StepListWidget.h"
#include "UpdaterTheme.h"

namespace {
void EnsureUpdaterResources() {
    static const bool initialized = [] {
        Q_INIT_RESOURCE(updater_fonts);
        return true;
    }();
    Q_UNUSED(initialized);
}
} // namespace

namespace {

using namespace updater_theme;

constexpr int kWindowWidth = 520;
constexpr int kWindowHeight = 680;
constexpr int kTitleBarHeight = 56;
constexpr int kWindowButtonWidth = 46;
constexpr int kActionHeight = 36;
constexpr int kStatePanelHeight = 110;

// ── Small painted icon (status line + footer marks) ──────────────────────────
enum class Ico { None, Check, Cross, Warning, ShieldCheck, Dot, Spinner, Chevron, Download, Layers };

class GlyphWidget : public QWidget {
  public:
    explicit GlyphWidget(int size, QWidget* parent = nullptr) : QWidget(parent) {
        setFixedSize(size, size);
    }
    void set(Ico ico, const QColor& color) {
        ico_ = ico;
        color_ = color;
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRectF r(0, 0, width(), height());
        switch (ico_) {
        case Ico::Check:
            paintCheck(p, r, color_, 2.0);
            break;
        case Ico::Cross:
            paintCross(p, r, color_, 2.0);
            break;
        case Ico::Warning:
            paintWarning(p, r, color_, 2.0);
            break;
        case Ico::ShieldCheck:
            paintShieldCheck(p, r.adjusted(1, 0, -1, 0), color_, 1.6);
            break;
        case Ico::Dot:
            paintDot(p, r, color_, width() * 0.5);
            break;
        case Ico::Spinner:
            paintSpinner(p, r.adjusted(2, 2, -2, -2), line2(), mint(), 2.0);
            break;
        case Ico::Chevron: {
            QPen pen(color_, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            const double x = r.left() + r.width() * 0.40;
            p.drawLine(QPointF(x, r.top() + r.height() * 0.28), QPointF(x + r.width() * 0.24, r.center().y()));
            p.drawLine(QPointF(x + r.width() * 0.24, r.center().y()), QPointF(x, r.bottom() - r.height() * 0.28));
            break;
        }
        case Ico::Download: {
            QPen pen(color_, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            const QPointF center = r.center();
            p.drawLine(QPointF(center.x(), r.top() + 1.5), QPointF(center.x(), r.bottom() - 5.0));
            p.drawLine(QPointF(center.x() - 3.0, center.y()), QPointF(center.x(), center.y() + 3.0));
            p.drawLine(QPointF(center.x() + 3.0, center.y()), QPointF(center.x(), center.y() + 3.0));
            p.drawLine(QPointF(r.left() + 2.0, r.bottom() - 1.5), QPointF(r.right() - 2.0, r.bottom() - 1.5));
            break;
        }
        case Ico::Layers: {
            QPen pen(color_, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            const QPointF center = r.center();
            const QPointF top(center.x(), r.top() + 2.0);
            const QPointF right(r.right() - 1.5, center.y() - 1.0);
            const QPointF bottom(center.x(), center.y() + 3.0);
            const QPointF left(r.left() + 1.5, center.y() - 1.0);
            p.drawPolygon(QPolygonF{top, right, bottom, left});
            p.drawLine(QPointF(left.x(), center.y() + 3.0), QPointF(center.x(), r.bottom() - 1.5));
            p.drawLine(QPointF(center.x(), r.bottom() - 1.5), QPointF(right.x(), center.y() + 3.0));
            break;
        }
        case Ico::None:
            break;
        }
    }

  private:
    Ico ico_ = Ico::None;
    QColor color_;
};

class TitleBarWidget : public QWidget {
  public:
    using QWidget::QWidget;

  protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            if (QWindow* handle = window()->windowHandle()) {
                handle->startSystemMove();
                event->accept();
                return;
            }
        }
        QWidget::mousePressEvent(event);
    }
};

// ── Brand mark (concentric-circle exosnap logo) ─────────────────────────────
class MarkWidget : public QWidget {
  public:
    explicit MarkWidget(int size, QWidget* parent = nullptr) : QWidget(parent) {
        setFixedSize(size, size);
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const double s = width() / 32.0;
        const QPointF c(width() / 2.0, height() / 2.0);
        QColor ring = mint();
        ring.setAlphaF(0.45f);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(ring, 1.5 * s));
        p.drawEllipse(c, 14.5 * s, 14.5 * s);
        p.setPen(QPen(mint(), 1.6 * s));
        p.drawEllipse(c, 6.2 * s, 6.2 * s);
        p.setPen(Qt::NoPen);
        p.setBrush(mint());
        p.drawEllipse(c, 2.4 * s, 2.4 * s);
    }
};

QString pillStyle(const QColor& fg, const QColor& bgc, const QColor& border) {
    return QStringLiteral("QLabel{color:%1;background:rgba(%2,%3,%4,%5);"
                          "border:1px solid rgba(%6,%7,%8,%9);border-radius:8px;padding:6px 12px;}")
        .arg(fg.name())
        .arg(bgc.red())
        .arg(bgc.green())
        .arg(bgc.blue())
        .arg(bgc.alphaF())
        .arg(border.red())
        .arg(border.green())
        .arg(border.blue())
        .arg(border.alphaF());
}

QString rgba(const QColor& c) {
    return QStringLiteral("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alphaF());
}

QIcon refreshIcon(const QColor& color, qreal dpr) {
    const int logicalSide = 16;
    QPixmap px(qRound(logicalSide * dpr), qRound(logicalSide * dpr));
    px.setDevicePixelRatio(dpr);
    px.fill(Qt::transparent);
    QPainter painter(&px);
    painter.setRenderHint(QPainter::Antialiasing, true);
    paintRefresh(painter, QRectF(0.5, 0.5, logicalSide - 1.0, logicalSide - 1.0), color, 1.6);
    return QIcon(px);
}

enum class ButtonVariant { Primary, Secondary, Destructive };

QPushButton* makeButton(const QString& text, ButtonVariant variant, QWidget* parent) {
    auto* b = new QPushButton(text, parent);
    b->setAccessibleName(text);
    b->setFont(ui(13, QFont::DemiBold));
    b->setCursor(Qt::PointingHandCursor);
    b->setFixedHeight(kActionHeight);
    b->setMinimumWidth(76);
    if (variant == ButtonVariant::Primary) {
        b->setStyleSheet(QStringLiteral("QPushButton{color:%1;background:%2;border:1px solid %2;"
                                        "border-radius:10px;padding:0 16px;}"
                                        "QPushButton:hover:enabled{background:%3;}"
                                        "QPushButton:pressed:enabled{background:%4;}"
                                        "QPushButton:disabled{color:%5;background:%6;border-color:%6;}")
                             .arg(mintInk().name(), mint().name(), mint().lighter(108).name(),
                                  mint().darker(108).name(), dim().name(), rgba(line())));
    } else if (variant == ButtonVariant::Destructive) {
        b->setStyleSheet(QStringLiteral("QPushButton{color:%1;background:%2;border:1px solid %3;"
                                        "border-radius:10px;padding:0 16px;}"
                                        "QPushButton:hover{background:%4;}")
                             .arg(error().name(), rgba(statusDim(error())), rgba(statusBorder(error())),
                                  rgba(withAlpha(error(), 0.20))));
    } else {
        b->setStyleSheet(
            QStringLiteral("QPushButton{color:%1;background:transparent;border:1px solid %2;"
                           "border-radius:10px;padding:0 16px;}"
                           "QPushButton:hover:enabled{color:%3;border-color:%3;}"
                           "QPushButton:pressed:enabled{color:%3;background:%4;border-color:%3;}"
                           "QPushButton:disabled{color:%5;background:transparent;border-color:%6;}")
                .arg(mut().name(), rgba(line2()), ink().name(), rgba(surf2()), dim().name(), rgba(line())));
    }
    if (text == QStringLiteral("Retry") || text == QStringLiteral("Re-download")) {
        b->setObjectName(QStringLiteral("updaterRetryButton"));
        b->setIcon(refreshIcon(variant == ButtonVariant::Primary ? mintInk() : mut(), parent->devicePixelRatioF()));
        b->setIconSize(QSize(16, 16));
    }
    return b;
}

} // namespace

// ── UpdaterWindow ────────────────────────────────────────────────────────────
UpdaterWindow::UpdaterWindow(QWidget* parent) : QWidget(parent) {
    EnsureUpdaterResources();
    ensureFontsLoaded();
    setWindowTitle(QStringLiteral("ExoSnap Updater"));
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setFixedSize(kWindowWidth, kWindowHeight);
    setAutoFillBackground(true);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->setSizeConstraint(QLayout::SetDefaultConstraint);

    // ── Title bar ────────────────────────────────────────────────────────────
    auto* titleBar = new TitleBarWidget(this);
    titleBar->setObjectName(QStringLiteral("updaterTitleBar"));
    titleBar->setFixedHeight(kTitleBarHeight);
    titleBar->setFixedWidth(kWindowWidth);
    titleBar->setStyleSheet(QStringLiteral("border-bottom:1px solid %1;").arg(rgba(line())));
    auto* tbLayout = new QHBoxLayout(titleBar);
    tbLayout->setContentsMargins(0, 0, 0, 0);
    tbLayout->setSpacing(0);

    // Match OperationalTitleBar's brand slot exactly: 16 px leading inset,
    // 20 px mark, 8 px mark/wordmark gap and 8 px trailing inset.
    auto* brandSlot = new QWidget(titleBar);
    brandSlot->setObjectName(QStringLiteral("updaterBrandSlot"));
    brandSlot->setStyleSheet(QStringLiteral("background:transparent;border:none;"));
    auto* brandLayout = new QHBoxLayout(brandSlot);
    brandLayout->setContentsMargins(16, 0, 8, 0);
    brandLayout->setSpacing(8);
    brandLayout->addWidget(new MarkWidget(20, brandSlot), 0, Qt::AlignVCenter);

    auto* wordmark = new QLabel(brandSlot);
    wordmark->setObjectName(QStringLiteral("updaterWordmark"));
    wordmark->setTextFormat(Qt::RichText);
    wordmark->setFont(ui(15, QFont::DemiBold));
    wordmark->setText(QStringLiteral("<span style=\"color:%1;\">exo</span><span style=\"color:%2;\">snap</span>")
                          .arg(ink().name(), mint().name()));
    wordmark->setStyleSheet(QStringLiteral("background:transparent;border:none;"));
    brandLayout->addWidget(wordmark, 0, Qt::AlignVCenter);
    tbLayout->addWidget(brandSlot);

    // The role label starts where the main title bar's first navigation item
    // starts (14 px inset after the brand slot), but remains neutral and stable.
    auto* roleSlot = new QWidget(titleBar);
    roleSlot->setObjectName(QStringLiteral("updaterRoleSlot"));
    roleSlot->setStyleSheet(QStringLiteral("background:transparent;border:none;"));
    auto* roleLayout = new QHBoxLayout(roleSlot);
    roleLayout->setContentsMargins(14, 0, 0, 0);
    roleLayout->setSpacing(0);
    auto* title = new QLabel(QStringLiteral("Updater"), roleSlot);
    title->setObjectName(QStringLiteral("updaterTitle"));
    title->setFont(ui(14, QFont::Medium));
    title->setStyleSheet(QStringLiteral("color:%1;background:transparent;border:none;").arg(mut().name()));
    roleLayout->addWidget(title, 0, Qt::AlignVCenter);
    tbLayout->addWidget(roleSlot);
    tbLayout->addStretch(1);

    minimize_button_ = new QPushButton(QStringLiteral("−"), titleBar);
    minimize_button_->setObjectName(QStringLiteral("updaterMinimizeButton"));
    minimize_button_->setFixedSize(kWindowButtonWidth, kTitleBarHeight);
    minimize_button_->setFocusPolicy(Qt::NoFocus);
    minimize_button_->setFont(mono(13, QFont::Medium));
    minimize_button_->setCursor(Qt::PointingHandCursor);
    minimize_button_->setToolTip(QStringLiteral("Minimize"));
    minimize_button_->setAccessibleName(QStringLiteral("Minimize updater"));
    minimize_button_->setStyleSheet(
        QStringLiteral("QPushButton{color:%1;background:transparent;border:none;border-radius:0;}"
                       "QPushButton:hover{color:%2;background:%3;}"
                       "QPushButton:pressed{color:%2;background:%4;}")
            .arg(mut().name(), ink().name(), rgba(surf2()), rgba(line2())));
    connect(minimize_button_, &QPushButton::clicked, this, &QWidget::showMinimized);
    tbLayout->addWidget(minimize_button_);

    close_button_ = new QPushButton(QStringLiteral("×"), titleBar);
    close_button_->setObjectName(QStringLiteral("updaterCloseButton"));
    close_button_->setFixedSize(kWindowButtonWidth, kTitleBarHeight);
    close_button_->setFocusPolicy(Qt::NoFocus);
    close_button_->setFont(mono(13, QFont::Medium));
    close_button_->setCursor(Qt::PointingHandCursor);
    close_button_->setToolTip(QStringLiteral("Close"));
    close_button_->setAccessibleName(QStringLiteral("Close updater"));
    close_button_->setStyleSheet(
        QStringLiteral("QPushButton{color:%1;background:transparent;border:none;border-radius:0;}"
                       "QPushButton:hover:enabled{color:#ffffff;background:#b8261d;}"
                       "QPushButton:pressed:enabled{color:#ffffff;background:#971e17;}"
                       "QPushButton:disabled{color:%2;background:transparent;}")
            .arg(mut().name(), rgba(line2())));
    connect(close_button_, &QPushButton::clicked, this, &UpdaterWindow::requestClose);
    tbLayout->addWidget(close_button_);

    root->addWidget(titleBar);

    // ── Content ───────────────────────────────────────────────────────────────
    auto* content = new QWidget(this);
    content->setFixedWidth(kWindowWidth);
    auto* col = new QVBoxLayout(content);
    col->setContentsMargins(24, 18, 24, 18);
    col->setSpacing(0);

    // Version transition
    auto* versionBlock = new QWidget(content);
    versionBlock->setFixedHeight(48);
    auto* vb = new QVBoxLayout(versionBlock);
    vb->setContentsMargins(0, 0, 0, 0);
    vb->setSpacing(9);

    auto* caption = new QLabel(QStringLiteral("UPDATING EXOSNAP"), versionBlock);
    caption->setFont(mono(10, QFont::Medium));
    caption->setStyleSheet(QStringLiteral("QLabel{color:%1;letter-spacing:1px;}").arg(dim().name()));
    caption->setAlignment(Qt::AlignHCenter);
    vb->addWidget(caption, 0, Qt::AlignHCenter);

    auto* pillRow = new QWidget(versionBlock);
    auto* pr = new QHBoxLayout(pillRow);
    pr->setContentsMargins(0, 0, 0, 0);
    pr->setSpacing(12);

    from_pill_ = new QLabel(pillRow);
    from_pill_->setFont(mono(14, QFont::Medium));
    from_pill_->setStyleSheet(pillStyle(mut(), surf2(), line()));
    pr->addWidget(from_pill_);

    auto* chevron = new GlyphWidget(16, pillRow);
    chevron->set(Ico::Chevron, mint());
    pr->addWidget(chevron);

    to_pill_ = new QLabel(pillRow);
    to_pill_->setFont(mono(14, QFont::DemiBold));
    to_pill_->setStyleSheet(pillStyle(mint(), accentDim(), accentBorder()));
    pr->addWidget(to_pill_);

    vb->addWidget(pillRow, 0, Qt::AlignHCenter);
    col->addWidget(versionBlock, 0, Qt::AlignHCenter);
    col->addSpacing(12);

    // Ring
    ring_ = new ProgressRing(content);
    col->addWidget(ring_, 0, Qt::AlignHCenter);
    col->addSpacing(8);

    // Status line
    status_row_ = new QWidget(content);
    status_row_->setFixedHeight(20);
    auto* sr = new QHBoxLayout(status_row_);
    sr->setContentsMargins(0, 0, 0, 0);
    sr->setSpacing(9);
    auto* statusIcon = new GlyphWidget(16, status_row_);
    status_icon_ = statusIcon;
    sr->addWidget(statusIcon, 0, Qt::AlignVCenter);
    status_text_ = new QLabel(status_row_);
    sr->addWidget(status_text_, 0, Qt::AlignVCenter);
    col->addWidget(status_row_, 0, Qt::AlignHCenter);
    col->addSpacing(14);

    // Step list
    steps_ = new StepListWidget(content);
    steps_->setFixedHeight(196);
    col->addWidget(steps_);
    col->addSpacing(14);

    // Fixed state panel + fixed action row. buildFooter() changes only their
    // contents, never their geometry.
    footer_container_ = new QWidget(content);
    footer_container_->setFixedHeight(kStatePanelHeight + 10 + kActionHeight);
    footer_layout_ = new QVBoxLayout(footer_container_);
    footer_layout_->setContentsMargins(0, 0, 0, 0);
    footer_layout_->setSpacing(10);
    col->addWidget(footer_container_);

    root->addWidget(content);

    // In-window confirmation keeps cancellation visually inside the updater
    // instead of introducing a second, differently styled native dialog.
    cancel_overlay_ = new QWidget(this);
    cancel_overlay_->setObjectName(QStringLiteral("updaterCancelOverlay"));
    cancel_overlay_->setGeometry(rect());
    cancel_overlay_->setStyleSheet(QStringLiteral("QWidget#updaterCancelOverlay{background:rgba(5,5,7,0.88);}"));
    auto* overlayLayout = new QVBoxLayout(cancel_overlay_);
    overlayLayout->setContentsMargins(32, 32, 32, 32);
    overlayLayout->addStretch(1);

    auto* dialog = new QWidget(cancel_overlay_);
    dialog->setObjectName(QStringLiteral("updaterCancelDialog"));
    dialog->setFixedWidth(390);
    dialog->setStyleSheet(QStringLiteral("QWidget#updaterCancelDialog{background:%1;border:1px solid %2;"
                                         "border-radius:14px;}")
                              .arg(rgba(surf()), rgba(line2())));
    auto* dialogLayout = new QVBoxLayout(dialog);
    dialogLayout->setContentsMargins(20, 18, 20, 18);
    dialogLayout->setSpacing(9);

    auto* dialogTitle = new QLabel(QStringLiteral("Cancel this update?"), dialog);
    dialogTitle->setObjectName(QStringLiteral("updaterCancelTitle"));
    dialogTitle->setFont(ui(16, QFont::DemiBold));
    dialogTitle->setStyleSheet(QStringLiteral("color:%1;background:transparent;border:none;").arg(ink().name()));
    dialogLayout->addWidget(dialogTitle);

    auto* dialogBody = new QLabel(QStringLiteral("Download and preparation progress will be discarded. "
                                                 "Your installed ExoSnap version remains unchanged."),
                                  dialog);
    dialogBody->setObjectName(QStringLiteral("updaterCancelBody"));
    dialogBody->setWordWrap(true);
    dialogBody->setFont(ui(13, QFont::Normal));
    dialogBody->setStyleSheet(QStringLiteral("color:%1;background:transparent;border:none;").arg(mut().name()));
    dialogLayout->addWidget(dialogBody);

    auto* dialogActions = new QHBoxLayout();
    dialogActions->setContentsMargins(0, 9, 0, 0);
    dialogActions->setSpacing(8);
    dialogActions->addStretch(1);
    auto* confirmCancel = makeButton(QStringLiteral("Cancel update"), ButtonVariant::Destructive, dialog);
    confirmCancel->setObjectName(QStringLiteral("updaterConfirmCancelButton"));
    auto* keepUpdating = makeButton(QStringLiteral("Keep updating"), ButtonVariant::Primary, dialog);
    keepUpdating->setObjectName(QStringLiteral("updaterKeepUpdatingButton"));
    connect(confirmCancel, &QPushButton::clicked, this, &UpdaterWindow::confirmCancelAndClose);
    connect(keepUpdating, &QPushButton::clicked, cancel_overlay_, &QWidget::hide);
    dialogActions->addWidget(confirmCancel);
    dialogActions->addWidget(keepUpdating);
    dialogLayout->addLayout(dialogActions);

    overlayLayout->addWidget(dialog, 0, Qt::AlignHCenter);
    overlayLayout->addStretch(1);
    cancel_overlay_->hide();
}

std::array<QString, 5> UpdaterWindow::stepLabels() {
    return StepListWidget::labels();
}

bool UpdaterWindow::closeEnabled() const {
    return close_button_ != nullptr && close_button_->isEnabled();
}

bool UpdaterWindow::cancelConfirmationVisible() const {
    return cancel_overlay_ != nullptr && cancel_overlay_->isVisible();
}

QStringList UpdaterWindow::footerButtonLabels() const {
    QStringList out;
    for (auto* b : footer_buttons_)
        out << b->text();
    return out;
}

void UpdaterWindow::render(const UpdaterUiState& state) {
    // Ring
    ring_->setValue(state.ring);
    ring_->setVariant(state.variant);

    // Version pills
    from_pill_->setText(state.from_version);
    to_pill_->setText(state.to_version);

    // Fail-row tint follows the terminal variant.
    QColor failColor = caution();
    if (state.variant == TerminalVariant::Red)
        failColor = error();
    else if (state.variant == TerminalVariant::Green || state.variant == TerminalVariant::RebootRequired)
        failColor = success();
    steps_->setFailColor(failColor);
    // Green is a soft-success terminal: a Failed step there is the auto-relaunch
    // that didn't happen, not a real error, so the row reads as "manual" rather
    // than "failed" (display-only -- the controller's StepStatus is untouched).
    steps_->setSteps(state.steps, state.variant == TerminalVariant::Green);

    // The central status row keeps the same footprint in every state. Terminal
    // states use a short summary here; the result card below owns the actionable
    // detail and safety truth.
    status_row_->setVisible(true);

    // Status line
    auto* icon = static_cast<GlyphWidget*>(status_icon_);
    if (state.variant == TerminalVariant::None) {
        icon->set(Ico::Spinner, mint());
        status_text_->setFont(mono(13, QFont::Medium));
        status_text_->setStyleSheet(QStringLiteral("color:%1;").arg(ink().name()));
        status_text_->setText(state.status_line);
    } else {
        Ico ico = Ico::Check;
        QColor toneCol = success();
        QString headline;
        switch (state.variant) {
        case TerminalVariant::Amber:
            ico = Ico::Warning;
            toneCol = caution();
            headline = QStringLiteral("Update didn't complete");
            break;
        case TerminalVariant::Red:
            ico = Ico::Cross;
            toneCol = error();
            headline = QStringLiteral("Update failed");
            break;
        case TerminalVariant::Green:
            ico = Ico::Check;
            toneCol = success();
            headline = QStringLiteral("Update complete — version %1 is ready").arg(state.to_version);
            break;
        case TerminalVariant::RebootRequired:
            ico = Ico::Check;
            toneCol = success();
            headline = QStringLiteral("Update installed — restart Windows to finish");
            break;
        case TerminalVariant::Success:
            ico = Ico::Check;
            toneCol = mint();
            headline = QStringLiteral("Update complete - version %1 is ready").arg(state.to_version);
            break;
        case TerminalVariant::None:
            break;
        }
        icon->set(ico, toneCol);
        status_text_->setFont(ui(14, QFont::DemiBold));
        status_text_->setStyleSheet(QStringLiteral("color:%1;").arg(ink().name()));
        status_text_->setText(headline);
    }

    // Footer
    buildFooter(state);

    // Close X: refuse to interrupt an install, verify, or launch in flight.
    // Launch includes the health-check and a possible restore, so aborting there
    // is as dangerous as aborting Install/Verify (stranded-install risk).
    const bool block = state.steps[size_t(UpStep::Install)] == StepStatus::Working ||
                       state.steps[size_t(UpStep::Verify)] == StepStatus::Working ||
                       state.steps[size_t(UpStep::Launch)] == StepStatus::Working;
    close_button_->setEnabled(!block);
    const bool terminal = state.variant != TerminalVariant::None;
    close_button_->setToolTip(block      ? QStringLiteral("Please wait - updating")
                              : terminal ? QStringLiteral("Close")
                                         : QStringLiteral("Cancel update and close"));
    close_button_->setAccessibleName(block      ? QStringLiteral("Close unavailable while updating")
                                     : terminal ? QStringLiteral("Close updater")
                                                : QStringLiteral("Cancel update and close"));
    close_blocked_ = block;
    cancel_confirmation_required_ = !terminal && !block;
    if (!cancel_confirmation_required_ && cancel_overlay_ != nullptr)
        cancel_overlay_->hide();
}

void UpdaterWindow::buildFooter(const UpdaterUiState& state) {
    footer_buttons_.clear();
    while (QLayoutItem* item = footer_layout_->takeAt(0)) {
        if (QWidget* w = item->widget())
            w->deleteLater();
        delete item;
    }
    const bool resultTerminal = state.variant != TerminalVariant::None && state.variant != TerminalVariant::Success;
    const bool successDone = state.variant == TerminalVariant::Success;
    const bool critical = state.steps[size_t(UpStep::Install)] == StepStatus::Working ||
                          state.steps[size_t(UpStep::Verify)] == StepStatus::Working ||
                          state.steps[size_t(UpStep::Launch)] == StepStatus::Working;

    auto* card = new QWidget(footer_container_);
    card->setFixedHeight(kStatePanelHeight);

    if (resultTerminal) {
        QColor tone = caution();
        if (state.variant == TerminalVariant::Red)
            tone = error();
        else if (state.variant == TerminalVariant::Green || state.variant == TerminalVariant::RebootRequired)
            tone = success();

        card->setObjectName(QStringLiteral("updaterResultCard"));
        card->setStyleSheet(QStringLiteral("QWidget#updaterResultCard{background:%1;border:1px solid %2;"
                                           "border-radius:12px;}")
                                .arg(rgba(statusDim(tone)), rgba(statusBorder(tone))));
        auto* box = new QVBoxLayout(card);
        box->setContentsMargins(15, 12, 15, 12);
        box->setSpacing(7);

        auto* header = new QWidget(card);
        header->setStyleSheet(QStringLiteral("background:transparent;border:none;"));
        auto* hr = new QHBoxLayout(header);
        hr->setContentsMargins(0, 0, 0, 0);
        hr->setSpacing(9);
        auto* resultIcon = new GlyphWidget(16, header);
        resultIcon->set(state.variant == TerminalVariant::Amber ? Ico::Warning
                        : state.variant == TerminalVariant::Red ? Ico::Cross
                                                                : Ico::Check,
                        tone);
        resultIcon->setStyleSheet(QStringLiteral("background:transparent;border:none;"));
        hr->addWidget(resultIcon, 0, Qt::AlignVCenter);
        auto* headline = new QLabel(state.headline, header);
        headline->setObjectName(QStringLiteral("updaterResultHeadline"));
        headline->setWordWrap(false);
        headline->setFont(ui(13, QFont::DemiBold));
        headline->setStyleSheet(QStringLiteral("color:%1;background:transparent;border:none;").arg(ink().name()));
        hr->addWidget(headline, 1);
        box->addWidget(header);

        auto* detail = new QLabel(state.detail_text, card);
        detail->setObjectName(QStringLiteral("updaterResultDetail"));
        detail->setWordWrap(false);
        detail->setFont(ui(12, QFont::Normal));
        detail->setStyleSheet(QStringLiteral("color:%1;background:transparent;border:none;").arg(mut().name()));
        box->addWidget(detail);

        auto* safety = new QWidget(card);
        safety->setStyleSheet(QStringLiteral("background:transparent;border:none;"));
        auto* safeRow = new QHBoxLayout(safety);
        safeRow->setContentsMargins(0, 0, 0, 0);
        safeRow->setSpacing(8);
        auto* shield = new GlyphWidget(14, safety);
        shield->set(Ico::ShieldCheck, success());
        shield->setStyleSheet(QStringLiteral("background:transparent;border:none;"));
        safeRow->addWidget(shield, 0, Qt::AlignVCenter);
        auto* safeText = new QLabel(state.safety_text, safety);
        safeText->setObjectName(QStringLiteral("updaterSafetyText"));
        safeText->setWordWrap(false);
        safeText->setFont(ui(12, QFont::Medium));
        safeText->setStyleSheet(QStringLiteral("color:%1;background:transparent;border:none;").arg(ink().name()));
        safeRow->addWidget(safeText, 1);
        box->addWidget(safety);
    } else {
        UpStep active = UpStep::Count;
        for (int i = 0; i < int(UpStep::Count); ++i) {
            if (state.steps[size_t(i)] == StepStatus::Working) {
                active = static_cast<UpStep>(i);
                break;
            }
        }

        QString title;
        QString detail;
        QString safety;
        Ico titleIcon = Ico::Spinner;
        QColor titleTone = mint();
        QColor panelBackground = surf2();
        QColor panelBorder = line();
        if (successDone) {
            title = QStringLiteral("Version %1 is ready").arg(state.to_version);
            detail = QStringLiteral("The update is installed and verified.");
            safety = QStringLiteral("ExoSnap is starting automatically.");
            titleIcon = Ico::Check;
            titleTone = success();
            panelBackground = statusDim(success());
            panelBorder = statusBorder(success());
        } else {
            switch (active) {
            case UpStep::Download:
                title = state.verification_reinstall ? QStringLiteral("Downloading update again")
                                                     : QStringLiteral("Downloading update");
                detail = QStringLiteral("Fetching and checking the signed update.");
                safety = QStringLiteral("Your installed version remains unchanged until installation begins.");
                titleIcon = Ico::Download;
                break;
            case UpStep::CloseApp:
                title = QStringLiteral("Closing previous version");
                detail = QStringLiteral("Waiting for ExoSnap to close.");
                safety = QStringLiteral("The verified package is ready before ExoSnap closes.");
                titleIcon = Ico::Cross;
                break;
            case UpStep::Install:
                title = state.verification_reinstall ? QStringLiteral("Reinstalling ExoSnap")
                                                     : QStringLiteral("Installing new files");
                detail = QStringLiteral("Replacing the application files.");
                safety = QStringLiteral("Keep your computer on while the application files are replaced.");
                titleIcon = Ico::Layers;
                break;
            case UpStep::Verify:
                title = QStringLiteral("Verifying installation");
                detail = QStringLiteral("Checking the installed files.");
                safety = QStringLiteral("The installed files are checked against the signed release.");
                titleIcon = Ico::ShieldCheck;
                break;
            case UpStep::Launch:
                title = QStringLiteral("Launching ExoSnap");
                detail = QStringLiteral("Starting the updated app.");
                safety = QStringLiteral("ExoSnap reopens automatically when the handoff completes.");
                titleIcon = Ico::Spinner;
                break;
            case UpStep::Count:
                title = QStringLiteral("Preparing update");
                detail = QStringLiteral("Getting the updater ready.");
                safety = QStringLiteral("Your installed version remains unchanged.");
                titleIcon = Ico::Spinner;
                break;
            }
        }

        card->setObjectName(QStringLiteral("updaterWorkingPanel"));
        card->setStyleSheet(QStringLiteral("QWidget#updaterWorkingPanel{background:%1;border:1px solid %2;"
                                           "border-radius:12px;}")
                                .arg(rgba(panelBackground), rgba(panelBorder)));
        auto* box = new QVBoxLayout(card);
        box->setContentsMargins(15, 12, 15, 12);
        box->setSpacing(7);

        auto* header = new QWidget(card);
        header->setStyleSheet(QStringLiteral("background:transparent;border:none;"));
        auto* hr = new QHBoxLayout(header);
        hr->setContentsMargins(0, 0, 0, 0);
        hr->setSpacing(9);
        auto* panelIcon = new GlyphWidget(16, header);
        panelIcon->set(titleIcon, titleTone);
        panelIcon->setStyleSheet(QStringLiteral("background:transparent;border:none;"));
        hr->addWidget(panelIcon, 0, Qt::AlignVCenter);
        auto* titleLabel = new QLabel(title, header);
        titleLabel->setObjectName(QStringLiteral("updaterWorkingTitle"));
        titleLabel->setFont(ui(13, QFont::DemiBold));
        titleLabel->setStyleSheet(QStringLiteral("color:%1;background:transparent;border:none;").arg(ink().name()));
        hr->addWidget(titleLabel, 1);
        box->addWidget(header);

        auto* detailLabel = new QLabel(detail, card);
        detailLabel->setObjectName(QStringLiteral("updaterWorkingDetail"));
        detailLabel->setFont(ui(12, QFont::Normal));
        detailLabel->setStyleSheet(QStringLiteral("color:%1;background:transparent;border:none;").arg(mut().name()));
        box->addWidget(detailLabel);

        auto* safetyRow = new QWidget(card);
        safetyRow->setStyleSheet(QStringLiteral("background:transparent;border:none;"));
        auto* sr = new QHBoxLayout(safetyRow);
        sr->setContentsMargins(0, 0, 0, 0);
        sr->setSpacing(8);
        auto* shield = new GlyphWidget(14, safetyRow);
        shield->set(Ico::ShieldCheck, success());
        shield->setStyleSheet(QStringLiteral("background:transparent;border:none;"));
        sr->addWidget(shield, 0, Qt::AlignVCenter);
        auto* safetyLabel = new QLabel(safety, safetyRow);
        safetyLabel->setObjectName(QStringLiteral("updaterWorkingSafety"));
        safetyLabel->setFont(ui(12, QFont::Medium));
        safetyLabel->setStyleSheet(QStringLiteral("color:%1;background:transparent;border:none;").arg(ink().name()));
        sr->addWidget(safetyLabel, 1);
        box->addWidget(safetyRow);
    }
    footer_layout_->addWidget(card);

    auto* actions = new QWidget(footer_container_);
    actions->setObjectName(QStringLiteral("updaterActionRow"));
    actions->setFixedHeight(kActionHeight);
    actions->setStyleSheet(QStringLiteral("background:transparent;border:none;"));
    auto* ar = new QHBoxLayout(actions);
    ar->setContentsMargins(0, 0, 0, 0);
    ar->setSpacing(8);
    auto* hint = new QLabel(actions);
    hint->setObjectName(QStringLiteral("updaterActionHint"));
    hint->setFont(ui(11, QFont::Normal));
    hint->setStyleSheet(QStringLiteral("color:%1;background:transparent;border:none;").arg(dim().name()));
    ar->addWidget(hint, 1);

    if (resultTerminal) {
        if (!state.primary_action.isEmpty()) {
            auto* button = makeButton(state.primary_action, ButtonVariant::Primary, actions);
            const QString action = state.primary_action;
            connect(button, &QPushButton::clicked, this, [this, action] { emitForAction(action); });
            footer_buttons_ << button;
            ar->addWidget(button);
        }
        if (!state.secondary_action.isEmpty()) {
            auto* button = makeButton(state.secondary_action, ButtonVariant::Secondary, actions);
            const QString action = state.secondary_action;
            connect(button, &QPushButton::clicked, this, [this, action] { emitForAction(action); });
            footer_buttons_ << button;
            ar->addWidget(button);
        }
    } else if (successDone) {
        hint->setText(QStringLiteral("ExoSnap is starting automatically."));
        auto* button = makeButton(QStringLiteral("Close"), ButtonVariant::Secondary, actions);
        connect(button, &QPushButton::clicked, this, &UpdaterWindow::requestClose);
        footer_buttons_ << button;
        ar->addWidget(button);
    } else if (!critical) {
        hint->setText(QStringLiteral("Cancelling discards this update run."));
        auto* button = makeButton(QStringLiteral("Cancel update"), ButtonVariant::Secondary, actions);
        button->setObjectName(QStringLiteral("updaterCancelButton"));
        connect(button, &QPushButton::clicked, this, &UpdaterWindow::requestClose);
        footer_buttons_ << button;
        ar->addWidget(button);
    } else {
        hint->setText(QStringLiteral("This phase cannot be interrupted."));
        auto* button = makeButton(QStringLiteral("Close"), ButtonVariant::Secondary, actions);
        button->setEnabled(false);
        footer_buttons_ << button;
        ar->addWidget(button);
    }
    footer_layout_->addWidget(actions);
}

void UpdaterWindow::requestClose() {
    if (close_blocked_)
        return;
    if (cancel_confirmation_required_) {
        showCancelConfirmation();
        return;
    }
    emit closeRequested();
}

void UpdaterWindow::showCancelConfirmation() {
    if (cancel_overlay_ == nullptr || close_blocked_ || !cancel_confirmation_required_)
        return;
    cancel_overlay_->setGeometry(rect());
    cancel_overlay_->show();
    cancel_overlay_->raise();
    if (auto* keep = cancel_overlay_->findChild<QPushButton*>(QStringLiteral("updaterKeepUpdatingButton")))
        keep->setFocus(Qt::OtherFocusReason);
}

void UpdaterWindow::confirmCancelAndClose() {
    cancel_confirmation_required_ = false;
    if (cancel_overlay_ != nullptr)
        cancel_overlay_->hide();
    emit closeRequested();
}

void UpdaterWindow::emitForAction(const QString& action) {
    if (action == QStringLiteral("Retry") || action == QStringLiteral("Re-download"))
        emit retryRequested();
    else if (action == QStringLiteral("Open ExoSnap"))
        emit openExoSnapRequested();
    else
        emit closeRequested();
}

void UpdaterWindow::closeEvent(QCloseEvent* event) {
    // The in-window close X is disabled during Install/Verify/Launch (see
    // render()), but that alone does not stop WM_CLOSE from reaching this
    // window through Alt+F4, a taskbar/system-menu close, or a Windows
    // logoff/shutdown -- Qt::FramelessWindowHint keeps the native frame's
    // WM_CLOSE handling active even without a titlebar. Ignoring it here is
    // what actually keeps a swap from being torn apart mid-rename; the button
    // being disabled is only the visible half of the guard.
    if (close_blocked_) {
        event->ignore();
        return;
    }
    if (cancel_confirmation_required_) {
        event->ignore();
        showCancelConfirmation();
        return;
    }
    event->accept();
}

void UpdaterWindow::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), bg());
    p.setPen(QPen(line2(), 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5));
}
