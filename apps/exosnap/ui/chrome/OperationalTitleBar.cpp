#include "OperationalTitleBar.h"

#include "../brand/BrandMarkWidget.h"
#include "../widgets/StatusPill.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPaintEvent>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>

namespace exosnap::ui::chrome {

OperationalTitleBar::OperationalTitleBar(QWidget* parent) : QWidget(parent) {
    setObjectName("operationalTitleBar");
    setFixedHeight(kHeight);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(10, 0, 0, 0);
    root->setSpacing(8);

    brand_block_ = new QWidget(this);
    brand_block_->setObjectName("titlebarBrandBlock");
    auto* brand_layout = new QHBoxLayout(brand_block_);
    brand_layout->setContentsMargins(4, 0, 4, 0);
    brand_layout->setSpacing(8);

    // 22 px keeps the E-silhouette legible in a compact 48 px chrome.
    auto* mark = new ui::brand::BrandMarkWidget(brand_block_);
    mark->setFixedSize(22, 22);

    auto* wordmark = new QLabel("EXO·SNAP", brand_block_);
    wordmark->setProperty("labelRole", "titlebarWordmark");
    wordmark->setTextFormat(Qt::RichText);
    wordmark->setText("EXO<span style=\"color:#f1b400;\">&middot;</span>SNAP");

    brand_layout->addWidget(mark, 0, Qt::AlignVCenter);
    brand_layout->addWidget(wordmark, 0, Qt::AlignVCenter);

    auto* sep_left = new QFrame(this);
    sep_left->setFrameShape(QFrame::VLine);
    sep_left->setObjectName("titlebarDivider");

    auto* context_block = new QWidget(this);
    context_block->setObjectName("titlebarContextBlock");
    auto* context_layout = new QHBoxLayout(context_block);
    context_layout->setContentsMargins(4, 0, 4, 0);
    context_layout->setSpacing(10);

    page_code_label_ = new QLabel("01 · RECORD", context_block);
    page_code_label_->setProperty("labelRole", "titlebarPageCode");

    status_pill_ = new ui::widgets::StatusPill(context_block);
    status_pill_->setTone(ui::widgets::StatusPill::Tone::Ready);
    status_pill_->setText("READY");

    context_label_ = new QLabel("DISPLAY1 · 2560×1440 · 60 fps · AV1", context_block);
    context_label_->setProperty("labelRole", "titlebarContext");

    context_layout->addWidget(page_code_label_, 0, Qt::AlignVCenter);
    context_layout->addWidget(status_pill_, 0, Qt::AlignVCenter);
    context_layout->addWidget(context_label_, 1, Qt::AlignVCenter);

    auto* runtime_block = new QWidget(this);
    runtime_block->setObjectName("titlebarRuntimeBlock");
    auto* runtime_layout = new QHBoxLayout(runtime_block);
    runtime_layout->setContentsMargins(0, 0, 12, 0);
    runtime_layout->setSpacing(12);

    cpu_label_ = new QLabel("CPU 8.2%", runtime_block);
    gpu_label_ = new QLabel("GPU 14.4%", runtime_block);
    ram_label_ = new QLabel("RAM 612 MB", runtime_block);
    for (QLabel* label : {cpu_label_, gpu_label_, ram_label_})
        label->setProperty("labelRole", "titlebarRuntime");

    runtime_layout->addWidget(cpu_label_);
    runtime_layout->addWidget(gpu_label_);
    runtime_layout->addWidget(ram_label_);

    auto* sep_right = new QFrame(this);
    sep_right->setFrameShape(QFrame::VLine);
    sep_right->setObjectName("titlebarDivider");

    auto* controls = new QWidget(this);
    controls->setObjectName("titlebarControls");
    auto* controls_layout = new QHBoxLayout(controls);
    controls_layout->setContentsMargins(0, 0, 0, 0);
    controls_layout->setSpacing(0);

    minimize_btn_ = new QPushButton("—", controls);
    maximize_btn_ = new QPushButton("□", controls);
    close_btn_ = new QPushButton("✕", controls);

    for (QPushButton* button : {minimize_btn_, maximize_btn_, close_btn_}) {
        button->setObjectName("titlebarWindowButton");
        button->setFixedSize(44, kHeight);
        button->setFocusPolicy(Qt::NoFocus);
    }
    close_btn_->setProperty("windowControlRole", "close");

    controls_layout->addWidget(minimize_btn_);
    controls_layout->addWidget(maximize_btn_);
    controls_layout->addWidget(close_btn_);

    root->addWidget(brand_block_, 0, Qt::AlignVCenter);
    root->addWidget(sep_left);
    root->addWidget(context_block, 1);
    root->addWidget(runtime_block, 0, Qt::AlignVCenter);
    root->addWidget(sep_right);
    root->addWidget(controls);

    connect(minimize_btn_, &QPushButton::clicked, this, &OperationalTitleBar::minimizeRequested);
    connect(maximize_btn_, &QPushButton::clicked, this, &OperationalTitleBar::maximizeRestoreRequested);
    connect(close_btn_, &QPushButton::clicked, this, &OperationalTitleBar::closeRequested);
}

void OperationalTitleBar::setPageContext(const QString& page_code, const QString& context_text) {
    page_code_label_->setText(page_code);
    context_label_->setText(context_text);
}

void OperationalTitleBar::setRecordingActive(bool recording) {
    if (recording_active_ == recording)
        return;
    recording_active_ = recording;
    update();
}

bool OperationalTitleBar::isRecordingActive() const noexcept {
    return recording_active_;
}

void OperationalTitleBar::setStatusLabel(const QString& status_text) {
    status_pill_->setText(status_text);
}

void OperationalTitleBar::setRuntimeMeta(const QString& cpu_text, const QString& gpu_text, const QString& ram_text) {
    cpu_label_->setText(cpu_text);
    gpu_label_->setText(gpu_text);
    ram_label_->setText(ram_text);
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

    painter.setPen(QPen(QColor("#2a2620"), 1.0));
    painter.drawLine(0, 0, width(), 0);

    const QColor border = recording_active_ ? QColor("#7d5f00") : QColor("#2a2620");
    painter.setPen(QPen(border, 1.0));
    painter.drawLine(0, height() - 1, width(), height() - 1);
}

} // namespace exosnap::ui::chrome
