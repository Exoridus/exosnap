#pragma once

#include <mfapi.h>

// ---------------------------------------------------------------------------
// CV-BUG-005: RAII guard around MFStartup/MFShutdown.
//
// MFShutdown must only be called if the matching MFStartup actually
// succeeded -- calling it unconditionally after a failed (or never-issued)
// MFStartup is a mismatched-lifetime bug of the same shape as the COM
// apartment issue in com_apartment.h.
// ---------------------------------------------------------------------------

namespace exosnap::engine {

class MfSession final {
  public:
    explicit MfSession(ULONG version = MF_VERSION, DWORD flags = MFSTARTUP_NOSOCKET) noexcept
        : result_(MFStartup(version, flags)) {
    }

    MfSession(const MfSession&) = delete;
    MfSession& operator=(const MfSession&) = delete;
    MfSession(MfSession&&) = delete;
    MfSession& operator=(MfSession&&) = delete;

    ~MfSession() {
        if (SUCCEEDED(result_)) {
            MFShutdown();
        }
    }

    [[nodiscard]] HRESULT result() const noexcept {
        return result_;
    }
    [[nodiscard]] bool usable() const noexcept {
        return SUCCEEDED(result_);
    }

  private:
    HRESULT result_;
};

} // namespace exosnap::engine
