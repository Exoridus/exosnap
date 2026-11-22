#include "diagnostics/SupportBundle.h"

#include "diagnostics/SessionReportJson.h"
#include "diagnostics/ZipWriter.h"

#include <crash_capture/crash_scrubber.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>

#include <utility>

namespace exosnap::diagnostics {
namespace {

const QString kCaptureTargetPlaceholder = QStringLiteral("[capture-target]");

QByteArray ReadFileBytes(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

bool IsLogLevel(const QString& level) {
    static const QStringList levels{QStringLiteral("trace"),   QStringLiteral("debug"),   QStringLiteral("info"),
                                    QStringLiteral("warn"),    QStringLiteral("warning"), QStringLiteral("error"),
                                    QStringLiteral("critical")};
    return levels.contains(level);
}

bool IsKnownComponent(const QString& component) {
    static const QStringList components{QStringLiteral("app"),
                                        QStringLiteral("audio"),
                                        QStringLiteral("capture"),
                                        QStringLiteral("diagnostics"),
                                        QStringLiteral("encoder"),
                                        QStringLiteral("engine"),
                                        QStringLiteral("mux"),
                                        QStringLiteral("record"),
                                        QStringLiteral("record.failure"),
                                        QStringLiteral("recording"),
                                        QStringLiteral("remux"),
                                        QStringLiteral("split"),
                                        QStringLiteral("startup"),
                                        QStringLiteral("storage"),
                                        QStringLiteral("video"),
                                        QStringLiteral("recording.artifacts")};
    return components.contains(component);
}

QByteArray ExportAppLogLine(const QByteArray& line) {
    static const QRegularExpression prefix(
        QStringLiteral(R"(^(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}) \[([a-z]+)\](?: \[([a-z._]+)\])?(?: |$))"));
    const auto match = prefix.match(QString::fromUtf8(line));
    if (!match.hasMatch() || !IsLogLevel(match.captured(2)))
        return {};
    QString result = match.captured(1) + QStringLiteral(" [") + match.captured(2) + QStringLiteral("]");
    const auto component = match.captured(3);
    if (IsKnownComponent(component))
        result += QStringLiteral(" [") + component + QStringLiteral("]");
    return result.toUtf8();
}

QByteArray ExportEngineLogLine(const QByteArray& line) {
    const auto document = QJsonDocument::fromJson(line);
    if (!document.isObject())
        return {};
    const auto record = document.object();
    const auto timestamp = record.value(QStringLiteral("timestamp_unix_ms")).toInteger(-1);
    const auto level = record.value(QStringLiteral("level")).toString();
    if (timestamp < 0 || !IsLogLevel(level))
        return {};

    QJsonObject result{{QStringLiteral("timestamp_unix_ms"), timestamp}, {QStringLiteral("level"), level}};
    const auto component = record.value(QStringLiteral("component")).toString();
    if (IsKnownComponent(component))
        result.insert(QStringLiteral("component"), component);
    const auto backend = record.value(QStringLiteral("fields")).toObject().value(QStringLiteral("backend")).toString();
    if (backend == QStringLiteral("wgc") || backend == QStringLiteral("dxgi"))
        result.insert(QStringLiteral("fields"), QJsonObject{{QStringLiteral("backend"), backend}});
    return QJsonDocument(result).toJson(QJsonDocument::Compact);
}

void AddProjectedLog(const QDir& dir, const QString& name, QByteArray (*project)(const QByteArray&),
                     std::vector<BundleEntry>& out) {
    QFile source(dir.filePath(name));
    if (!source.open(QIODevice::ReadOnly))
        return;
    QByteArray exported;
    while (!source.atEnd()) {
        const auto projected = project(source.readLine().trimmed());
        if (!projected.isEmpty())
            exported += projected + '\n';
    }
    if (!exported.isEmpty())
        out.push_back({name, std::move(exported)});
}

QJsonObject BuildCapabilityJson(const BundleCapability& c) {
    QJsonObject gpu;
    gpu[QStringLiteral("adapter_name")] = c.gpu_adapter_name;
    gpu[QStringLiteral("nvenc_dll_present")] = c.nvenc_dll_present;
    gpu[QStringLiteral("nvenc_api_version")] = c.nvenc_api_version;
    gpu[QStringLiteral("nvenc_av1")] = c.nvenc_av1;
    gpu[QStringLiteral("nvenc_hevc")] = c.nvenc_hevc;
    gpu[QStringLiteral("nvenc_h264")] = c.nvenc_h264;
    gpu[QStringLiteral("nvenc_444")] = c.nvenc_444;

    QJsonObject os;
    os[QStringLiteral("version")] = c.os_version_string;
    os[QStringLiteral("build")] = c.os_build_number;

    QJsonObject mf;
    mf[QStringLiteral("webcam")] = c.mf_webcam;

    QJsonObject root;
    root[QStringLiteral("gpu")] = gpu;
    root[QStringLiteral("os")] = os;
    root[QStringLiteral("media_foundation")] = mf;
    return root;
}

QByteArray ToJsonBytes(const QJsonObject& obj) {
    return QJsonDocument(obj).toJson(QJsonDocument::Indented);
}

QByteArray ToJsonBytes(const QJsonArray& arr) {
    return QJsonDocument(arr).toJson(QJsonDocument::Indented);
}

} // namespace

QString RedactCaptureTargets(const QString& text) {
    QString out = text;
    // Freetext: target="<title>"  (e.g. the RecordingCoordinator "start … target=…" line).
    static const QRegularExpression kFreetext(QStringLiteral("target=\"[^\"]*\""));
    out.replace(kFreetext, QStringLiteral("target=\"%1\"").arg(kCaptureTargetPlaceholder));
    // JSONL: "target":"<title>" or "target": "<title>".
    static const QRegularExpression kJson(QStringLiteral("\"target\"\\s*:\\s*\"[^\"]*\""));
    out.replace(kJson, QStringLiteral("\"target\":\"%1\"").arg(kCaptureTargetPlaceholder));
    return out;
}

QByteArray ScrubBundleText(const QByteArray& text) {
    const std::string scrubbed =
        crash_capture::ScrubString(std::string(text.constData(), static_cast<size_t>(text.size())));
    const QString redacted = RedactCaptureTargets(QString::fromStdString(scrubbed));
    return redacted.toUtf8();
}

std::vector<BundleEntry> CollectBundleEntries(const BundleInputs& inputs) {
    std::vector<BundleEntry> entries;
    const QDir dir(inputs.log_dir);

    // Multiline messages and JSON strings can carry arbitrary private data.
    // Export only known metadata rather than rewriting the source text.
    AddProjectedLog(dir, QStringLiteral("exosnap.log"), ExportAppLogLine, entries);
    AddProjectedLog(dir, QStringLiteral("exosnap.log.1"), ExportAppLogLine, entries);
    AddProjectedLog(dir, QStringLiteral("exosnap.log.2"), ExportAppLogLine, entries);

    AddProjectedLog(dir, QStringLiteral("engine.jsonl"), ExportEngineLogLine, entries);
    AddProjectedLog(dir, QStringLiteral("engine.1.jsonl"), ExportEngineLogLine, entries);
    AddProjectedLog(dir, QStringLiteral("engine.2.jsonl"), ExportEngineLogLine, entries);

    // Last N session reports (newest first).
    {
        const QDir reports_dir(dir.filePath(QStringLiteral("reports")));
        if (reports_dir.exists()) {
            const QFileInfoList reports =
                reports_dir.entryInfoList({QStringLiteral("session-*.json")}, QDir::Files, QDir::Time);
            const int limit = inputs.max_reports > 0 ? inputs.max_reports : reports.size();
            for (int i = 0; i < reports.size() && i < limit; ++i) {
                const auto report = ReadSessionReportJson(ReadFileBytes(reports[i].absoluteFilePath()));
                if (!report.isNull())
                    entries.push_back({QStringLiteral("reports/%1").arg(reports[i].fileName()), report.toJson()});
            }
        }
    }

    // startup-trace.txt. Milestone labels and millisecond readings
    // only — no paths, no identifiers — so it needs no scrubbing. Omitted rather
    // than written empty when the process recorded nothing, because an empty
    // table reads as "startup produced no milestones" instead of "this build
    // never wired the trace up".
    if (!inputs.startup_trace.empty()) {
        entries.push_back({QStringLiteral("startup-trace.txt"), FormatStartupTrace(inputs.startup_trace).toUtf8()});
    }

    // capability.json
    entries.push_back({QStringLiteral("capability.json"), ToJsonBytes(BuildCapabilityJson(inputs.capability))});

    // adapters.json
    {
        QJsonArray arr;
        for (const auto& a : inputs.adapters) {
            QJsonObject o;
            o[QStringLiteral("name")] = a.name;
            o[QStringLiteral("vendor")] = a.vendor;
            o[QStringLiteral("kind")] = a.kind;
            o[QStringLiteral("vendor_id")] = static_cast<double>(a.vendor_id);
            o[QStringLiteral("device_id")] = static_cast<double>(a.device_id);
            o[QStringLiteral("dedicated_vram_bytes")] = static_cast<double>(a.dedicated_vram_bytes);
            o[QStringLiteral("driver_version")] = a.driver_version;
            arr.append(o);
        }
        entries.push_back({QStringLiteral("adapters.json"), ToJsonBytes(arr)});
    }

    // displays.json
    {
        QJsonArray arr;
        for (const auto& d : inputs.displays) {
            QJsonObject o;
            o[QStringLiteral("name")] = d.name;
            o[QStringLiteral("hdr_active")] = d.hdr_active;
            o[QStringLiteral("bits_per_color")] = static_cast<double>(d.bits_per_color);
            o[QStringLiteral("min_luminance")] = d.min_luminance;
            o[QStringLiteral("max_luminance")] = d.max_luminance;
            o[QStringLiteral("max_full_frame_luminance")] = d.max_full_frame_luminance;
            arr.append(o);
        }
        entries.push_back({QStringLiteral("displays.json"), ToJsonBytes(arr)});
    }

    // settings.txt — the ConfigSummary text, scrubbed (never a raw settings.ini dump).
    entries.push_back({QStringLiteral("settings.txt"), ScrubBundleText(inputs.settings_summary.toUtf8())});

    // bundle-manifest.json — created last so it can list every included file.
    {
        QJsonObject manifest;
        manifest[QStringLiteral("created_at")] = inputs.created_at;
        manifest[QStringLiteral("app_version")] = inputs.app_version;
        manifest[QStringLiteral("channel")] = inputs.channel;
        manifest[QStringLiteral("commit")] = inputs.commit_sha;
        manifest[QStringLiteral("launch_session_id")] = inputs.launch_session_id;
        manifest[QStringLiteral("scrubber_version")] = inputs.scrubber_version;
        // Only present while the mode is on — a normal bundle carries no such key.
        if (inputs.verify_update_reinstall)
            manifest[QStringLiteral("verify_update_reinstall")] = true;
        manifest[QStringLiteral("note")] =
            QStringLiteral("No telemetry. Created locally on user action; not transmitted anywhere.");
        manifest[QStringLiteral("privacy")] = QStringLiteral(
            "Logs contain allowlisted metadata. Free-form log messages and arbitrary fields are omitted.");
        QJsonArray files;
        for (const auto& e : entries)
            files.append(e.name);
        manifest[QStringLiteral("files")] = files;
        entries.push_back({QStringLiteral("bundle-manifest.json"), ToJsonBytes(manifest)});
    }

    return entries;
}

bool WriteBundleZip(const QString& zip_path, const std::vector<BundleEntry>& entries, QString* out_error) {
    ZipWriter zip;
    for (const auto& e : entries) {
        if (!zip.AddFileFromMemory(e.name.toStdString(), e.bytes.constData(),
                                   static_cast<std::size_t>(e.bytes.size()))) {
            if (out_error)
                *out_error = QStringLiteral("failed to add bundle entry: %1").arg(e.name);
            return false;
        }
    }
    if (!zip.WriteToFile(zip_path.toStdWString())) {
        if (out_error)
            *out_error = QStringLiteral("failed to write bundle archive");
        return false;
    }
    return true;
}

} // namespace exosnap::diagnostics
