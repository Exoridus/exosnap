#pragma once
// Windows adapter: wraps the WGC + D3D11 capture pipeline behind IVideoCaptureSource.
// Full implementation wiring (session thread integration) is deferred to a later phase.
//
// D3D11 threading contract: all methods must be called from the VideoThread exclusively.

#include <recorder_core/interfaces/IVideoCaptureSource.h>
#include <recorder_core/recorder_session.h>

#include "../../wgc_capture.h"

#include <cstdint>
#include <string>

#include <d3d11.h>
#include <windows.h>

namespace recorder_core {

class WgcCaptureSrc : public IVideoCaptureSource {
  public:
    // target: the capture target (monitor or window) selected by the user.
    // device: D3D11 device shared with the encoder (borrowed, not owned).
    explicit WgcCaptureSrc(const CaptureTarget& target, ID3D11Device* device);
    ~WgcCaptureSrc() override;

    WgcCaptureSrc(const WgcCaptureSrc&) = delete;
    WgcCaptureSrc& operator=(const WgcCaptureSrc&) = delete;

    bool Open(std::string& out_error) override;

    bool AcquireFrame(RawVideoFrame& out_frame, std::string& out_error) override;

    void ReleaseFrame() override;

    uint32_t Width() const override;
    uint32_t Height() const override;

    bool IsSourceLost() const override;

    void Close() override;

  private:
    CaptureTarget m_target;
    ID3D11Device* m_device = nullptr; // borrowed
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    bool m_source_lost = false;
};

} // namespace recorder_core
