#pragma once

#include "DiagnosticResult.h"

#include <filesystem>
#include <string>

namespace exosnap::diagnostics {

struct SelfTestResult {
    std::string category;
    bool passed = false;
    std::string detail;
};

class SelfTestRunner {
  public:
    DiagnosticChecklist Run() const;

    static SelfTestResult CheckCaptureAvailability();
    static SelfTestResult CheckEncoderAvailability();
    static SelfTestResult CheckMuxerAvailability();
    static SelfTestResult CheckOutputPathWritable(const std::filesystem::path& path);
    static SelfTestResult CheckAudioDeviceAvailability();
};

} // namespace exosnap::diagnostics
