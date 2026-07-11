#pragma once

#include <capability/capability_cache_key.h>
#include <capability/runtime_snapshot.h>

#include <QString>

#include <optional>

namespace exosnap {

// Disk cache for the last known-good RuntimeCapabilitySnapshot — a pure
// warm-start optimization, never a source of truth. The NVENC session probe,
// D3D11 device creation, and Media Foundation queries in
// CapabilityBuilder::BuildFromHardwareQuery() only ever change on a GPU or
// driver swap, yet MainWindow re-ran the full probe from scratch on every
// launch, leaving Diagnostics/Device blank until it finished.
//
// MainWindow still ALWAYS re-runs the real, off-thread hardware probe on every
// launch and REPLACES whatever this cache produced (and rewrites the cache
// with the fresh result) once it completes. A cache hit only lets
// Diagnostics/Device show non-blank data during the probe's cold window.
//
// Safety: RecordPage/the recording-start gate never sees a cache-sourced
// CapabilitySet. CapabilityBuilder::BuildEffectiveCapabilities(), which
// MainWindow calls directly on a cache hit, leaves CapabilitySet::probed at
// its default (false); only CapabilityBuilder::BuildFromHardwareQuery() (the
// real probe) sets it true, and RecordingCoordinator::OnCapabilitiesReady()
// refuses any CapabilitySet with probed == false. See MainWindow's warm-start
// wiring (the QTimer::singleShot(0) block that kicks off the async probe).
//
// Format: capability-cache.json in the app config dir (ConfigPaths), same
// atomic-write (QSaveFile) / "corrupt or mismatched -> discard silently, no
// migration" convention as RecoveryManifestStore (pre-v1.0 policy).
class CapabilityCacheStore {
  public:
    CapabilityCacheStore();
    explicit CapabilityCacheStore(QString file_path);

    // Returns the cached snapshot only when the stored key exactly matches
    // `expected_key` (adapter LUID, driver version, app version, schema
    // version — see CapabilityCacheKey). Missing file, a parse error, or any
    // key mismatch all return std::nullopt; none of these are logged as
    // errors — an absent or stale cache is normal steady-state behavior, not
    // a fault.
    [[nodiscard]] std::optional<capability::RuntimeCapabilitySnapshot>
    LoadMatching(const capability::CapabilityCacheKey& expected_key) const;

    // Overwrites the cache with the given snapshot + key, atomically. Called
    // after every real probe completes (whether or not a cache existed
    // before), so the NEXT launch can warm-start from this run's answer.
    bool Save(const capability::RuntimeCapabilitySnapshot& snapshot, const capability::CapabilityCacheKey& key) const;

    [[nodiscard]] const QString& StorePath() const;

  private:
    QString file_path_;
};

} // namespace exosnap
