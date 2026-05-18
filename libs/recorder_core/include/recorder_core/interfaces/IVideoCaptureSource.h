#pragma once
// Platform-agnostic video capture interface.
// Windows implementation: WgcCaptureSrc (wraps WgcCapture + D3D11).
// Add macOS/Linux implementations in platform/macos/ and platform/linux/ later.

#include <cstdint>
#include <string>

namespace recorder_core {

struct RawVideoFrame {
    // GPU-side handle (fastest path): non-null when the backend can deliver
    // the frame without a GPU->CPU blit (e.g. D3D11 texture on Windows).
    // On Windows: ID3D11Texture2D* cast to void*.
    // Null when only cpu_bytes is available.
    void* gpu_texture_handle = nullptr;

    // CPU-side bytes (fallback): non-owning, valid only until ReleaseFrame().
    // NV12 layout: Y plane followed by interleaved UV plane.
    const uint8_t* cpu_bytes = nullptr;
    uint32_t stride_y = 0;

    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t pts_ns = 0;
};

class IVideoCaptureSource {
  public:
    virtual ~IVideoCaptureSource() = default;

    // Open the capture source. Must be called before AcquireFrame().
    virtual bool Open(std::string& out_error) = 0;

    // Poll for the next frame.
    // Returns true + frame.gpu_texture_handle or frame.cpu_bytes set -> frame ready.
    // Returns true + both null -> no frame yet (non-blocking, call again).
    // Returns false -> fatal error (out_error set).
    virtual bool AcquireFrame(RawVideoFrame& out_frame, std::string& out_error) = 0;

    // Release the frame acquired by the last AcquireFrame(). Must be called
    // before the next AcquireFrame().
    virtual void ReleaseFrame() = 0;

    virtual uint32_t Width() const = 0;
    virtual uint32_t Height() const = 0;

    // True when the capture source is gone (window closed, monitor disconnected).
    virtual bool IsSourceLost() const = 0;

    virtual void Close() = 0;
};

} // namespace recorder_core
