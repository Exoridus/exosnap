#include "DiagnosticsProbe.h"

#include "DiskSpaceProvider.h"
#include "FilesystemProvider.h"
#include "SelfTestRunner.h"

#include <QStorageInfo>
#include <QString>

namespace exosnap::diagnostics {

std::string DriveLabelForPath(const std::filesystem::path& path) {
    std::string drive = path.root_name().string();
    if (drive.empty())
        drive = path.string();
    return drive;
}

DiagnosticsController::ProbeResult RunDiagnosticsProbe(const DiagnosticsProbeRequest& request) {
    DiagnosticsController::ProbeResult result;

    {
        Win32DiskSpaceProvider provider;
        result.free_bytes = provider.FreeBytesForPath(request.output_folder);
    }
    {
        Win32FilesystemProvider provider;
        result.filesystem_name = provider.FilesystemNameForPath(request.output_folder);
    }

    // Volume total drives the Disk tile's usage bar only. A volume Qt cannot read
    // leaves the bar hidden rather than drawing a fabricated 0 %.
    const QStorageInfo storage(QString::fromStdWString(request.output_folder.wstring()));
    if (storage.isValid() && storage.bytesTotal() > 0)
        result.total_bytes = static_cast<uint64_t>(storage.bytesTotal());

    result.drive_label = DriveLabelForPath(request.output_folder);
    result.output_path_writable = SelfTestRunner::CheckOutputPathWritable(request.output_folder.string()).passed;

    if (request.run_self_test) {
        SelfTestRunner runner;
        result.self_test = runner.Run();
        result.self_test_valid = true;
    }

    return result;
}

} // namespace exosnap::diagnostics
