#pragma once

// WGC (Windows Graphics Capture) helpers.
// All D3D11 context / video context usage is EXCLUSIVE to VideoThread.
// See D3D11 threading contract in video_thread.cpp.

#include <recorder_core/recorder_session.h>

#include <cstdint>
#include <vector>

#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <windows.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

namespace recorder_core {

// Enumerate capture targets: monitors and top-level windows.
// May be called from any thread; does not require a D3D11 device.
std::vector<CaptureTarget> EnumerateWgcTargets();

// Utility: wide string to UTF-8
std::string WideToUtf8(const wchar_t* wstr);

} // namespace recorder_core
