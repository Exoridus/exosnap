#include "UpdaterWindow.h"

#include <QCloseEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLayout>
#include <QLayoutItem>
#include <QPaintEvent>
#include <QPainter>
#include <QPixmap>
#include <QPointF>
#include <QPushButton>
#include <QRectF>
#include <QResource>
#include <QVBoxLayout>

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

constexpr int kWindowWidth = 640;
constexpr int kWindowHeight = 768;
constexpr int kActionHeight = 36;

// ── Small painted icon (status line + footer marks) ──────────────────────────
enum class Ico { None, Check, Cross, Warning, ShieldCheck, Dot, Spinner, Chevron };

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
        case Ico::None:
            break;
        }
    }

  private:
    Ico ico_ = Ico::None;
    QColor color_;
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

QIcon closeIcon(const QColor& color, qreal dpr) {
    constexpr int logicalSide = 20;
    QPixmap px(qRound(logicalSide * dpr), qRound(logicalSide * dpr));
    px.setDevicePixelRatio(dpr);
    px.fill(Qt::transparent);
    QPainter painter(&px);
    painter.setRenderHint(QPainter::Antialiasing, true);
    paintCross(painter, QRectF(3, 3, 14, 14), color, 1.6);
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
                                        "QPushButton:hover{background:%3;}")
                             .arg(mintInk().name(), mint().name(), mint().lighter(108).name()));
    } else if (variant == ButtonVariant::Destructive) {
        b->setStyleSheet(QStringLiteral("QPushButton{color:%1;background:%2;border:1px solid %3;"
                                        "border-radius:10px;padding:0 16px;}"
                                        "QPushButton:hover{background:%4;}")
                             .arg(error().name(), rgba(statusDim(error())), rgba(statusBorder(error())),
                                  rgba(withAlpha(error(), 0.20))));
    } else {
        b->setStyleSheet(QStringLiteral("QPushButton{color:%1;background:transparent;border:1px solid %2;"
                                        "border-radius:10px;padding:0 16px;}"
                                        "QPushButton:hover{color:%3;border-color:%3;}")
                             .arg(mut().name(), rgba(line2()), ink().name()));
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
    auto* titleBar = new QWidget(this);
    titleBar->setFixedHeight(62);
    titleBar->setFixedWidth(kWindowWidth);
    titleBar->setStyleSheet(QStringLiteral("border-bottom:1px solid %1;").arg(rgba(line())));
    auto* tbLayout = new QHBoxLayout(titleBar);
    tbLayout->setContentsMargins(16, 0, 10, 0);
    tbLayout->setSpacing(12);

    tbLayout->addWidget(new MarkWidget(20, titleBar));

    auto* titleStack = new QVBoxLayout();
    titleStack->setContentsMargins(0, 0, 0, 0);
    titleStack->setSpacing(1);

    auto* titleRow = new QHBoxLayout();
    titleRow->setContentsMargins(0, 0, 0, 0);
    titleRow->setSpacing(8);
    auto* title = new QLabel(QStringLiteral("Updater"), titleBar);
    title->setObjectName(QStringLiteral("updaterTitle"));
    title->setFont(ui(14, QFont::DemiBold));
    title->setStyleSheet(QStringLiteral("color:%1;background:transparent;border:none;").arg(ink().name()));
    titleRow->addWidget(title);

    title_status_ = new QLabel(QStringLiteral("Downloading"), titleBar);
    title_status_->setObjectName(QStringLiteral("updaterTitleStatus"));
    title_status_->setFont(mono(10, QFont::DemiBold));
    titleRow->addWidget(title_status_);

    verify_tag_ = new QLabel(QStringLiteral("VERIFY"), titleBar);
    verify_tag_->setObjectName(QStringLiteral("updaterVerifyTag"));
    verify_tag_->setAccessibleName(QStringLiteral("Verification reinstall"));
    verify_tag_->setFont(mono(8, QFont::DemiBold));
    verify_tag_->setStyleSheet(QStringLiteral("QLabel{color:%1;background:%2;border:1px solid %3;"
                                              "border-radius:5px;padding:1px 5px;letter-spacing:0.8px;}")
                                   .arg(mint().name(), rgba(accentDim()), rgba(accentBorder())));
    verify_tag_->hide();
    titleRow->addWidget(verify_tag_);
    titleRow->addStretch(1);
    titleStack->addLayout(titleRow);

    title_detail_ = new QLabel(QStringLiteral("Fetching and checking the signed update"), titleBar);
    title_detail_->setObjectName(QStringLiteral("updaterTitleDetail"));
    title_detail_->setFont(ui(11, QFont::Normal));
    title_detail_->setStyleSheet(QStringLiteral("color:%1;background:transparent;border:none;").arg(mut().name()));
    titleStack->addWidget(title_detail_);
    tbLayout->addLayout(titleStack, 1);

    close_button_ = new QPushButton(titleBar);
    close_button_->setFixedSize(34, 34);
    close_button_->setCursor(Qt::PointingHandCursor);
    close_button_->setToolTip(QStringLiteral("Close"));
    close_button_->setAccessibleName(QStringLiteral("Close updater"));
    close_button_->setStyleSheet(QStringLiteral("QPushButton{background:transparent;border:none;}"
                                                "QPushButton:hover:enabled{background:%1;border-radius:8px;}")
                                     .arg(rgba(line())));
    close_button_->setIcon(closeIcon(mut(), devicePixelRatioF()));
    connect(close_button_, &QPushButton::clicked, this, [this] { emit closeRequested(); });
    tbLayout->addWidget(close_button_);

    root->addWidget(titleBar);

    // ── Content ───────────────────────────────────────────────────────────────
    auto* content = new QWidget(this);
    content->setFixedWidth(kWindowWidth);
    auto* col = new QVBoxLayout(content);
    col->setContentsMargins(40, 20, 40, 20);
    col->setSpacing(14);

    // Version transition
    auto* versionBlock = new QWidget(content);
    versionBlock->setFixedHeight(55);
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

    // Ring
    ring_ = new ProgressRing(content);
    col->addWidget(ring_, 0, Qt::AlignHCenter);

    // Status line
    status_row_ = new QWidget(content);
    status_row_->setFixedHeight(22);
    auto* sr = new QHBoxLayout(status_row_);
    sr->setContentsMargins(0, 0, 0, 0);
    sr->setSpacing(9);
    auto* statusIcon = new GlyphWidget(16, status_row_);
    status_icon_ = statusIcon;
    sr->addWidget(statusIcon, 0, Qt::AlignVCenter);
    status_text_ = new QLabel(status_row_);
    sr->addWidget(status_text_, 0, Qt::AlignVCenter);
    col->addWidget(status_row_, 0, Qt::AlignHCenter);

    // Step list
    steps_ = new StepListWidget(content);
    steps_->setFixedHeight(210);
    col->addWidget(steps_);

    // Footer
    footer_container_ = new QWidget(content);
    footer_container_->setFixedHeight(170);
    footer_layout_ = new QVBoxLayout(footer_container_);
    footer_layout_->setContentsMargins(0, 0, 0, 0);
    footer_layout_->setSpacing(0);
    col->addWidget(footer_container_);

    root->addWidget(content);
}

std::array<QString, 5> UpdaterWindow::stepLabels() {
    return StepListWidget::labels();
}

bool UpdaterWindow::closeEnabled() const {
    return close_button_ != nullptr && close_button_->isEnabled();
}

QStringList UpdaterWindow::footerButtonLabels() const {
    QStringList out;
    for (auto* b : footer_buttons_)
        out << b->text();
    return out;
}

void UpdaterWindow::render(const UpdaterUiState& state) {
    updateTitleBar(state);

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
    close_button_->setToolTip(block ? QStringLiteral("Please wait - updating")
                                    : terminal ? QStringLiteral("Close")
                                               : QStringLiteral("Cancel update and close"));
    close_button_->setAccessibleName(block ? QStringLiteral("Close unavailable while updating")
                                          : terminal ? QStringLiteral("Close updater")
                                                     : QStringLiteral("Cancel update and close"));
    close_button_->setIcon(closeIcon(block ? line2() : mut(), devicePixelRatioF()));
    close_blocked_ = block;
}

void UpdaterWindow::updateTitleBar(const UpdaterUiState& state) {
    QString status = QStringLiteral("Preparing");
    QString detail = QStringLiteral("Getting the updater ready");
    QColor tone = mint();

    if (state.variant != TerminalVariant::None) {
        switch (state.variant) {
        case TerminalVariant::Success:
        case TerminalVariant::Green:
            status = QStringLiteral("Completed");
            detail = QStringLiteral("Version %1 is ready").arg(state.to_version);
            tone = success();
            break;
        case TerminalVariant::RebootRequired:
            status = QStringLiteral("Completed");
            detail = QStringLiteral("Restart Windows to finish");
            tone = success();
            break;
        case TerminalVariant::Amber:
            status = QStringLiteral("Warning");
            detail = state.headline;
            tone = caution();
            break;
        case TerminalVariant::Red:
            status = QStringLiteral("Error");
            detail = state.headline;
            tone = error();
            break;
        case TerminalVariant::None:
            break;
        }
    } else if (state.steps[size_t(UpStep::Launch)] == StepStatus::Working) {
        status = QStringLiteral("Launching");
        detail = QStringLiteral("Starting the updated app");
    } else if (state.steps[size_t(UpStep::Verify)] == StepStatus::Working) {
        status = QStringLiteral("Verifying");
        detail = QStringLiteral("Checking the installed files");
    } else if (state.steps[size_t(UpStep::Install)] == StepStatus::Working) {
        status = QStringLiteral("Installing");
        detail = QStringLiteral("Replacing the application files");
    } else if (state.steps[size_t(UpStep::CloseApp)] == StepStatus::Working) {
        status = QStringLiteral("Preparing");
        detail = QStringLiteral("Waiting for ExoSnap to close");
    } else if (state.steps[size_t(UpStep::Download)] == StepStatus::Working) {
        status = QStringLiteral("Downloading");
        detail = QStringLiteral("Fetching and checking the signed update");
    }

    title_status_->setText(status);
    title_status_->setStyleSheet(QStringLiteral("color:%1;background:transparent;border:none;").arg(tone.name()));
    title_detail_->setText(detail);
    verify_tag_->setVisible(state.verification_reinstall);
}

void UpdaterWindow::buildFooter(const UpdaterUiState& state) {
    footer_buttons_.clear();
    while (QLayoutItem* item = footer_layout_->takeAt(0)) {
        if (QWidget* w = item->widget())
            w->deleteLater();
        delete item;
    }
    footer_layout_->addStretch(1);

    const bool terminal = state.variant != TerminalVariant::None && state.variant != TerminalVariant::Success;

    auto* card = new QWidget(footer_container_);

    if (!terminal) {
        // In-progress (or soft Success) reassurance strip.
        const bool successDone = state.variant == TerminalVariant::Success;
        const QColor bgc = successDone ? accentDim() : surf2();
        const QColor bdc = successDone ? accentBorder() : line();
        card->setStyleSheet(QStringLiteral("QWidget{background:%1;border:1px solid %2;"
                                           "border-radius:11px;}")
                                .arg(rgba(bgc), rgba(bdc)));
        auto* row = new QHBoxLayout(card);
        row->setContentsMargins(14, 11, 14, 11);
        row->setSpacing(10);
        auto* shield = new GlyphWidget(16, card);
        shield->set(successDone ? Ico::Check : Ico::ShieldCheck, successDone ? mint() : success());
        shield->setStyleSheet(QStringLiteral("background:transparent;border:none;"));
        row->addWidget(shield, 0, Qt::AlignTop);
        auto* note = new QLabel(card);
        note->setWordWrap(true);
        note->setFont(ui(12, QFont::Normal));
        note->setStyleSheet(QStringLiteral("color:%1;background:transparent;border:none;").arg(dim().name()));
        note->setText(
            successDone
                ? QStringLiteral("ExoSnap %1 is starting - this window closes automatically.").arg(state.to_version)
                : QStringLiteral("Keep your computer on. ExoSnap reopens on %1 automatically "
                                 "when the swap finishes.")
                      .arg(state.to_version));
        row->addWidget(note, 1);
        footer_layout_->addWidget(card);
        return;
    }

    // Terminal result card, tinted by variant.
    QColor tone = caution();
    if (state.variant == TerminalVariant::Red)
        tone = error();
    else if (state.variant == TerminalVariant::Green || state.variant == TerminalVariant::RebootRequired)
        tone = success();

    auto* box = new QVBoxLayout(card);
    box->setContentsMargins(16, 14, 16, 14);
    box->setSpacing(13);

    card->setObjectName(QStringLiteral("updaterResultCard"));
    card->setStyleSheet(QStringLiteral("QWidget#updaterResultCard{background:%1;border:1px solid %2;"
                                       "border-radius:12px;}")
                            .arg(rgba(statusDim(tone)), rgba(statusBorder(tone))));

    auto* header = new QWidget(card);
    header->setStyleSheet(QStringLiteral("background:transparent;border:none;"));
    auto* hr = new QHBoxLayout(header);
    hr->setContentsMargins(0, 0, 0, 0);
    hr->setSpacing(9);
    auto* result_icon = new GlyphWidget(18, header);
    if (state.variant == TerminalVariant::Amber)
        result_icon->set(Ico::Warning, tone);
    else if (state.variant == TerminalVariant::Red)
        result_icon->set(Ico::Cross, tone);
    else
        result_icon->set(Ico::Check, tone);
    result_icon->setStyleSheet(QStringLiteral("background:transparent;border:none;"));
    hr->addWidget(result_icon, 0, Qt::AlignTop);

    auto* headline = new QLabel(state.headline, header);
    headline->setObjectName(QStringLiteral("updaterResultHeadline"));
    headline->setWordWrap(true);
    headline->setFont(ui(14, QFont::DemiBold));
    headline->setStyleSheet(QStringLiteral("color:%1;background:transparent;border:none;").arg(ink().name()));
    hr->addWidget(headline, 1);
    box->addWidget(header);

    auto* detail = new QLabel(state.detail_text, card);
    detail->setObjectName(QStringLiteral("updaterResultDetail"));
    detail->setWordWrap(true);
    detail->setFont(ui(12, QFont::Normal));
    detail->setStyleSheet(QStringLiteral("color:%1;background:transparent;border:none;").arg(mut().name()));
    box->addWidget(detail);

    auto* safety = new QWidget(card);
    safety->setStyleSheet(QStringLiteral("background:transparent;border:none;"));
    auto* safe_row = new QHBoxLayout(safety);
    safe_row->setContentsMargins(0, 0, 0, 0);
    safe_row->setSpacing(8);
    auto* shield = new GlyphWidget(16, safety);
    shield->set(Ico::ShieldCheck, success());
    shield->setStyleSheet(QStringLiteral("background:transparent;border:none;"));
    safe_row->addWidget(shield, 0, Qt::AlignTop);
    auto* safe_text = new QLabel(state.safety_text, safety);
    safe_text->setObjectName(QStringLiteral("updaterSafetyText"));
    safe_text->setWordWrap(true);
    safe_text->setFont(ui(12, QFont::Medium));
    safe_text->setStyleSheet(QStringLiteral("color:%1;background:transparent;border:none;").arg(ink().name()));
    safe_row->addWidget(safe_text, 1);
    box->addWidget(safety);

    // Action buttons, right-aligned: primary (filled) then secondary (outline).
    auto* actions = new QWidget(card);
    actions->setStyleSheet(QStringLiteral("background:transparent;border:none;"));
    auto* ar = new QHBoxLayout(actions);
    ar->setContentsMargins(0, 0, 0, 0);
    ar->setSpacing(8);
    ar->addStretch(1);
    if (!state.primary_action.isEmpty()) {
        auto* b = makeButton(state.primary_action, ButtonVariant::Primary, actions);
        const QString action = state.primary_action;
        connect(b, &QPushButton::clicked, this, [this, action] { emitForAction(action); });
        footer_buttons_ << b;
        ar->addWidget(b);
    }
    if (!state.secondary_action.isEmpty()) {
        auto* b = makeButton(state.secondary_action, ButtonVariant::Secondary, actions);
        const QString action = state.secondary_action;
        connect(b, &QPushButton::clicked, this, [this, action] { emitForAction(action); });
        footer_buttons_ << b;
        ar->addWidget(b);
    }
    box->addWidget(actions);

    footer_layout_->addWidget(card);
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
    event->accept();
}

void UpdaterWindow::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), bg());
    p.setPen(QPen(line2(), 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5));
}
