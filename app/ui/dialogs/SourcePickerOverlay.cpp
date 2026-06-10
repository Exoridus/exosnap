#include "SourcePickerOverlay.h"

#include "../theme/ExoSnapPalette.h"

#include <QColor>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QShowEvent>
#include <QVBoxLayout>

namespace exosnap::ui::dialogs {
namespace {

// Backdrop tint — matches Hybrid modal dim (rgba(8,8,10, 0.62)).
constexpr int kBackdropAlpha = 158;

} // namespace

SourcePickerOverlay::SourcePickerOverlay(QWidget* parent) : QWidget(parent) {
    setObjectName("sourcePickerOverlay");
    setFocusPolicy(Qt::StrongFocus);
    setVisible(false);

    // SourcePickerPanel is a plain QWidget — no native OS window created, so
    // no separate window chrome appears. It is embedded directly in the overlay.
    panel_ = new SourcePickerPanel(this);
    panel_->setObjectName("sourcePickerDialog"); // keep object name for QSS + tests
    panel_->setMaximumHeight(720);
    panel_->setMinimumWidth(900);

    // Center the picker panel over the backdrop.
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(30, 20, 30, 20);
    root->setSpacing(0);
    root->addStretch(1);
    root->addWidget(panel_, 0, Qt::AlignHCenter);
    root->addStretch(1);

    connect(panel_, &SourcePickerPanel::accepted, this, &SourcePickerOverlay::onPanelAccepted);
    connect(panel_, &SourcePickerPanel::rejected, this, &SourcePickerOverlay::closeOverlay);
    connect(panel_, &SourcePickerPanel::sourceDataRequested, this, &SourcePickerOverlay::sourceDataRequested);

    if (parent != nullptr)
        parent->installEventFilter(this);
}

void SourcePickerOverlay::openOverlay() {
    syncGeometryToParent();
    if (panel_)
        panel_->setVisible(true);
    setVisible(true);
    raise();
    if (panel_)
        panel_->setFocus(Qt::OtherFocusReason);
}

void SourcePickerOverlay::closeOverlay() {
    if (isHidden())
        return;
    if (panel_ && !panel_->isHidden())
        panel_->hide();
    setVisible(false);
    emit closed();
}

bool SourcePickerOverlay::isOpen() const noexcept {
    return !isHidden();
}

void SourcePickerOverlay::setScreenOptions(const std::vector<SourceOption>& options) {
    if (panel_)
        panel_->setScreenOptions(options);
}

void SourcePickerOverlay::setWindowOptions(const std::vector<SourceOption>& options) {
    if (panel_)
        panel_->setWindowOptions(options);
}

void SourcePickerOverlay::setRegionState(const QString& summary, bool has_region, bool select_on_record,
                                         const QRect& region_rect) {
    if (panel_)
        panel_->setRegionState(summary, has_region, select_on_record, region_rect);
}

void SourcePickerOverlay::setCurrentSection(Section section, int target_index) {
    if (panel_)
        panel_->setCurrentSelection(section, target_index);
}

#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
void SourcePickerOverlay::applyVisualRegionPreset(int preset_w, int preset_h) {
    if (panel_)
        panel_->applyVisualRegionPreset(preset_w, preset_h);
}
#endif

void SourcePickerOverlay::onPanelAccepted() {
    if (panel_ == nullptr)
        return;
    const auto result = panel_->selectionResult();
    emit sourceSelected(result);
    closeOverlay();
}

void SourcePickerOverlay::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        closeOverlay();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void SourcePickerOverlay::mousePressEvent(QMouseEvent* event) {
    if (panel_ == nullptr || !panel_->geometry().contains(event->pos())) {
        closeOverlay();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void SourcePickerOverlay::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    QColor backdrop(theme::ExoSnapPalette::kBg0);
    backdrop.setAlpha(kBackdropAlpha);
    painter.fillRect(rect(), backdrop);
}

bool SourcePickerOverlay::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget() &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Move || event->type() == QEvent::Show)) {
        syncGeometryToParent();
    }
    return QWidget::eventFilter(watched, event);
}

void SourcePickerOverlay::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    syncGeometryToParent();
    raise();
    if (panel_)
        panel_->setFocus(Qt::OtherFocusReason);
}

void SourcePickerOverlay::syncGeometryToParent() {
    if (QWidget* host = parentWidget())
        setGeometry(host->rect());
}

} // namespace exosnap::ui::dialogs
