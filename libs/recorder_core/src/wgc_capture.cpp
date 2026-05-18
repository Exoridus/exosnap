#include "wgc_capture.h"

#include <windows.h>

namespace recorder_core {

std::vector<CaptureTarget> EnumerateWgcTargets() {
    std::vector<CaptureTarget> targets;

    winrt::com_ptr<IDXGIFactory1> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())))) {
        winrt::com_ptr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapters1(i, adapter.put()) != DXGI_ERROR_NOT_FOUND; ++i) {
            winrt::com_ptr<IDXGIOutput> output;
            for (UINT j = 0; adapter->EnumOutputs(j, output.put()) != DXGI_ERROR_NOT_FOUND; ++j) {
                DXGI_OUTPUT_DESC desc{};
                if (SUCCEEDED(output->GetDesc(&desc)) && desc.AttachedToDesktop) {
                    MONITORINFOEXW mi{};
                    mi.cbSize = sizeof(mi);
                    CaptureTarget t;
                    t.kind = CaptureTarget::Kind::Monitor;
                    t.hmonitor = desc.Monitor;
                    t.description = GetMonitorInfoW(desc.Monitor, &mi) ? mi.szDevice : std::to_wstring(targets.size());
                    targets.push_back(std::move(t));
                }
                output = nullptr;
            }
            adapter = nullptr;
        }
    }

    EnumWindows(
        [](HWND hwnd, LPARAM lParam) -> BOOL {
            auto& vec = *reinterpret_cast<std::vector<CaptureTarget>*>(lParam);
            if (!IsWindowVisible(hwnd))
                return TRUE;
            if ((GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) != 0)
                return TRUE;
            if (GetWindow(hwnd, GW_OWNER) != nullptr)
                return TRUE;
            wchar_t title[256] = {};
            if (GetWindowTextW(hwnd, title, 256) == 0)
                return TRUE;
            CaptureTarget t;
            t.kind = CaptureTarget::Kind::Window;
            t.description = title;
            t.hwnd = hwnd;
            vec.push_back(std::move(t));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&targets));

    return targets;
}

std::string WideToUtf8(const wchar_t* wstr) {
    if (wstr == nullptr || wstr[0] == L'\0')
        return "(unnamed)";
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0)
        return "(unnamed)";
    std::string result(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, result.data(), len, nullptr, nullptr);
    return result;
}

} // namespace recorder_core
