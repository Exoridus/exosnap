#include "diagnostics/SupportBundle.h"

#include "diagnostics/ZipWriter.h"

#include <crash_capture/crash_scrubber.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace exosnap::diagnostics {
namespace {

const QString kCaptureTargetPlaceholder = QStringLiteral("[capture-target]");

QByteArray ReadFileBytes(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

// Append the file at <log_dir>/<name>, scrubbed, as bundle entry <name> if it exists.
void AddScrubbedFileIfPresent(const QDir& dir, const QString& name, std::vector<BundleEntry>& out) {
    const QString path = dir.filePath(name);
    if (!QFileInfo::exists(path))
        return;
    out.push_back({name, ScrubBundleText(ReadFileBytes(path))});
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

    // Text logs (current + rotated) — exosnap.log[.1][.2].
    AddScrubbedFileIfPresent(dir, QStringLiteral("exosnap.log"), entries);
    AddScrubbedFileIfPresent(dir, QStringLiteral("exosnap.log.1"), entries);
    AddScrubbedFileIfPresent(dir, QStringLiteral("exosnap.log.2"), entries);

    // Engine JSONL (current + rotated) — spdlog names backups engine.1.jsonl etc.
    AddScrubbedFileIfPresent(dir, QStringLiteral("engine.jsonl"), entries);
    AddScrubbedFileIfPresent(dir, QStringLiteral("engine.1.jsonl"), entries);
    AddScrubbedFileIfPresent(dir, QStringLiteral("engine.2.jsonl"), entries);

    // Last N session reports (newest first), scrubbed defensively.
    {
        const QDir reports_dir(dir.filePath(QStringLiteral("reports")));
        if (reports_dir.exists()) {
            const QFileInfoList reports =
                reports_dir.entryInfoList({QStringLiteral("session-*.json")}, QDir::Files, QDir::Time);
            const int limit = inputs.max_reports > 0 ? inputs.max_reports : reports.size();
            for (int i = 0; i < reports.size() && i < limit; ++i) {
                entries.push_back({QStringLiteral("reports/%1").arg(reports[i].fileName()),
                                   ScrubBundleText(ReadFileBytes(reports[i].absoluteFilePath()))});
            }
        }
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
        manifest[QStringLiteral("note")] =
            QStringLiteral("No telemetry. Created locally on user action; not transmitted anywhere.");
        manifest[QStringLiteral("privacy")] =
            QStringLiteral("Paths, username and machine name are scrubbed; capture-target window "
                           "titles are redacted to [capture-target].");
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
