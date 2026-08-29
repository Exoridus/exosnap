#include "RecoveryService.h"

#include "services/AtomicFileOps.h"

#include <exosnap/engine/mp4_remuxer.h>

#include <QDir>
#include <QFileInfo>

#include <string>

#include "diagnostics/AppLog.h"

namespace exosnap {
namespace {

// Build a no-op or wrapping RemuxProgressCallback from the caller's bool-returning lambda.
exosnap::engine::RemuxProgressCallback WrapProgress(std::function<bool(float)> cb) {
    if (!cb)
        return exosnap::engine::RemuxNoopCallback();
    return [cb = std::move(cb)](float f) -> bool { return cb(f); };
}

// Derive a stem name for the output file from the manifest entry.
// Prefer the final_output_path stem (the recording's intended filename without extension);
// fall back to the artefact stem with the live-artifact suffix stripped.
std::wstring DeriveStemFromEntry(const RecoveryManifestEntry& entry) {
    if (!entry.final_output_path.isEmpty()) {
        std::filesystem::path p(entry.final_output_path.toStdWString());
        if (p.extension() == L".tmp" || p.extension() == L".partial")
            p.replace_extension(L"");
        return p.stem().wstring();
    }
    std::filesystem::path artefact(entry.artefact_path.toStdWString());
    if (artefact.extension() == L".tmp" || artefact.extension() == L".partial")
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
        const QFileInfo artefact(e.artefact_path);
        if (e.artefact_path.isEmpty() || !artefact.exists()) {
            // Orphaned entry — artefact is gone; remove silently.
            diagnostics::AppLog::info(
                QStringLiteral("recovery"),
                QStringLiteral("Removing orphaned manifest entry id=%1 path=%2").arg(e.id, e.artefact_path));
            store_.Remove(e.id);
            continue;
        }
        // An empty artefact is as orphaned as a missing one, and it used to
        // survive this filter purely because the file existed: a recording
        // killed before the muxer wrote its first byte then offered itself for
        // recovery on every single launch, promising to restore nothing. Size
        // is checked here rather than hidden in the UI, so the manifest stops
        // carrying the entry instead of the prompt learning to ignore it.
        if (artefact.size() <= 0) {
            diagnostics::AppLog::info(
                QStringLiteral("recovery"),
                QStringLiteral("Removing empty manifest entry id=%1 path=%2").arg(e.id, e.artefact_path));
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

    // Decide the destination for a remux-produced file. Recovery owns exactly
    // entry.final_output_path: a file already sitting there is this recording's own
    // half-written remux (a crash/powerloss mid-remux) and is replaced in place. A
    // collision at any *other* derived path is a stranger's file and is side-stepped
    // to "(N)". Combined with the temp+atomic-rename below, this guarantees the user
    // never finds a corrupt half-file where their recording should be.
    auto resolve_remux_target = [&](const std::filesystem::path& preferred) -> std::filesystem::path {
        if (!entry.final_output_path.isEmpty()) {
            const std::filesystem::path own(entry.final_output_path.toStdWString());
            if (PathsEqual(preferred, own))
                return preferred;
        }
        return ResolveUniqueOutputPath(preferred);
    };

    if (!is_mp4) {
        // MKV-intended path.
        const std::filesystem::path preferred = dest_folder / (stem + L".mkv");

        if (entry.finalized) {
            // Artefact is cleanly finalized — simple rename, no remux needed. A pure
            // rename is already atomic and cannot leave a torn file, so it keeps the
            // collision-safe (never-clobber) behaviour rather than replacing in place.
            const auto target = ResolveUniqueOutputPath(preferred);
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

        // Not finalized — repair-remux via libavformat matroska muxer. Write to a
        // sibling temp and only atomically publish it at the target on success, so a
        // crash during the repair leaves the target path untouched rather than a
        // half-written MKV.
        const std::filesystem::path repair_target = resolve_remux_target(preferred);
        const std::filesystem::path temp = MakeSiblingTempPath(repair_target);
        const auto result = exosnap::engine::RemuxToMkv(artefact, temp, WrapProgress(std::move(progress_cb)));
        if (!result.success) {
            // The repair did not complete cleanly — the artefact (the original,
            // possibly-unfinalized MKV) is the only trustworthy recording and
            // must be kept. Remove the abandoned temp so it is never mistaken for a
            // real result; the user-visible target path was never touched.
            std::error_code cleanup_ec;
            std::filesystem::remove(temp, cleanup_ec);
            return {false, result.message};
        }

        if (const unsigned long move_err = AtomicReplaceInPlace(temp, repair_target); move_err != 0) {
            std::error_code cleanup_ec;
            std::filesystem::remove(temp, cleanup_ec);
            const std::string msg = "Atomic move to final output failed (Win32 error " + std::to_string(move_err) + ")";
            diagnostics::AppLog::warning(QStringLiteral("recovery"), QString::fromStdString(msg));
            return {false, msg};
        }

        std::error_code rm_ec;
        std::filesystem::remove(artefact, rm_ec);
        if (rm_ec) {
            diagnostics::AppLog::warning(
                QStringLiteral("recovery"),
                QStringLiteral("Could not remove artefact after repair: %1").arg(entry.artefact_path));
        }
        store_.Remove(entry.id);
        diagnostics::AppLog::info(QStringLiteral("recovery"),
                                  QStringLiteral("Finish(mkv/remux) id=%1 → %2")
                                      .arg(entry.id, QString::fromStdWString(repair_target.wstring())));
        return {true, {}};
    }

    // MP4-intended path (finalized or not — we always remux MKV → MP4).
    //
    // Remux to a sibling temp on the target's own volume, then atomically rename it
    // onto the target. A kill/powerloss mid-remux leaves only the ".tmp" staging file - the
    // user-visible target path never holds a half-written MP4. When a corrupt partial
    // from an earlier interrupted remux already sits at the target, the atomic replace
    // overwrites it in place instead of side-stepping to a fresh name and stranding it.
    const std::filesystem::path preferred = dest_folder / (stem + L".mp4");
    const std::filesystem::path target = resolve_remux_target(preferred);
    const std::filesystem::path temp = MakeSiblingTempPath(target);

    const auto result = exosnap::engine::RemuxToProgressiveMp4(artefact, temp, WrapProgress(std::move(progress_cb)));
    if (!result.success) {
        // The remux did not complete cleanly — the artefact (playable MKV) is the
        // only trustworthy recording and must be kept. Remove the abandoned temp; the
        // target path was never touched (any pre-existing stale file stays as it was).
        std::error_code cleanup_ec;
        std::filesystem::remove(temp, cleanup_ec);
        return {false, result.message};
    }

    if (const unsigned long move_err = AtomicReplaceInPlace(temp, target); move_err != 0) {
        std::error_code cleanup_ec;
        std::filesystem::remove(temp, cleanup_ec);
        const std::string msg = "Atomic move to final output failed (Win32 error " + std::to_string(move_err) + ")";
        diagnostics::AppLog::warning(QStringLiteral("recovery"), QString::fromStdString(msg));
        return {false, msg};
    }

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
