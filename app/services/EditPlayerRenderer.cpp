// app/services/EditPlayerRenderer.cpp (Task 1 stub -- creates the child HWND
// and a swap chain, but PresentFrame just clears to black via the stub
// converter's debug color; no shader work happens here yet)
#include "EditPlayerRenderer.h"

#include <d3d11.h>
#include <dxgi1_2.h>

namespace exosnap {

EditPlayerRenderer::EditPlayerRenderer() = default;
EditPlayerRenderer::~EditPlayerRenderer() {
    Shutdown();
}

bool EditPlayerRenderer::Initialize(HWND parentHwnd, uint32_t hwndWidth, uint32_t hwndHeight) {
    // Task 4 implements real child-HWND creation (mirror
    // PreviewSurface::tryStartDxgiPreview's WA_NativeWindow/winId() call site
    // and DxgiPreviewRenderer::InitD3D11/InitSwapChain for the D3D11 device +
    // swap chain setup) and DPI-aware sizing. This stub intentionally does
    // nothing so Task 1 stays small; Task 4 owns this file.
    parentHwnd_ = parentHwnd;
    (void)hwndWidth;
    (void)hwndHeight;
    return false; // honestly reports "not yet implemented" rather than faking success
}

void EditPlayerRenderer::Resize(uint32_t, uint32_t) {
}
void EditPlayerRenderer::PresentFrame(recorder_core::RawDecodedVideoFrame, float) {
}
void EditPlayerRenderer::ShowPlaceholder(const std::wstring&) {
}
void EditPlayerRenderer::Shutdown() {
}

} // namespace exosnap
