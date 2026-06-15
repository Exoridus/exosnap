#pragma once
// CrashIssueReport.h -- Stage-0 GitHub crash-issue builder.
//
// Pure, UI-agnostic helpers (QString/QUrl only — no widgets, no QObject) that
// turn an already-scrubbed crash model into:
//   1. a metadata block (shown in the report body / copied to clipboard), and
//   2. a prefilled GitHub "new issue" URL using the crash issue-form template.
//
// Privacy (ADR 0017): every field below is allowlisted, structured context.
// No file paths, usernames or machine names ever appear here — callers scrub
// upstream (crash_capture::ScrubString) before populating CrashIssueData.
//
// The URL prefills the crash issue form at .github/ISSUE_TEMPLATE/crash.yml.
// The auto-filled metadata textarea in that form has id "metadata"; the URL
// sets it via the issue-form query parameter &metadata=<value>.

#include <QString>

namespace exosnap::services {

// Allowlisted crash facts surfaced to the user / GitHub. All values are
// expected to be pre-scrubbed by the caller.
struct CrashIssueData {
    QString app_version;    // "0.4.0 · build <sha>"
    QString os;             // "Windows 11 · 26100.x"
    QString gpu;            // "NVIDIA RTX 4070 · driver 552.44"
    QString encoder;        // "NVENC AV1 → MKV"
    QString exception;      // optional; empty -> omitted / shown as "—"
    QString correlation_id; // random per-report id (not a persistent install id)
};

// The metadata block shown in the issue body / copied to clipboard.
// Allowlisted fields only. A missing exception renders as "—".
QString BuildCrashMetadataBlock(const CrashIssueData& data);

// A prefilled GitHub "new issue" URL for repo Exoridus/exosnap using the crash
// issue-form template (template=crash.yml, labels=crash). The metadata block is
// passed via the form's &metadata= field id.
//
// All values are percent-encoded via QUrl/QUrlQuery. The URL is kept within a
// sane length budget (<= 6000 chars); if the encoded URL would exceed the
// budget, the bare template URL is returned instead (the metadata still goes to
// the clipboard via BuildCrashMetadataBlock).
QString BuildCrashIssueUrl(const CrashIssueData& data);

} // namespace exosnap::services
