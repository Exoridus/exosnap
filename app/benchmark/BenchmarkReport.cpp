#include "BenchmarkReport.h"

#include "ExoSnapBuildInfo.h"

#include "services/DisplayIdentityEnumerator.h"

#include <QDateTime>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRect>
#include <QScreen>
#include <QSettings>
#include <QSysInfo>
#include <QtGlobal>

#include <algorithm>

#include <windows.h>

#include <dxgi1_2.h>
#include <psapi.h>
#include <wrl/client.h>

namespace exosnap::benchmark {
namespace {

using Microsoft::WRL::ComPtr;

QJsonObject MetricToJson(const Metric& metric) {
    QJsonObject obj;
    if (metric.value.has_value()) {
        obj.insert(QStringLiteral("value"), *metric.value);
    } else {
        // Explicitly null rather than absent: a reader must be able to tell
        // "this run could not measure it" from "this schema has no such field".
        obj.insert(QStringLiteral("value"), QJsonValue());
        obj.insert(QStringLiteral("unavailable"), true);
    }
    obj.insert(QStringLiteral("comparability"), QString::fromLatin1(ComparabilityName(metric.comparability)));
    if (!metric.probe.empty())
        obj.insert(QStringLiteral("probe"), QString::fromStdString(metric.probe));
    return obj;
}

QString ReadRegistryString(const QString& key, const QString& value) {
    QSettings settings(key, QSettings::NativeFormat);
    return settings.value(value).toString();
}

// User-mode driver version as the vendor control panels report it: DXGI encodes
// it as four 16-bit fields in a LARGE_INTEGER.
QString FormatUmdVersion(LARGE_INTEGER umd) {
    const auto part = [&](int shift) { return static_cast<quint16>((umd.QuadPart >> shift) & 0xFFFF); };
    return QStringLiteral("%1.%2.%3.%4").arg(part(48)).arg(part(32)).arg(part(16)).arg(part(0));
}

void CollectGpu(Environment* env) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.GetAddressOf()))))
        return;
    ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(0, adapter.GetAddressOf()) != S_OK)
        return;

    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc))) {
        env->gpu_description = QString::fromWCharArray(desc.Description);
        env->gpu_vendor_id = desc.VendorId;
        env->gpu_device_id = desc.DeviceId;
        env->gpu_dedicated_vram_mb = static_cast<uint64_t>(desc.DedicatedVideoMemory) / (1024ULL * 1024ULL);
    }

    LARGE_INTEGER umd{};
    if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
        env->gpu_driver_umd_version = FormatUmdVersion(umd);
}

void CollectDisplayIdentities(Environment* env) {
    for (const EnumeratedDisplayIdentity& identity : EnumerateDisplayIdentities()) {
        Environment::DisplayFacts facts;
        facts.gdi_name = QString::fromStdString(identity.id.gdi_name);
        facts.friendly_name = QString::fromStdString(identity.id.friendly_name);
        facts.device_path = QString::fromStdString(identity.id.device_path);
        facts.edid_vendor = QString::fromStdString(identity.id.edid_vendor);
        facts.serial = QString::fromStdString(identity.id.serial);
        facts.x = identity.rc_monitor_physical.left;
        facts.y = identity.rc_monitor_physical.top;
        facts.width = identity.rc_monitor_physical.width();
        facts.height = identity.rc_monitor_physical.height();
        // Win32's own definition, not an enumeration position: the primary monitor
        // is the one anchored at the virtual-desktop origin.
        facts.primary = identity.rc_monitor_physical.left == 0 && identity.rc_monitor_physical.top == 0;
        env->displays.append(facts);
    }
}

void CollectDisplay(Environment* env) {
    // The primary screen is the one every scenario in this campaign records; a
    // scenario that targets a different screen states so in its manifest.
    QScreen* screen = QGuiApplication::primaryScreen();
    if (screen == nullptr)
        return;
    env->display_name = screen->name();
    env->display_width = screen->size().width();
    env->display_height = screen->size().height();
    env->display_refresh_hz = screen->refreshRate();
    env->display_device_pixel_ratio = screen->devicePixelRatio();
}

uint64_t ProcessCpu100ns() {
    FILETIME creation{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exited, &kernel, &user) == FALSE)
        return 0;
    const auto value = [](FILETIME time) {
        return (static_cast<uint64_t>(time.dwHighDateTime) << 32U) | time.dwLowDateTime;
    };
    return value(kernel) + value(user);
}

} // namespace

const char* ComparabilityName(Comparability value) noexcept {
    switch (value) {
    case Comparability::Identical:
        return "identical";
    case Comparability::Approximate:
        return "approximate";
    case Comparability::FrontendOnly:
        return "frontend_only";
    }
    return "approximate";
}

Metric MakeMetric(double value, Comparability comparability, std::string probe) {
    Metric metric;
    metric.value = value;
    metric.comparability = comparability;
    metric.probe = std::move(probe);
    return metric;
}

Metric UnavailableMetric(Comparability comparability, std::string probe) {
    Metric metric;
    metric.comparability = comparability;
    metric.probe = std::move(probe);
    return metric;
}

const char* FrontendName(Frontend value) noexcept {
    switch (value) {
    case Frontend::Widgets:
        return "widgets";
    case Frontend::Quick:
        return "quick";
    case Frontend::Headless:
        return "headless";
    }
    return "unknown";
}

bool FrontendFromString(const QString& text, Frontend* out) {
    if (out == nullptr)
        return false;
    if (text.compare(QStringLiteral("widgets"), Qt::CaseInsensitive) == 0) {
        *out = Frontend::Widgets;
        return true;
    }
    if (text.compare(QStringLiteral("quick"), Qt::CaseInsensitive) == 0) {
        *out = Frontend::Quick;
        return true;
    }
    if (text.compare(QStringLiteral("headless"), Qt::CaseInsensitive) == 0) {
        *out = Frontend::Headless;
        return true;
    }
    return false;
}

Environment CollectEnvironment() {
    Environment env;

    env.app_version = QString::fromLatin1(exosnap::build::kVersion);
    env.git_commit = QString::fromLatin1(exosnap::build::kGitCommit);
    env.git_commit_full = QString::fromLatin1(exosnap::build::kGitCommitFull);
    env.dirty_source_tree = exosnap::build::kDirtySourceTree;
    env.official_build = exosnap::build::kOfficialBuild;
#if defined(EXOSNAP_BUILD_CONFIG)
    env.build_config = QString::fromLatin1(EXOSNAP_BUILD_CONFIG);
#else
    env.build_config = QStringLiteral("Unknown");
#endif

    env.qt_version_runtime = QString::fromLatin1(qVersion());
    env.qt_version_compiled = QString::fromLatin1(QT_VERSION_STR);

    env.os_version = QSysInfo::prettyProductName();
    static const QString kCurrentVersionKey =
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion");
    const QString display_version = ReadRegistryString(kCurrentVersionKey, QStringLiteral("DisplayVersion"));
    const QString build = ReadRegistryString(kCurrentVersionKey, QStringLiteral("CurrentBuild"));
    const QString ubr = ReadRegistryString(kCurrentVersionKey, QStringLiteral("UBR"));
    if (!build.isEmpty()) {
        env.os_edition = ReadRegistryString(kCurrentVersionKey, QStringLiteral("ProductName"));
        env.os_version = QStringLiteral("%1 (%2 build %3.%4)")
                             .arg(env.os_version, display_version, build, ubr.isEmpty() ? QStringLiteral("0") : ubr);
    }

    env.cpu_name =
        ReadRegistryString(QStringLiteral("HKEY_LOCAL_MACHINE\\HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0"),
                           QStringLiteral("ProcessorNameString"))
            .trimmed();
    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    env.cpu_logical_processors = static_cast<int>(system_info.dwNumberOfProcessors);

    CollectGpu(&env);
    CollectDisplay(&env);
    CollectDisplayIdentities(&env);

    env.timestamp = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    return env;
}

int CountChildWindows(void* top_level) {
    if (top_level == nullptr)
        return -1;
    int count = 0;
    // EnumChildWindows walks the whole descendant chain, not just direct children:
    // a native grandchild is exactly as capable of owning a pixel as a direct one.
    EnumChildWindows(
        static_cast<HWND>(top_level),
        [](HWND, LPARAM param) -> BOOL {
            ++*reinterpret_cast<int*>(param);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&count));
    return count;
}

HarnessWindowPlacement ResolveHarnessWindowPlacement() {
    HarnessWindowPlacement placement;
    QScreen* const primary = QGuiApplication::primaryScreen();
    QScreen* chosen = nullptr;
    for (QScreen* screen : QGuiApplication::screens()) {
        if (screen != primary) {
            chosen = screen;
            placement.on_secondary_screen = true;
            break;
        }
    }
    if (chosen == nullptr)
        chosen = primary;
    if (chosen == nullptr)
        return placement;

    const QRect available = chosen->availableGeometry();
    placement.x = available.x();
    placement.y = available.y();
    placement.screen_name = chosen->name();
    // Clamp rather than overflow a small secondary screen: a window hanging off
    // the edge is partially unrendered, and an unrendered frontend is not the
    // frontend cost the campaign claims to measure.
    placement.width = std::min(placement.width, available.width());
    placement.height = std::min(placement.height, available.height());
    return placement;
}

void ProcessSampler::Start() noexcept {
    cpu_100ns_at_start_ = ProcessCpu100ns();
    started_ = true;
}

ProcessMetrics ProcessSampler::Sample(double elapsed_seconds) const {
    ProcessMetrics metrics;
    const std::string cpu_probe =
        "Win32 GetProcessTimes kernel+user delta over the measured window, divided by logical processors";
    const std::string ws_probe = "Win32 GetProcessMemoryInfo at the end of the measured window";

    if (!started_ || elapsed_seconds <= 0.0) {
        metrics.cpu_percent = UnavailableMetric(Comparability::Identical, cpu_probe);
        metrics.working_set_mb = UnavailableMetric(Comparability::Identical, ws_probe);
        metrics.peak_working_set_mb = UnavailableMetric(Comparability::Identical, ws_probe);
        return metrics;
    }

    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    const uint64_t delta = ProcessCpu100ns() - cpu_100ns_at_start_;
    if (system_info.dwNumberOfProcessors > 0) {
        const double percent =
            static_cast<double>(delta) / (elapsed_seconds * 10'000'000.0 * system_info.dwNumberOfProcessors) * 100.0;
        metrics.cpu_percent = MakeMetric(percent, Comparability::Identical, cpu_probe);
    } else {
        metrics.cpu_percent = UnavailableMetric(Comparability::Identical, cpu_probe);
    }

    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) != FALSE) {
        constexpr double kMb = 1024.0 * 1024.0;
        metrics.working_set_mb =
            MakeMetric(static_cast<double>(counters.WorkingSetSize) / kMb, Comparability::Identical, ws_probe);
        metrics.peak_working_set_mb =
            MakeMetric(static_cast<double>(counters.PeakWorkingSetSize) / kMb, Comparability::Identical, ws_probe);
    } else {
        metrics.working_set_mb = UnavailableMetric(Comparability::Identical, ws_probe);
        metrics.peak_working_set_mb = UnavailableMetric(Comparability::Identical, ws_probe);
    }
    return metrics;
}

QString ArtifactBaseName(const RunConfig& config) {
    const QString scenario = config.scenario.isEmpty() ? QStringLiteral("unnamed") : config.scenario;
    return QStringLiteral("%1-%2-run%3")
        .arg(QString::fromLatin1(FrontendName(config.frontend)), scenario,
             QStringLiteral("%1").arg(config.run_index, 2, 10, QLatin1Char('0')));
}

bool WriteReport(const QString& path, const Environment& environment, const RunConfig& config,
                 const EffectiveRecordingConfig& effective, const RunOutcome& outcome, const PreviewMetrics& preview,
                 const RecordingMetrics& recording, const ProcessMetrics& process) {
    QJsonObject root;
    root.insert(QStringLiteral("schema"), QStringLiteral("exosnap.frontend-ab-benchmark/1"));

    QJsonObject run;
    run.insert(QStringLiteral("frontend"), QString::fromLatin1(FrontendName(config.frontend)));
    run.insert(QStringLiteral("scenario"), config.scenario);
    run.insert(QStringLiteral("run_index"), config.run_index);
    run.insert(QStringLiteral("repetitions"), config.repetitions);
    run.insert(QStringLiteral("warmup_seconds"), config.warmup_seconds);
    run.insert(QStringLiteral("measured_seconds"), config.measured_seconds);
    run.insert(QStringLiteral("artifact_base_name"), ArtifactBaseName(config));
    if (!config.source_notes.isEmpty())
        run.insert(QStringLiteral("source_notes"), config.source_notes);
    root.insert(QStringLiteral("run"), run);

    QJsonObject capture;
    capture.insert(QStringLiteral("target_kind"), config.capture_target_kind);
    capture.insert(QStringLiteral("target_description"), config.capture_target_description);
    capture.insert(QStringLiteral("requested_fps"), config.requested_fps);
    capture.insert(QStringLiteral("container"), config.container);
    capture.insert(QStringLiteral("video_codec"), config.video_codec);
    capture.insert(QStringLiteral("audio_codec"), config.audio_codec);
    capture.insert(QStringLiteral("chroma"), config.chroma);
    capture.insert(QStringLiteral("bit_depth"), config.bit_depth);
    capture.insert(QStringLiteral("hdr_mode"), config.hdr_mode);
    capture.insert(QStringLiteral("audio_rows"), config.audio_rows);
    root.insert(QStringLiteral("recording_config"), capture);

    // The block above is what was ASKED for; this one is what the engine got.
    // They are emitted side by side on purpose — a divergence between them is
    // itself a finding, and the comparison tool gates on the fingerprint here,
    // never on the requested values above.
    QJsonObject effective_config;
    effective_config.insert(QStringLiteral("available"), effective.available);
    if (effective.available) {
        effective_config.insert(QStringLiteral("fingerprint"), effective.fingerprint);
        QJsonArray fields;
        for (const QString& field : effective.fields)
            fields.append(field);
        effective_config.insert(QStringLiteral("fields"), fields);
    } else {
        effective_config.insert(QStringLiteral("reason"),
                                QStringLiteral("no recording session reached engine validation"));
    }
    root.insert(QStringLiteral("effective_recording_config"), effective_config);

    QJsonObject env;
    env.insert(QStringLiteral("app_version"), environment.app_version);
    env.insert(QStringLiteral("git_commit"), environment.git_commit);
    env.insert(QStringLiteral("git_commit_full"), environment.git_commit_full);
    env.insert(QStringLiteral("dirty_source_tree"), environment.dirty_source_tree);
    env.insert(QStringLiteral("official_build"), environment.official_build);
    env.insert(QStringLiteral("build_config"), environment.build_config);
    env.insert(QStringLiteral("qt_version_runtime"), environment.qt_version_runtime);
    env.insert(QStringLiteral("qt_version_compiled"), environment.qt_version_compiled);
    env.insert(QStringLiteral("os_version"), environment.os_version);
    env.insert(QStringLiteral("os_edition"), environment.os_edition);
    env.insert(QStringLiteral("cpu_name"), environment.cpu_name);
    env.insert(QStringLiteral("cpu_logical_processors"), environment.cpu_logical_processors);
    env.insert(QStringLiteral("gpu_description"), environment.gpu_description);
    env.insert(QStringLiteral("gpu_vendor_id"), static_cast<qint64>(environment.gpu_vendor_id));
    env.insert(QStringLiteral("gpu_device_id"), static_cast<qint64>(environment.gpu_device_id));
    env.insert(QStringLiteral("gpu_dedicated_vram_mb"), static_cast<qint64>(environment.gpu_dedicated_vram_mb));
    env.insert(QStringLiteral("gpu_driver_umd_version"), environment.gpu_driver_umd_version);
    env.insert(QStringLiteral("display_name"), environment.display_name);
    env.insert(QStringLiteral("display_width"), environment.display_width);
    env.insert(QStringLiteral("display_height"), environment.display_height);
    env.insert(QStringLiteral("display_refresh_hz"), environment.display_refresh_hz);
    env.insert(QStringLiteral("display_device_pixel_ratio"), environment.display_device_pixel_ratio);
    env.insert(QStringLiteral("timestamp"), environment.timestamp);

    QJsonArray displays;
    for (const Environment::DisplayFacts& facts : environment.displays) {
        QJsonObject entry;
        entry.insert(QStringLiteral("gdi_name"), facts.gdi_name);
        entry.insert(QStringLiteral("friendly_name"), facts.friendly_name);
        entry.insert(QStringLiteral("device_path"), facts.device_path);
        entry.insert(QStringLiteral("edid_vendor"), facts.edid_vendor);
        entry.insert(QStringLiteral("serial"), facts.serial);
        entry.insert(QStringLiteral("x"), facts.x);
        entry.insert(QStringLiteral("y"), facts.y);
        entry.insert(QStringLiteral("width"), facts.width);
        entry.insert(QStringLiteral("height"), facts.height);
        entry.insert(QStringLiteral("primary"), facts.primary);
        displays.append(entry);
    }
    env.insert(QStringLiteral("displays"), displays);
    root.insert(QStringLiteral("environment"), env);

    QJsonObject result;
    result.insert(QStringLiteral("succeeded"), outcome.succeeded);
    result.insert(QStringLiteral("output_path"), outcome.output_path);
    result.insert(QStringLiteral("output_file_bytes"), outcome.output_file_bytes);
    result.insert(QStringLiteral("media_duration_seconds"), outcome.media_duration_seconds);
    result.insert(QStringLiteral("output_width"), outcome.output_width);
    result.insert(QStringLiteral("output_height"), outcome.output_height);
    result.insert(QStringLiteral("error_phase"), outcome.error_phase);
    result.insert(QStringLiteral("error_detail"), outcome.error_detail);
    root.insert(QStringLiteral("outcome"), result);

    QJsonObject preview_json;
    preview_json.insert(QStringLiteral("frames_presented"), MetricToJson(preview.frames_presented));
    preview_json.insert(QStringLiteral("source_frames_consumed"), MetricToJson(preview.source_frames_consumed));
    preview_json.insert(QStringLiteral("mutex_misses"), MetricToJson(preview.mutex_misses));
    preview_json.insert(QStringLiteral("frame_cadence_fps"), MetricToJson(preview.frame_cadence_fps));
    preview_json.insert(QStringLiteral("frame_ms_p50"), MetricToJson(preview.frame_ms_p50));
    preview_json.insert(QStringLiteral("frame_ms_p95"), MetricToJson(preview.frame_ms_p95));
    preview_json.insert(QStringLiteral("frame_ms_p99"), MetricToJson(preview.frame_ms_p99));
    preview_json.insert(QStringLiteral("frame_ms_max"), MetricToJson(preview.frame_ms_max));
    preview_json.insert(QStringLiteral("source_delivery_fps"), MetricToJson(preview.source_delivery_fps));
    preview_json.insert(QStringLiteral("source_interval_ms_p95"), MetricToJson(preview.source_interval_ms_p95));
    preview_json.insert(QStringLiteral("source_interval_ms_p99"), MetricToJson(preview.source_interval_ms_p99));
    preview_json.insert(QStringLiteral("submit_us_p50"), MetricToJson(preview.submit_us_p50));
    preview_json.insert(QStringLiteral("submit_us_p95"), MetricToJson(preview.submit_us_p95));
    preview_json.insert(QStringLiteral("submit_us_p99"), MetricToJson(preview.submit_us_p99));
    preview_json.insert(QStringLiteral("child_hwnd_count"), MetricToJson(preview.child_hwnd_count));
    preview_json.insert(QStringLiteral("render_amplification"), MetricToJson(preview.render_amplification));
    preview_json.insert(QStringLiteral("preview_publish_signals"), MetricToJson(preview.preview_publish_signals));
    preview_json.insert(QStringLiteral("preview_scene_update_requests"),
                        MetricToJson(preview.preview_scene_update_requests));
    preview_json.insert(QStringLiteral("consumer_acquires"), MetricToJson(preview.consumer_acquires));
    preview_json.insert(QStringLiteral("consumer_acquire_abandoned"), MetricToJson(preview.consumer_acquire_abandoned));
    preview_json.insert(QStringLiteral("consumer_conversion_failures"),
                        MetricToJson(preview.consumer_conversion_failures));
    preview_json.insert(QStringLiteral("publish_interval_ms_p50"), MetricToJson(preview.publish_interval_ms_p50));
    preview_json.insert(QStringLiteral("publish_interval_ms_p95"), MetricToJson(preview.publish_interval_ms_p95));
    preview_json.insert(QStringLiteral("publish_interval_ms_p99"), MetricToJson(preview.publish_interval_ms_p99));
    preview_json.insert(QStringLiteral("publish_interval_ms_max"), MetricToJson(preview.publish_interval_ms_max));
    preview_json.insert(QStringLiteral("presentation_debt_ms_p50"), MetricToJson(preview.presentation_debt_ms_p50));
    preview_json.insert(QStringLiteral("presentation_debt_ms_p95"), MetricToJson(preview.presentation_debt_ms_p95));
    preview_json.insert(QStringLiteral("presentation_debt_ms_p99"), MetricToJson(preview.presentation_debt_ms_p99));
    preview_json.insert(QStringLiteral("presentation_debt_ms_max"), MetricToJson(preview.presentation_debt_ms_max));
    preview_json.insert(QStringLiteral("source_interval_ms_max"), MetricToJson(preview.source_interval_ms_max));
    root.insert(QStringLiteral("preview"), preview_json);

    QJsonObject recording_json;
    recording_json.insert(QStringLiteral("target_fps"), MetricToJson(recording.target_fps));
    recording_json.insert(QStringLiteral("actual_fps"), MetricToJson(recording.actual_fps));
    recording_json.insert(QStringLiteral("measured_window_emitted_fps"),
                          MetricToJson(recording.measured_window_emitted_fps));
    recording_json.insert(QStringLiteral("frames_captured"), MetricToJson(recording.frames_captured));
    recording_json.insert(QStringLiteral("frames_emitted"), MetricToJson(recording.frames_emitted));
    recording_json.insert(QStringLiteral("frames_duplicated"), MetricToJson(recording.frames_duplicated));
    recording_json.insert(QStringLiteral("frames_dropped_coalesced"), MetricToJson(recording.frames_dropped_coalesced));
    recording_json.insert(QStringLiteral("frames_dropped_cfr"), MetricToJson(recording.frames_dropped_cfr));
    recording_json.insert(QStringLiteral("frames_dropped_backpressure"),
                          MetricToJson(recording.frames_dropped_backpressure));
    recording_json.insert(QStringLiteral("frames_dropped_processing_failure"),
                          MetricToJson(recording.frames_dropped_processing_failure));
    recording_json.insert(QStringLiteral("frames_dropped_problematic"),
                          MetricToJson(recording.frames_dropped_problematic));
    recording_json.insert(QStringLiteral("acquire_average_ms"), MetricToJson(recording.acquire_average_ms));
    recording_json.insert(QStringLiteral("acquire_peak_ms"), MetricToJson(recording.acquire_peak_ms));
    recording_json.insert(QStringLiteral("encoder_queue_depth"), MetricToJson(recording.encoder_queue_depth));
    recording_json.insert(QStringLiteral("encoder_latency_ms"), MetricToJson(recording.encoder_latency_ms));
    recording_json.insert(QStringLiteral("mux_queue_depth"), MetricToJson(recording.mux_queue_depth));
    recording_json.insert(QStringLiteral("audio_frames_dropped"), MetricToJson(recording.audio_frames_dropped));
    recording_json.insert(QStringLiteral("av_drift_ms"), MetricToJson(recording.av_drift_ms));
    recording_json.insert(QStringLiteral("preview_tap_frames_seen"), MetricToJson(recording.preview_tap_frames_seen));
    recording_json.insert(QStringLiteral("preview_tap_gate_passes"), MetricToJson(recording.preview_tap_gate_passes));
    recording_json.insert(QStringLiteral("preview_tap_shared_texture_ready"),
                          MetricToJson(recording.preview_tap_shared_texture_ready));
    recording_json.insert(QStringLiteral("preview_tap_publish_attempts"),
                          MetricToJson(recording.preview_tap_publish_attempts));
    recording_json.insert(QStringLiteral("preview_tap_publish_successes"),
                          MetricToJson(recording.preview_tap_publish_successes));
    recording_json.insert(QStringLiteral("preview_tap_publish_mutex_misses"),
                          MetricToJson(recording.preview_tap_publish_mutex_misses));
    recording_json.insert(QStringLiteral("preview_tap_publish_abandoned"),
                          MetricToJson(recording.preview_tap_publish_abandoned));
    recording_json.insert(QStringLiteral("preview_tap_publish_failures"),
                          MetricToJson(recording.preview_tap_publish_failures));
    recording_json.insert(QStringLiteral("preview_tap_publish_release_failures"),
                          MetricToJson(recording.preview_tap_publish_release_failures));
    recording_json.insert(QStringLiteral("preview_tap_published_edges"),
                          MetricToJson(recording.preview_tap_published_edges));
    root.insert(QStringLiteral("recording"), recording_json);

    QJsonObject process_json;
    process_json.insert(QStringLiteral("cpu_percent"), MetricToJson(process.cpu_percent));
    process_json.insert(QStringLiteral("working_set_mb"), MetricToJson(process.working_set_mb));
    process_json.insert(QStringLiteral("peak_working_set_mb"), MetricToJson(process.peak_working_set_mb));
    root.insert(QStringLiteral("process"), process_json);

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(QJsonDocument(root).toJson()) >= 0;
}

} // namespace exosnap::benchmark
