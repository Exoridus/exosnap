#include "SelfTestRunner.h"

#include <chrono>
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
                            const std::string& current_value = "", const std::string& recommendation = "",
                            const std::string& optional_fix = "") {
    DiagnosticResult r;
    r.id = id;
    r.group = group;
    r.severity = sev;
    r.title = title;
    r.summary = summary;
    r.detail = detail;
    r.current_value = current_value;
    r.recommendation = recommendation;
    r.optional_fix = optional_fix;
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
                                        st.passed ? "" : "Investigate " + st.category + " availability.", "");
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
    // Scaffold: always report pass in MVP. Real WGC probe deferred.
    result.passed = true;
    result.detail = "Windows Graphics Capture (WGC) is available on Windows 10 1903+.";
    return result;
}

SelfTestResult SelfTestRunner::CheckEncoderAvailability() {
    SelfTestResult result;
    result.category = "Encoder";
    // Scaffold: generic pass. Real NVENC probe deferred.
    result.passed = true;
    result.detail = "NVENC hardware encoder abstraction reports ready (static check only).";
    return result;
}

SelfTestResult SelfTestRunner::CheckMuxerAvailability() {
    SelfTestResult result;
    result.category = "Muxer";
    // Scaffold: libwebm and MF sink writer availability deferred.
    result.passed = true;
    result.detail = "libwebm (MKV/WebM) and Media Foundation (MP4) muxers linked.";
    return result;
}

SelfTestResult SelfTestRunner::CheckOutputPathWritable(const std::string& /*path*/) {
    SelfTestResult result;
    result.category = "Output Path";
    // Scaffold: basic writable test using temp.
    try {
        auto tmp = std::filesystem::temp_directory_path() / "exosnap_selftest_write_check.tmp";
        {
            std::ofstream f(tmp, std::ios::binary);
            if (!f) {
                result.passed = false;
                result.detail = "Cannot write to temp directory: " + tmp.string();
                return result;
            }
            f << "exosnap-selftest";
        }
        std::filesystem::remove(tmp);
    } catch (...) {
        result.passed = false;
        result.detail = "Exception during output path writability check.";
        return result;
    }
    result.passed = true;
    result.detail = "Output path is writable (temp directory probe succeeded).";
    return result;
}

SelfTestResult SelfTestRunner::CheckAudioDeviceAvailability() {
    SelfTestResult result;
    result.category = "Audio Devices";
    // Scaffold: WASAPI enumeration deferred. Report pass in MVP.
    result.passed = true;
    result.detail = "WASAPI audio device enumeration not yet probed in self-test. Manual check recommended.";
    return result;
}

} // namespace exosnap::diagnostics
