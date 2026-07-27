#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

#include <cstdint>
#include <vector>

namespace exosnap::diagnostics {

// One file inside the support bundle: an entry name (zip-relative) and its bytes.
struct BundleEntry {
    QString name;
    QByteArray bytes;
};

// Plain-data view of the host's GPU/OS/runtime facts (allowlist thinking: only
// these known-safe fields are ever put in the bundle; never a raw dump).
struct BundleCapability {
    QString gpu_adapter_name;
    bool nvenc_dll_present = false;
    QString nvenc_api_version;
    bool nvenc_av1 = false;
    bool nvenc_hevc = false;
    bool nvenc_h264 = false;
    bool nvenc_444 = false;
    QString os_version_string;
    QString os_build_number;
    bool mf_webcam = false;
};

struct BundleAdapter {
    QString name;
    QString vendor;
    QString kind;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    uint64_t dedicated_vram_bytes = 0;
    QString driver_version;
};

struct BundleDisplay {
    QString name;
    bool hdr_active = false;
    uint32_t bits_per_color = 0;
    double min_luminance = 0.0;
    double max_luminance = 0.0;
    double max_full_frame_luminance = 0.0;
};

// Everything the collector needs. Log/report bytes are read from log_dir; the
// structured facts are supplied as plain data so the collector never depends on
// capability internals and is fully unit-testable from fixtures.
struct BundleInputs {
    QString log_dir; // directory holding exosnap.log, engine.jsonl and reports/
    int max_reports = 10;

    BundleCapability capability;
    std::vector<BundleAdapter> adapters;
    std::vector<BundleDisplay> displays;

    // Human-readable settings summary (ConfigSummary). Scrubbed before inclusion.
    QString settings_summary;

    // Manifest metadata.
    QString app_version;
    QString channel;
    QString commit_sha;
    QString launch_session_id;
    QString scrubber_version;
    QString created_at; // ISO 8601; a fixed value keeps tests deterministic
};

// Bundle-local redaction of capture-target identifiers. ScrubString only removes
// paths/username/machine; a window title in a `target="…"` freetext field or a
// JSONL `"target":"…"` field carries no drive prefix and would slip through, so we
// additionally replace the value with [capture-target] (backend/event survive).
// This lives here, NOT in crash_scrubber, so the consent-gated crash path is
// unaffected.
[[nodiscard]] QString RedactCaptureTargets(const QString& text);

// ScrubString (paths/user/machine) followed by RedactCaptureTargets. The single
// text-scrubbing pass every log/report entry goes through.
[[nodiscard]] QByteArray ScrubBundleText(const QByteArray& text);

// Assemble every bundle entry: the rotated text + JSONL logs (scrubbed), the last
// N session reports (scrubbed), capability/adapters/displays JSON, a scrubbed
// settings.txt, and bundle-manifest.json. Deterministic for fixed inputs.
[[nodiscard]] std::vector<BundleEntry> CollectBundleEntries(const BundleInputs& inputs);

// Write the collected entries to a .zip at zip_path (UTF-16). Returns false on any
// failure. out_error receives a message on failure.
bool WriteBundleZip(const QString& zip_path, const std::vector<BundleEntry>& entries, QString* out_error = nullptr);

} // namespace exosnap::diagnostics
