#include <recorder_core/edit_frame_gpu_converter.h>

namespace recorder_core {

bool EditFrameGpuConverter::Init(ID3D11Device* device, ID3D11DeviceContext* context, std::string& err) {
    if (device == nullptr || context == nullptr) {
        err = "EditFrameGpuConverter::Init invalid arguments";
        return false;
    }
    device_ = device;
    context_ = context;
    return true;
}

// STUB (Task 1 prep only): clears dst to a fixed debug color instead of
// converting, so Task 4 can build and test the render pipeline (child HWND,
// swap chain, present loop, resize) before Task 2's real shaders land. Task 2
// replaces this whole file.
bool EditFrameGpuConverter::Convert(const RawDecodedVideoFrame& frame, ID3D11Texture2D* dst, float /*hdr_peak_scale*/,
                                    std::string& err) {
    if (context_ == nullptr || dst == nullptr || frame.width == 0 || frame.height == 0) {
        err = "EditFrameGpuConverter::Convert called before Init or with invalid arguments";
        return false;
    }
    winrt::com_ptr<ID3D11RenderTargetView> rtv;
    if (FAILED(device_->CreateRenderTargetView(dst, nullptr, rtv.put()))) {
        err = "EditFrameGpuConverter::Convert CreateRenderTargetView failed";
        return false;
    }
    const float magenta[4] = {1.0f, 0.0f, 1.0f, 1.0f}; // unmissable stub marker
    context_->ClearRenderTargetView(rtv.get(), magenta);
    return true;
}

} // namespace recorder_core
