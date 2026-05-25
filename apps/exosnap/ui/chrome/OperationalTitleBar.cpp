#include "OperationalTitleBar.h"

#include "../brand/BrandMarkWidget.h"
#include "../widgets/StatusPill.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QStyle>

namespace exosnap::ui::chrome {
namespace {

QFrame* makeSeparator(QWidget* parent) {
    auto* separator = new QFrame(parent);
    separator->setObjectName("titlebarSeparator");
    separator->setFrameShape(QFrame::VLine);
    separator->setFixedWidth(1);
    return separator;
}

QString fallbackDash(const QString& text) {
    const QString trimmed = text.trimmed();
    return trimmed.isEmpty() ? QStringLiteral("–") : trimmed;
}

} // namespace

OperationalTitleBar::OperationalTitleBar(QWidget* parent) : QWidget(parent) {
    setObjectName("operationalTitleBar");
    setFixedHeight(kHeight);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setProperty("recording", false);

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* brand_slot = new QWidget(this);
    brand_slot->setObjectName("titlebarBrandSlot");
    brand_slot->setFixedWidth(120);
    auto* brand_layout = new QHBoxLayout(brand_slot);
    brand_layout->setContentsMargins(10, 0, 10, 0);
    brand_layout->setSpacing(8);

    auto* mark = new ui::brand::BrandMarkWidget(brand_slot);
    mark->setFixedSize(20, 20);
    auto* wordmark = new QLabel("EXO·SNAP", brand_slot);
    wordmark->setProperty("labelRole", "titlebarWordmark");

    brand_layout->addWidget(mark, 0, Qt::AlignVCenter);
    brand_layout->addWidget(wordmark, 0, Qt::AlignVCenter);
    brand_layout->addStretch(1);

    auto* page_slot = new QWidget(this);
    page_slot->setObjectName("titlebarPageSlot");
    page_slot->setFixedWidth(220);
    auto* page_layout = new QHBoxLayout(page_slot);
    page_layout->setContentsMargins(10, 0, 10, 0);
    page_layout->setSpacing(8);

    page_code_label_ = new QLabel("01 · RECORD", page_slot);
    page_code_label_->setProperty("labelRole", "titlebarPageCode");

    status_pill_ = new ui::widgets::StatusPill(page_slot);
    status_pill_->setText("READY");
    status_pill_->setTone(ui::widgets::StatusPill::Tone::Ready);

    page_layout->addWidget(page_code_label_, 0, Qt::AlignVCenter);
    page_layout->addWidget(status_pill_, 0, Qt::AlignVCenter);
    page_layout->addStretch(1);

    auto* capture_slot = new QWidget(this);
    capture_slot->setObjectName("titlebarCaptureSlot");
    capture_slot->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* capture_layout = new QHBoxLayout(capture_slot);
    capture_layout->setContentsMargins(12, 0, 12, 0);
    capture_layout->setSpacing(0);

    context_label_ = new QLabel("DISPLAY1 · 2560×1440 · 60 fps · AV1", capture_slot);
    context_label_->setProperty("labelRole", "titlebarContext");
    capture_layout->addWidget(context_label_, 1, Qt::AlignVCenter);

    auto* metrics_slot = new QWidget(this);
    metrics_slot->setObjectName("titlebarMetricsSlot");
    metrics_slot->setFixedWidth(260);
    auto* metrics_layout = new QHBoxLayout(metrics_slot);
    metrics_layout->setContentsMargins(0, 0, 12, 0);
    metrics_layout->setSpacing(0);

    metrics_label_ = new QLabel(metrics_slot);
    metrics_label_->setProperty("labelRole", "titlebarRuntime");
    metrics_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    metrics_label_->setTextFormat(Qt::RichText);
    metrics_layout->addWidget(metrics_label_);

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
    root->addWidget(makeSeparator(this), 0, Qt::AlignVCenter);
    root->addWidget(page_slot);
    root->addWidget(makeSeparator(this), 0, Qt::AlignVCenter);
    root->addWidget(capture_slot, 1);
    root->addWidget(metrics_slot);
    root->addWidget(controls);

    connect(minimize_btn_, &QPushButton::clicked, this, &OperationalTitleBar::minimizeRequested);
    connect(maximize_btn_, &QPushButton::clicked, this, &OperationalTitleBar::maximizeRestoreRequested);
    connect(close_btn_, &QPushButton::clicked, this, &OperationalTitleBar::closeRequested);

    refreshStatusChip();
    refreshMetricsLabel();
}

void OperationalTitleBar::setPageContext(const QString& page_code, const QString& context_text) {
    page_code_label_->setText(page_code);
    context_label_->setText(context_text);
}

void OperationalTitleBar::setRecordingActive(bool recording) {
    if (recording_active_ == recording)
        return;
    recording_active_ = recording;
    setProperty("recording", recording_active_);
    style()->unpolish(this);
    style()->polish(this);
    refreshStatusChip();
    refreshMetricsLabel();
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

void OperationalTitleBar::setRuntimeMeta(const QString& cpu_text, const QString& gpu_text, const QString& ram_text) {
    idle_cpu_text_ = fallbackDash(cpu_text);
    idle_gpu_text_ = fallbackDash(gpu_text);
    idle_ram_text_ = fallbackDash(ram_text);
    refreshMetricsLabel();
}

void OperationalTitleBar::setRecordingRuntime(const QString& elapsed_text, const QString& bitrate_text,
                                              const QString& drop_text) {
    rec_elapsed_text_ = elapsed_text.trimmed().isEmpty() ? QStringLiteral("--:--:--") : elapsed_text.trimmed();
    rec_bitrate_text_ = fallbackDash(bitrate_text);
    rec_drop_text_ = fallbackDash(drop_text);
    refreshStatusChip();
    refreshMetricsLabel();
}

void OperationalTitleBar::setMaximizedState(bool maximized) {
    maximize_btn_->setText(maximized ? "❐" : "□");
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
    painter.fillRect(rect(), QColor("#14130f"));
    painter.setPen(QPen(recording_active_ ? QColor("#d7a744") : QColor("#292826"), 1.0));
    painter.drawLine(0, height() - 1, width(), height() - 1);
}

void OperationalTitleBar::refreshStatusChip() {
    const QString status = status_label_.trimmed().toUpper();
    if (status.contains(QStringLiteral("REC"))) {
        status_pill_->setTone(ui::widgets::StatusPill::Tone::Recording);
        status_pill_->setDotVisible(true);
        status_pill_->setText(QStringLiteral("REC"));
    } else if (status.contains(QStringLiteral("PAUSED"))) {
        status_pill_->setTone(ui::widgets::StatusPill::Tone::Warn);
        status_pill_->setDotVisible(true);
        status_pill_->setText(QStringLiteral("PAUSED"));
    } else if (status.contains(QStringLiteral("STOPPING"))) {
        status_pill_->setTone(ui::widgets::StatusPill::Tone::Warn);
        status_pill_->setDotVisible(true);
        status_pill_->setText(QStringLiteral("STOPPING"));
    } else if (status.contains(QStringLiteral("STARTING"))) {
        status_pill_->setTone(ui::widgets::StatusPill::Tone::Warn);
        status_pill_->setDotVisible(true);
        status_pill_->setText(QStringLiteral("STARTING"));
    } else if (status.contains(QStringLiteral("CHECK"))) {
        status_pill_->setTone(ui::widgets::StatusPill::Tone::Warn);
        status_pill_->setDotVisible(true);
        status_pill_->setText(QStringLiteral("CHECKING"));
    } else if (status.contains(QStringLiteral("ERROR"))) {
        status_pill_->setTone(ui::widgets::StatusPill::Tone::Blocked);
        status_pill_->setDotVisible(true);
        status_pill_->setText(QStringLiteral("ERROR"));
    } else if (status.contains(QStringLiteral("BLOCK"))) {
        status_pill_->setTone(ui::widgets::StatusPill::Tone::Blocked);
        status_pill_->setDotVisible(true);
        status_pill_->setText(QStringLiteral("BLOCKED"));
    } else {
        status_pill_->setTone(ui::widgets::StatusPill::Tone::Ready);
        status_pill_->setDotVisible(false);
        status_pill_->setText(QStringLiteral("READY"));
    }
}

void OperationalTitleBar::refreshMetricsLabel() {
    if (recording_active_) {
        metrics_label_->setText(QStringLiteral("BITRATE <b>%1</b>  DROP <b>%2</b>")
                                    .arg(rec_bitrate_text_.toHtmlEscaped(), rec_drop_text_.toHtmlEscaped()));
        return;
    }

    metrics_label_->setText(
        QStringLiteral("CPU <b>%1</b>  GPU <b>%2</b>  RAM <b>%3</b>")
            .arg(idle_cpu_text_.toHtmlEscaped(), idle_gpu_text_.toHtmlEscaped(), idle_ram_text_.toHtmlEscaped()));
}

} // namespace exosnap::ui::chrome
