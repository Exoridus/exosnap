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
    // Scaffold only: no runtime probe executed yet.
    result.passed = false;
    result.detail = "Capture self-test probe not executed in this build.";
    return result;
}

SelfTestResult SelfTestRunner::CheckEncoderAvailability() {
    SelfTestResult result;
    result.category = "Encoder";
    // Scaffold only: no runtime probe executed yet.
    result.passed = false;
    result.detail = "Encoder self-test probe not executed in this build.";
    return result;
}

SelfTestResult SelfTestRunner::CheckMuxerAvailability() {
    SelfTestResult result;
    result.category = "Muxer";
    // Scaffold only: no runtime probe executed yet.
    result.passed = false;
    result.detail = "Muxer self-test probe not executed in this build.";
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
    // Scaffold only: no runtime probe executed yet.
    result.passed = false;
    result.detail = "Audio device self-test probe not executed in this build.";
    return result;
}

} // namespace exosnap::diagnostics
