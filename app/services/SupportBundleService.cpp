#include "SupportBundleService.h"

#include "ExoSnapBuildInfo.h"

#include <capability/adapter_enum.h>

#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>
#include <QThread>

#include <utility>

namespace exosnap {
namespace {

QString VendorName(capability::AdapterVendor vendor) {
    switch (vendor) {
    case capability::AdapterVendor::Nvidia:
        return QStringLiteral("NVIDIA");
    case capability::AdapterVendor::Amd:
        return QStringLiteral("AMD");
    case capability::AdapterVendor::Intel:
        return QStringLiteral("Intel");
    case capability::AdapterVendor::Other:
        break;
    }
    return QStringLiteral("Other");
}

QString KindName(capability::AdapterKind kind) {
    switch (kind) {
    case capability::AdapterKind::Discrete:
        return QStringLiteral("discrete");
    case capability::AdapterKind::Integrated:
        return QStringLiteral("integrated");
    case capability::AdapterKind::Unknown:
        break;
    }
    return QStringLiteral("unknown");
}

} // namespace

diagnostics::BundleInputs BuildSupportBundleInputs(const SupportBundleContext& context,
                                                   const capability::CapabilitySet& caps,
                                                   const std::vector<capability::AdapterInfo>& adapters) {
    diagnostics::BundleInputs inputs;
    inputs.log_dir = context.log_dir;
    inputs.max_reports = context.max_reports;
    inputs.launch_session_id = context.launch_session_id;
    inputs.created_at = context.created_at;
    inputs.scrubber_version = QStringLiteral("1");
    inputs.app_version = QString::fromLatin1(build::kVersion);
    inputs.commit_sha = QString::fromLatin1(build::kGitCommit);
    inputs.verify_update_reinstall = context.verify_update_reinstall;
    inputs.settings_summary = context.settings_summary;
    // ADR 0044 promises startup-trace.txt in the bundle. The trace is a
    // process-global singleton, so it is read here at the boundary rather than
    // inside the collector, which stays a pure function of its inputs.
    inputs.startup_trace = diagnostics::StartupTrace::instance().entries();

    const auto& rt = caps.runtime;
    inputs.capability.gpu_adapter_name = QString::fromStdString(caps.gpu_adapter_name);
    inputs.capability.nvenc_dll_present = rt.nvidia.nvenc_dll_present;
    inputs.capability.nvenc_api_version =
        rt.nvidia.nvenc_api_version_valid ? QString::number(rt.nvidia.nvenc_api_version) : QString();
    inputs.capability.nvenc_av1 = rt.nvidia.nvenc_av1;
    inputs.capability.nvenc_hevc = rt.nvidia.nvenc_hevc;
    inputs.capability.nvenc_h264 = rt.nvidia.nvenc_h264;
    inputs.capability.nvenc_444 = rt.nvidia.nvenc_yuv444_h264 || rt.nvidia.nvenc_yuv444_hevc;
    inputs.capability.os_version_string = QString::fromStdString(rt.os.version_string);
    inputs.capability.os_build_number = QString::number(rt.os.build_number);
    inputs.capability.mf_webcam = rt.mf_webcam.available;

    for (const auto& adapter : adapters) {
        diagnostics::BundleAdapter entry;
        entry.name = QString::fromStdString(adapter.name);
        entry.vendor = VendorName(adapter.vendor);
        entry.kind = KindName(adapter.kind);
        entry.vendor_id = adapter.vendor_id;
        entry.device_id = adapter.device_id;
        entry.dedicated_vram_bytes = adapter.dedicated_video_memory_bytes;
        inputs.adapters.push_back(std::move(entry));
    }

    for (const auto& display : rt.displays) {
        diagnostics::BundleDisplay entry;
        entry.name = QString::fromStdString(display.name);
        entry.hdr_active = display.hdr_active;
        entry.bits_per_color = display.bits_per_color;
        entry.min_luminance = display.min_luminance_nits;
        entry.max_luminance = display.max_luminance_nits;
        entry.max_full_frame_luminance = display.max_full_frame_nits;
        inputs.displays.push_back(std::move(entry));
    }

    return inputs;
}

SupportBundleService::SupportBundleService(QObject* parent) : QObject(parent) {
}

bool SupportBundleService::busy() const noexcept {
    return busy_;
}

void SupportBundleService::createAsync(const QString& zip_path, SupportBundleContext context,
                                       capability::CapabilitySet caps) {
    if (busy_) {
        emit finished(false, QStringLiteral("A support bundle is already being created."));
        return;
    }
    if (zip_path.isEmpty()) {
        emit finished(false, QStringLiteral("No destination was chosen."));
        return;
    }
    if (context.log_dir.isEmpty()) {
        emit finished(false, QStringLiteral("No log directory is available yet."));
        return;
    }

    busy_ = true;
    emit busyChanged();

    // The work runs on a pool the service owns, and the result is marshalled back
    // through the application object so the QPointer check happens on the thread
    // that would destroy this service. The pool is a member rather than a
    // detached thread specifically so teardown waits for it — see the header.
    QPointer<SupportBundleService> guard(this);
    write_pool_.start([guard, zip_path, context = std::move(context), caps = std::move(caps)]() {
        const diagnostics::BundleInputs inputs =
            BuildSupportBundleInputs(context, caps, capability::EnumerateAdapters());
        const auto entries = diagnostics::CollectBundleEntries(inputs);
        QString error;
        const bool ok = diagnostics::WriteBundleZip(zip_path, entries, &error);
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [guard, ok, zip_path, error]() {
                if (!guard)
                    return;
                guard->busy_ = false;
                emit guard->busyChanged();
                emit guard->finished(ok, ok ? zip_path : error);
            },
            Qt::QueuedConnection);
    });
}

} // namespace exosnap
