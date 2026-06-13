#include "RecoveryService.h"

#include <recorder_core/mp4_remuxer.h>

#include <QDir>
#include <QFileInfo>

#include "diagnostics/AppLog.h"

namespace exosnap {
namespace {

// Build a no-op or wrapping RemuxProgressCallback from the caller's bool-returning lambda.
recorder_core::RemuxProgressCallback WrapProgress(std::function<bool(float)> cb) {
    if (!cb)
        return recorder_core::RemuxNoopCallback();
    return [cb = std::move(cb)](float f) -> bool { return cb(f); };
}

// Derive a stem name for the output file from the manifest entry.
// Prefer the final_output_path stem (the recording's intended filename without extension);
// fall back to the artefact stem with ".tmp" stripped.
std::wstring DeriveStemFromEntry(const RecoveryManifestEntry& entry) {
    if (!entry.final_output_path.isEmpty()) {
        std::filesystem::path p(entry.final_output_path.toStdWString());
        // Strip double-extension for .mkv.tmp artefacts used as final_output_path proxy.
        if (p.extension() == L".tmp")
            p.replace_extension(L"");
        return p.stem().wstring();
    }
    std::filesystem::path artefact(entry.artefact_path.toStdWString());
    if (artefact.extension() == L".tmp")
        artefact.replace_extension(L"");
    return artefact.stem().wstring();
}

} // namespace

RecoveryService::RecoveryService(RecoveryManifestStore& store) : store_(store) {
}

void RecoveryService::SetFallbackOutputFolder(const QString& folder) {
    fallback_output_folder_ = folder;
}

// Resolve the destination folder from the manifest entry with fallback logic:
//   1. Use the stored folder (parent of final_output_path) if it exists.
//   2. Fall back to fallback_output_folder_ if set and the directory exists.
//   3. Last resort: artefact parent directory.
std::filesystem::path RecoveryService::ResolveDestinationFolder(const RecoveryManifestEntry& entry) const {
    // (1) Prefer the folder recorded in the manifest.
    if (!entry.final_output_path.isEmpty()) {
        const std::filesystem::path stored_dir =
            std::filesystem::path(entry.final_output_path.toStdWString()).parent_path();
        if (!stored_dir.empty() && std::filesystem::exists(stored_dir))
            return stored_dir;
    }

    // (2) Configured fallback (current output directory from settings).
    if (!fallback_output_folder_.isEmpty()) {
        const std::filesystem::path fallback(fallback_output_folder_.toStdWString());
        if (!fallback.empty() && std::filesystem::exists(fallback))
            return fallback;
    }

    // (3) Artefact parent as last resort.
    return std::filesystem::path(entry.artefact_path.toStdWString()).parent_path();
}

QVector<RecoveryCandidate> RecoveryService::Scan() {
    auto entries = store_.Load();
    QVector<RecoveryManifestEntry> surviving;
    surviving.reserve(entries.size());

    for (const auto& e : entries) {
        if (e.artefact_path.isEmpty() || !QFileInfo::exists(e.artefact_path)) {
            // Orphaned entry — artefact is gone; remove silently.
            diagnostics::AppLog::info(
                QStringLiteral("recovery"),
                QStringLiteral("Removing orphaned manifest entry id=%1 path=%2").arg(e.id, e.artefact_path));
            store_.Remove(e.id);
            continue;
        }
        surviving.append(e);
    }

    QVector<RecoveryCandidate> candidates;
    candidates.reserve(surviving.size());
    for (const auto& e : surviving) {
        RecoveryCandidate c;
        c.entry = e;
        const QFileInfo fi(e.artefact_path);
        c.artefact_size_bytes = fi.exists() ? fi.size() : 0;
        candidates.append(c);
    }

    return candidates;
}

RecoveryActionResult RecoveryService::Finish(const RecoveryManifestEntry& entry,
                                             std::function<bool(float)> progress_cb) {
    const std::filesystem::path artefact(entry.artefact_path.toStdWString());
    const std::filesystem::path dest_folder = ResolveDestinationFolder(entry);
    const std::wstring stem = DeriveStemFromEntry(entry);

    const bool is_mp4 = (entry.intended_container.toLower() == QStringLiteral("mp4"));

    if (!is_mp4) {
        // MKV-intended path.
        const std::filesystem::path preferred = dest_folder / (stem + L".mkv");
        const auto target = ResolveUniqueOutputPath(preferred);

        if (entry.finalized) {
            // Artefact is cleanly finalized — simple rename, no remux needed.
            std::error_code ec;
            std::filesystem::rename(artefact, target, ec);
            if (ec) {
                const std::string msg = "Rename failed: " + ec.message();
                diagnostics::AppLog::warning(QStringLiteral("recovery"), QString::fromStdString(msg));
                return {false, msg};
            }
            store_.Remove(entry.id);
            diagnostics::AppLog::info(QStringLiteral("recovery"),
                                      QStringLiteral("Finish(mkv/rename) id=%1 → %2")
                                          .arg(entry.id, QString::fromStdWString(target.wstring())));
            return {true, {}};
        }

        // Not finalized — repair-remux via libavformat matroska muxer.
        const auto result = recorder_core::RemuxToMkv(artefact, target, WrapProgress(std::move(progress_cb)));
        if (!result.success)
            return {false, result.message};

        std::error_code rm_ec;
        std::filesystem::remove(artefact, rm_ec);
        if (rm_ec) {
            diagnostics::AppLog::warning(
                QStringLiteral("recovery"),
                QStringLiteral("Could not remove artefact after repair: %1").arg(entry.artefact_path));
        }
        store_.Remove(entry.id);
        diagnostics::AppLog::info(
            QStringLiteral("recovery"),
            QStringLiteral("Finish(mkv/remux) id=%1 → %2").arg(entry.id, QString::fromStdWString(target.wstring())));
        return {true, {}};
    }

    // MP4-intended path (finalized or not — we always remux MKV → MP4).
    const std::filesystem::path preferred = dest_folder / (stem + L".mp4");
    const auto target = ResolveUniqueOutputPath(preferred);

    const auto result = recorder_core::RemuxToProgressiveMp4(artefact, target, WrapProgress(std::move(progress_cb)));
    if (!result.success)
        return {false, result.message};

    std::error_code rm_ec;
    std::filesystem::remove(artefact, rm_ec);
    if (rm_ec) {
        diagnostics::AppLog::warning(
            QStringLiteral("recovery"),
            QStringLiteral("Could not remove artefact after MP4 finish: %1").arg(entry.artefact_path));
    }
    store_.Remove(entry.id);
    diagnostics::AppLog::info(
        QStringLiteral("recovery"),
        QStringLiteral("Finish(mp4) id=%1 → %2").arg(entry.id, QString::fromStdWString(target.wstring())));
    return {true, {}};
}

// Legacy alias: Keep as MKV.
RecoveryActionResult RecoveryService::KeepAsMkv(const RecoveryManifestEntry& entry,
                                                std::function<bool(float)> progress_cb) {
    // Treat as Finish for an MKV-intended entry regardless of intended_container in the
    // stored manifest — this alias preserves existing test behaviour.
    RecoveryManifestEntry mkv_entry = entry;
    mkv_entry.intended_container = QStringLiteral("mkv");
    return Finish(mkv_entry, std::move(progress_cb));
}

// Legacy alias: Export as MP4.
RecoveryActionResult RecoveryService::ExportAsMp4(const RecoveryManifestEntry& entry,
                                                  std::function<bool(float)> progress_cb) {
    RecoveryManifestEntry mp4_entry = entry;
    mp4_entry.intended_container = QStringLiteral("mp4");
    return Finish(mp4_entry, std::move(progress_cb));
}

RecoveryActionResult RecoveryService::Discard(const RecoveryManifestEntry& entry) {
    const std::filesystem::path artefact(entry.artefact_path.toStdWString());

    if (QFileInfo::exists(entry.artefact_path)) {
        std::error_code ec;
        std::filesystem::remove(artefact, ec);
        if (ec) {
            const std::string msg = "Delete failed: " + ec.message();
            diagnostics::AppLog::warning(QStringLiteral("recovery"), QString::fromStdString(msg));
            return {false, msg};
        }
    }

    store_.Remove(entry.id);
    diagnostics::AppLog::info(QStringLiteral("recovery"),
                              QStringLiteral("Discard id=%1 path=%2").arg(entry.id, entry.artefact_path));
    return {true, {}};
}

// static
std::filesystem::path RecoveryService::ResolveUniqueOutputPath(const std::filesystem::path& preferred) {
    if (!std::filesystem::exists(preferred))
        return preferred;

    const auto parent = preferred.parent_path();
    const auto stem = preferred.stem().wstring();
    const auto ext = preferred.extension().wstring();

    for (int n = 2; n < 1000; ++n) {
        const auto candidate = parent / (stem + L" (" + std::to_wstring(n) + L")" + ext);
        if (!std::filesystem::exists(candidate))
            return candidate;
    }

    // Absolute fallback — should never be reached in practice.
    return preferred;
}

} // namespace exosnap
