// clang-format off
#include <windows.h>
#include <dxgi.h>
#include <mmdeviceapi.h>
// clang-format on

#include "SelfTestRunner.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace exosnap::diagnostics {

namespace {

uint64_t NowTimestamp() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

DiagnosticResult MakeResult(const std::string& id, DiagnosticGroup group, DiagnosticSeverity sev,
                            const std::string& title, const std::string& summary, const std::string& detail = "",
                            const std::string& current_value = "", const std::string& recommendation = "") {
    DiagnosticResult r;
    r.id = id;
    r.group = group;
    r.severity = sev;
    r.title = title;
    r.summary = summary;
    r.detail = detail;
    r.current_value = current_value;
    r.recommendation = recommendation;
    // fix_action is intentionally left as std::nullopt — self-tests never provide a FixAction.
    r.timestamp = NowTimestamp();
    return r;
}

} // namespace

DiagnosticChecklist SelfTestRunner::Run() const {
    DiagnosticChecklist checklist;
    bool all_pass = true;

    auto add_result = [&](const SelfTestResult& st, const std::string& id) {
        DiagnosticSeverity sev = st.passed ? DiagnosticSeverity::Pass : DiagnosticSeverity::Notice;
        if (!st.passed)
            all_pass = false;
        DiagnosticResult r = MakeResult("st." + id, DiagnosticGroup::SelfTest, sev, "Self-test: " + st.category,
                                        st.passed ? "PASS" : "WARN", st.detail, st.passed ? "OK" : "Warning",
                                        st.passed ? "" : "Investigate " + st.category + " availability.");
        checklist.results.push_back(std::move(r));
    };

    add_result(CheckCaptureAvailability(), "capture");
    add_result(CheckEncoderAvailability(), "encoder");
    add_result(CheckMuxerAvailability(), "muxer");
    add_result(CheckOutputPathWritable(""), "output_path");
    add_result(CheckAudioDeviceAvailability(), "audio_device");

    if (!all_pass)
        checklist.has_notice = true;
    return checklist;
}

SelfTestResult SelfTestRunner::CheckCaptureAvailability() {
    SelfTestResult result;
    result.category = "Capture";

    IDXGIFactory1* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
    if (factory) {
        factory->Release();
        factory = nullptr;
    }

    if (SUCCEEDED(hr)) {
        result.passed = true;
        result.detail = "DXGI available \xe2\x80\x94 display capture can initialize.";
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "DXGI initialization failed (HRESULT=0x%08X).", static_cast<unsigned>(hr));
        result.passed = false;
        result.detail = buf;
    }
    return result;
}

SelfTestResult SelfTestRunner::CheckEncoderAvailability() {
    SelfTestResult result;
    result.category = "Encoder";

    HMODULE h = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (h) {
        FreeLibrary(h);
        result.passed = true;
        result.detail = "NVENC encoder library available (nvEncodeAPI64.dll found).";
    } else {
        result.passed = false;
        result.detail = "NVENC encoder library not found (nvEncodeAPI64.dll). NVIDIA GPU with driver support required.";
    }
    return result;
}

SelfTestResult SelfTestRunner::CheckMuxerAvailability() {
    SelfTestResult result;
    result.category = "Muxer";

    namespace fs = std::filesystem;
    std::string target = fs::temp_directory_path().string();
    auto probe = fs::path(target) / "exosnap_muxer_probe.tmp";
    try {
        std::ofstream f(probe, std::ios::binary);
        if (!f) {
            result.passed = false;
            result.detail = "Cannot write to: " + target;
            return result;
        }
        f.close();
        fs::remove(probe);
        result.passed = true;
        result.detail = "Output path writable \xe2\x80\x94 muxer can write to: " + target;
    } catch (const std::exception& e) {
        result.passed = false;
        result.detail = std::string("Muxer path check exception: ") + e.what();
    }
    return result;
}

SelfTestResult SelfTestRunner::CheckOutputPathWritable(const std::string& path) {
    SelfTestResult result;
    result.category = "Output Path";

    namespace fs = std::filesystem;
    std::string target = path.empty() ? fs::temp_directory_path().string() : path;
    auto probe = fs::path(target) / "exosnap_selftest_write_check.tmp";
    try {
        std::ofstream f(probe, std::ios::binary);
        if (!f) {
            result.passed = false;
            result.detail = "Cannot write to: " + target;
            return result;
        }
        f << "exosnap-selftest";
        f.close();
        fs::remove(probe);
        result.passed = true;
        result.detail = "Output path is writable \xe2\x80\x94 probe succeeded for: " + target;
    } catch (...) {
        result.passed = false;
        result.detail = "Exception during output path writability check for: " + target;
    }
    return result;
}

SelfTestResult SelfTestRunner::CheckAudioDeviceAvailability() {
    SelfTestResult result;
    result.category = "Audio Devices";

    // Initialize COM on this thread. S_OK = we initialized it, S_FALSE = already initialized
    // (ref-count bumped either way), RPC_E_CHANGED_MODE = already initialized in a different
    // apartment (COM is still usable). Call CoUninitialize only when we bumped the ref-count.
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool uninit_on_exit = SUCCEEDED(hrCo);

    IMMDeviceEnumerator* pEnumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                  __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&pEnumerator));
    if (FAILED(hr)) {
        if (uninit_on_exit)
            CoUninitialize();
        char buf[80];
        snprintf(buf, sizeof(buf), "Audio device enumerator creation failed (HRESULT=0x%08X).",
                 static_cast<unsigned>(hr));
        result.passed = false;
        result.detail = buf;
        return result;
    }

    IMMDeviceCollection* pCollection = nullptr;
    hr = pEnumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &pCollection);
    pEnumerator->Release();
    pEnumerator = nullptr;

    if (FAILED(hr)) {
        if (uninit_on_exit)
            CoUninitialize();
        char buf[80];
        snprintf(buf, sizeof(buf), "Audio endpoint enumeration failed (HRESULT=0x%08X).", static_cast<unsigned>(hr));
        result.passed = false;
        result.detail = buf;
        return result;
    }

    UINT count = 0;
    hr = pCollection->GetCount(&count);
    pCollection->Release();
    pCollection = nullptr;

    if (uninit_on_exit)
        CoUninitialize();

    if (FAILED(hr)) {
        result.passed = false;
        result.detail = "Failed to count audio capture devices.";
        return result;
    }

    if (count > 0) {
        char buf[80];
        snprintf(buf, sizeof(buf), "%u active audio capture device(s) found.", static_cast<unsigned>(count));
        result.passed = true;
        result.detail = buf;
    } else {
        result.passed = false;
        result.detail = "No active audio capture devices found. Check that a microphone or audio device is "
                        "connected and enabled.";
    }
    return result;
}

} // namespace exosnap::diagnostics
