#include "EditPlayerSurface.h"

#include "../../services/EditPlayerRenderer.h"

#include <QHideEvent>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QShowEvent>

#include <algorithm>

namespace exosnap::ui::widgets {

EditPlayerSurface::EditPlayerSurface(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("editPlayerSurface"));
    setMinimumHeight(180);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

EditPlayerSurface::~EditPlayerSurface() {
    // renderer_'s destructor (EditPlayerRenderer::~EditPlayerRenderer) calls
    // Shutdown(), which stops+joins the render thread and destroys the child
    // HWND -- explicit reset here only for readability, unique_ptr would do
    // it anyway on scope exit.
    renderer_.reset();
}

QSize EditPlayerSurface::sizeHint() const {
    return QSize(640, 360);
}

bool EditPlayerSurface::startGpuRendering() {
    if (renderer_)
        return true;

    setAttribute(Qt::WA_NativeWindow);
    winId();
    HWND hwnd = reinterpret_cast<HWND>(effectiveWinId());
    if (!hwnd)
        return false;

    // Win32 CreateWindowEx expects physical pixels for DPI-aware apps; Qt
    // width()/height() are logical -- same convention as PreviewSurface::
    // tryStartDxgiPreview.
    const qreal dpr = devicePixelRatioF();
    const uint32_t hwndW = static_cast<uint32_t>(std::max(1.0, width() * dpr));
    const uint32_t hwndH = static_cast<uint32_t>(std::max(1.0, height() * dpr));

    auto candidate = std::make_unique<exosnap::EditPlayerRenderer>();
    if (!candidate->Initialize(hwnd, hwndW, hwndH))
        return false; // stays on the legacy QPainter path (no hardware D3D11 adapter, etc.)

    renderer_ = std::move(candidate);
    has_frame_ = false;
    frame_ = QImage{};
    renderer_->ShowPlaceholder(placeholder_.toStdWString());
    update(); // one more repaint so paintEvent's early-return takes over cleanly
    return true;
}

void EditPlayerSurface::presentFrame(recorder_core::RawDecodedVideoFrame frame, float hdr_peak_scale) {
    has_frame_ = true;
    if (renderer_)
        renderer_->PresentFrame(std::move(frame), hdr_peak_scale);
}

void EditPlayerSurface::updateClockUs(int64_t media_time_us) noexcept {
    if (renderer_)
        renderer_->SetClockUs(media_time_us);
}

void EditPlayerSurface::setFrame(QImage frame) {
    frame_ = std::move(frame);
    has_frame_ = !frame_.isNull();
    if (renderer_) {
        // GPU path active (harness-only in practice -- see the class
        // comment): there is no raw YUV frame to hand the renderer here, so
        // fall back to its placeholder instead of silently doing nothing.
        renderer_->ShowPlaceholder(placeholder_.toStdWString());
        return;
    }
    update();
}

void EditPlayerSurface::clearFrame() {
    if (!has_frame_)
        return;
    has_frame_ = false;
    frame_ = QImage{};
    if (renderer_)
        renderer_->ShowPlaceholder(placeholder_.toStdWString());
    else
        update();
}

void EditPlayerSurface::setPlaceholderText(const QString& text) {
    if (placeholder_ == text)
        return;
    placeholder_ = text;
    if (has_frame_)
        return; // never interrupts a frame already on screen
    if (renderer_)
        renderer_->ShowPlaceholder(placeholder_.toStdWString());
    else
        update();
}

void EditPlayerSurface::applyResize() {
    if (!renderer_)
        return;
    const qreal dpr = devicePixelRatioF();
    const uint32_t hwndW = static_cast<uint32_t>(std::max(1.0, width() * dpr));
    const uint32_t hwndH = static_cast<uint32_t>(std::max(1.0, height() * dpr));
    renderer_->Resize(hwndW, hwndH);
}

void EditPlayerSurface::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    applyResize();
}

void EditPlayerSurface::hideEvent(QHideEvent* event) {
    if (renderer_)
        renderer_->SetChildWindowVisible(false);
    QWidget::hideEvent(event);
}

void EditPlayerSurface::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (renderer_)
        renderer_->SetChildWindowVisible(true);
}

void EditPlayerSurface::paintEvent(QPaintEvent* /*event*/) {
    if (renderer_)
        return; // native child HWND occludes this rect; nothing to paint

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF frame_rect = rect().adjusted(0.5, 0.5, -0.5, -0.5);

    QLinearGradient bg_grad(frame_rect.topLeft(), frame_rect.bottomRight());
    bg_grad.setColorAt(0.0, QColor("#181612"));
    bg_grad.setColorAt(1.0, QColor("#0e0d0b"));
    painter.setBrush(bg_grad);
    painter.setPen(QPen(QColor("#353330"), 1.0));
    painter.drawRoundedRect(frame_rect, 5.0, 5.0);

    if (!frame_.isNull()) {
        painter.save();
        QPainterPath clip_path;
        clip_path.addRoundedRect(frame_rect, 5.0, 5.0);
        painter.setClipPath(clip_path);

        const double sx = static_cast<double>(width()) / frame_.width();
        const double sy = static_cast<double>(height()) / frame_.height();
        const double s = std::min(sx, sy);
        const int dw = static_cast<int>(frame_.width() * s);
        const int dh = static_cast<int>(frame_.height() * s);
        const int dx = (width() - dw) / 2;
        const int dy = (height() - dh) / 2;
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawImage(QRect(dx, dy, dw, dh), frame_);
        painter.restore();
        return;
    }

    if (!placeholder_.isEmpty()) {
        painter.setPen(QColor(255, 255, 255, 120));
        // The play/pause control floats centered over this surface (60 px circle),
        // so the placeholder caption sits below the vertical center — clear of the
        // control — instead of colliding with it.
        constexpr qreal kCaptionOffset = 44.0; // 30 px control half-height + gap
        QRectF text_rect = frame_rect.adjusted(16, 16, -16, -16);
        text_rect.setTop(std::min(frame_rect.center().y() + kCaptionOffset, text_rect.bottom() - 16.0));
        painter.drawText(text_rect, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, placeholder_);
    }
}

} // namespace exosnap::ui::widgets
