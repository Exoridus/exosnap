#include "RecoveryService.h"

#include <recorder_core/mp4_remuxer.h>

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

// Derive the base output path for a "Keep as MKV" operation from the entry.
// Rules:
//   - If artefact ends with ".mkv.tmp", strip ".tmp" → ".mkv".
//   - Otherwise use the final_output_path with extension forced to ".mkv".
//   - Fall back to artefact_path + ".repaired.mkv" if all else is empty.
std::filesystem::path DeriveKeepAsMkvBase(const RecoveryManifestEntry& entry) {
    const std::filesystem::path artefact(entry.artefact_path.toStdWString());
    if (artefact.extension() == L".tmp") {
        // .mkv.tmp → .mkv
        auto base = artefact;
        base.replace_extension(L""); // remove .tmp → now ends in .mkv
        return base;
    }
    if (!entry.final_output_path.isEmpty()) {
        auto base = std::filesystem::path(entry.final_output_path.toStdWString());
        base.replace_extension(L".mkv");
        return base;
    }
    return artefact.parent_path() / (artefact.stem().wstring() + L".repaired.mkv");
}

// Derive the base output path for an "Export as MP4" operation.
std::filesystem::path DeriveExportAsMp4Base(const RecoveryManifestEntry& entry) {
    if (!entry.final_output_path.isEmpty()) {
        auto base = std::filesystem::path(entry.final_output_path.toStdWString());
        base.replace_extension(L".mp4");
        return base;
    }
    const std::filesystem::path artefact(entry.artefact_path.toStdWString());
    auto base = artefact;
    base.replace_extension(L".mp4");
    return base;
}

} // namespace

RecoveryService::RecoveryService(RecoveryManifestStore& store) : store_(store) {
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

RecoveryActionResult RecoveryService::KeepAsMkv(const RecoveryManifestEntry& entry,
                                                std::function<bool(float)> progress_cb) {
    const std::filesystem::path artefact(entry.artefact_path.toStdWString());

    if (entry.finalized) {
        // Artefact is a cleanly finalized MKV (or .mkv.tmp that was fully
        // written before stop). A simple rename is sufficient — no remux needed.
        const auto target = ResolveUniqueOutputPath(DeriveKeepAsMkvBase(entry));

        std::error_code ec;
        std::filesystem::rename(artefact, target, ec);
        if (ec) {
            const std::string msg = "Rename failed: " + ec.message();
            diagnostics::AppLog::warning(QStringLiteral("recovery"), QString::fromStdString(msg));
            return {false, msg};
        }

        store_.Remove(entry.id);
        diagnostics::AppLog::info(
            QStringLiteral("recovery"),
            QStringLiteral("KeepAsMkv (rename) id=%1 → %2").arg(entry.id, QString::fromStdWString(target.wstring())));
        return {true, {}};
    }

    // Not finalized — repair-remux via libavformat matroska muxer.
    const auto target = ResolveUniqueOutputPath(DeriveKeepAsMkvBase(entry));

    const auto result = recorder_core::RemuxToMkv(artefact, target, WrapProgress(std::move(progress_cb)));
    if (!result.success) {
        return {false, result.message};
    }

    // Delete the artefact after successful remux.
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
        QStringLiteral("KeepAsMkv (remux) id=%1 → %2").arg(entry.id, QString::fromStdWString(target.wstring())));
    return {true, {}};
}

RecoveryActionResult RecoveryService::ExportAsMp4(const RecoveryManifestEntry& entry,
                                                  std::function<bool(float)> progress_cb) {
    const std::filesystem::path artefact(entry.artefact_path.toStdWString());
    const auto target = ResolveUniqueOutputPath(DeriveExportAsMp4Base(entry));

    const auto result = recorder_core::RemuxToProgressiveMp4(artefact, target, WrapProgress(std::move(progress_cb)));
    if (!result.success) {
        // On failure/cancel, artefact and manifest entry are preserved.
        return {false, result.message};
    }

    std::error_code rm_ec;
    std::filesystem::remove(artefact, rm_ec);
    if (rm_ec) {
        diagnostics::AppLog::warning(
            QStringLiteral("recovery"),
            QStringLiteral("Could not remove artefact after MP4 export: %1").arg(entry.artefact_path));
    }

    store_.Remove(entry.id);
    diagnostics::AppLog::info(
        QStringLiteral("recovery"),
        QStringLiteral("ExportAsMp4 id=%1 → %2").arg(entry.id, QString::fromStdWString(target.wstring())));
    return {true, {}};
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
