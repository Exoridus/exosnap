#include "WebcamPreviewFeed.h"

namespace exosnap::quick {

void WebcamPreviewFeed::setFrame(std::shared_ptr<const exosnap::engine::WebcamFrameSnapshot> frame) {
    if (frame_ == frame)
        return;
    frame_ = std::move(frame);
    emit frameChanged();
}

void WebcamPreviewFeed::setStyle(bool mirror, float opacity, const exosnap::engine::ChromaKeyParams& chroma) {
    if (mirror_ == mirror && opacity_ == opacity && chroma_.enabled == chroma.enabled && chroma_.r == chroma.r &&
        chroma_.g == chroma.g && chroma_.b == chroma.b && chroma_.tolerance == chroma.tolerance &&
        chroma_.softness == chroma.softness && chroma_.spill_reduction == chroma.spill_reduction) {
        return;
    }
    mirror_ = mirror;
    opacity_ = opacity;
    chroma_ = chroma;
    emit styleChanged();
}

void WebcamPreviewFeed::addViewer() {
    if (++viewers_ == 1)
        emit hasViewersChanged();
}

void WebcamPreviewFeed::removeViewer() {
    if (viewers_ == 0)
        return;
    if (--viewers_ == 0)
        emit hasViewersChanged();
}

} // namespace exosnap::quick
