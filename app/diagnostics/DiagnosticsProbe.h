#pragma once

#include "DiagnosticsController.h"

#include <filesystem>

// Every blocking probe the Diagnostics surface needs, gathered into one call that
// is safe to run on a worker thread.
//
// The Qt Widgets DiagnosticsPage ran all of this on the GUI thread: a Win32 volume
// query on every data push, a real create/write/delete of a probe file twice per
// refresh (and at 2 Hz while recording), and a self-test that opens a DXGI factory,
// LoadLibraryW's the NVENC DLL, writes a temp file and enumerates COM audio
// endpoints. None of it belongs on the thread that paints.
namespace exosnap::diagnostics {

struct DiagnosticsProbeRequest {
    std::filesystem::path output_folder;
    // The self-test is comparatively expensive (COM + DXGI + LoadLibraryW), so it
    // only runs when the surface actually needs a fresh checklist.
    bool run_self_test = true;
};

// Blocking. Never call from the GUI thread.
[[nodiscard]] DiagnosticsController::ProbeResult RunDiagnosticsProbe(const DiagnosticsProbeRequest& request);

// The drive label shown on the Disk tile: the volume root ("C:") when the path has
// one, otherwise the path itself. Pure, so the tile text is unit-testable without
// touching a filesystem.
[[nodiscard]] std::string DriveLabelForPath(const std::filesystem::path& path);

} // namespace exosnap::diagnostics
