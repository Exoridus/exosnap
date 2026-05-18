#pragma once

#include <cstdint>
#include <string>

namespace exosnap::capability {

struct NvidiaRuntimeFacts {
    bool nvenc_dll_present = false;
    bool nvenc_api_version_valid = false;
    uint32_t nvenc_api_version = 0;
    std::string adapter_name;
    std::string failure_detail;
};

struct MfAacRuntimeFacts {
    bool mftenum_found = false;
    bool clsid_instantiable = false;
    std::string failure_detail;

    bool available() const noexcept {
        return mftenum_found || clsid_instantiable;
    }
};

struct OsRuntimeFacts {
    uint32_t build_number = 0;
    std::string version_string;
    std::string failure_detail;
};

struct RuntimeCapabilitySnapshot {
    NvidiaRuntimeFacts nvidia;
    MfAacRuntimeFacts mf_aac;
    OsRuntimeFacts os;
};

} // namespace exosnap::capability
