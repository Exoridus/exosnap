#pragma once
// update_service_interface.h -- UI seam for the update system.
//
// The engine (libs/update) is UI-agnostic per CLAUDE.md.  This interface
// defines the callbacks the UI layer must implement to receive update events.
// The concrete Qt implementation lives in app/services/UpdateService.h.

#include <string>
#include <update/update_types.h>

namespace exosnap::update {

struct IUpdateServiceObserver {
    virtual ~IUpdateServiceObserver() = default;

    // Called when the update check completes (success or failure).
    virtual void OnUpdateCheckComplete(const UpdateCheckResult& result) = 0;

    // Called when the running UpdateState changes (channel, block reason, etc.)
    virtual void OnUpdateStateChanged(const UpdateState& state) = 0;

    // Called when a package download completes and has been verified.
    // installer_path is the verified local file path, ready for handoff.
    virtual void OnPackageReadyForInstall(const std::string& installer_path) = 0;

    // Called when any update action fails (download error, hash mismatch, etc.)
    virtual void OnUpdateError(VerifyResult result, const std::string& detail) = 0;
};

} // namespace exosnap::update
