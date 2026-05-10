#pragma once

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <d3d11.h>
#include <dxgi1_2.h>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace wgc_preview {

struct CaptureTarget {
    enum class Kind { Monitor, Window };

    Kind kind = Kind::Monitor;
    std::wstring description;
    HMONITOR hmonitor = nullptr;
    HWND hwnd = nullptr;
};

class Probe {
  public:
    static std::vector<CaptureTarget> EnumerateTargets();

    explicit Probe(const CaptureTarget& target);
    ~Probe();

    Probe(const Probe&) = delete;
    Probe& operator=(const Probe&) = delete;

    bool Start();
    void Run();

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

  private:
    CaptureTarget m_target;
    bool m_running = false;

    winrt::com_ptr<ID3D11Device> m_d3dDevice;
    winrt::com_ptr<ID3D11DeviceContext> m_d3dContext;
    winrt::com_ptr<IDXGISwapChain1> m_swapChain;
    HWND m_hwnd = nullptr;

    winrt::Windows::Graphics::Capture::GraphicsCaptureItem m_item{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool m_framePool{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession m_session{nullptr};

    winrt::event_token m_closedToken{};

    std::mutex m_frameMutex;
    winrt::com_ptr<ID3D11Texture2D> m_latestFrame;
    uint32_t m_frameWidth = 0;
    uint32_t m_frameHeight = 0;

    std::chrono::steady_clock::time_point m_startTime;
    std::chrono::steady_clock::time_point m_lastMetricTime;
    uint64_t m_totalFrameCount = 0;
    uint64_t m_lastMetricFrameCount = 0;
    bool m_hadFirstFrame = false;
    bool m_sourceLost = false;

    bool InitD3D11();
    bool InitCaptureItem();
    bool InitFramePool(HWND hwnd);
    bool InitPreviewWindow();
    void CleanupCapture();
    void CleanupWindow();

    void PollAndProcessFrames();
    void RenderLatestFrame();
    void PrintMetrics();
    void PrintSummary();

    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
};

} // namespace wgc_preview
