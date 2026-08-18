#pragma once
// WhatsNewPayload.h -- persistence for the one-time post-update "What's new"
// overlay.
//
// When the user clicks Update, the gap-aware release notes (every version in
// (installed, target]) are written to a small JSON payload in the app config
// dir. On the next launch, if the payload's target equals the running build and
// the suppress setting is off, the overlay is shown once and the payload is
// deleted. First install / downgrade / manual-ZIP update leave no matching
// payload, so the overlay never appears.
//
// The write/read/delete helpers take an explicit path so they can be unit-tested
// headless; WhatsNewPayloadPath() resolves the real config-dir location.

#include <QString>
#include <QVector>
#include <optional>

namespace exosnap {

struct WhatsNewNote {
    QString version;  // e.g. "1.2.0"
    QString body;     // GitHub release body (Markdown)
    QString html_url; // release page URL

    [[nodiscard]] bool operator==(const WhatsNewNote&) const = default;
};

struct WhatsNewPendingPayload {
    QString target_version;      // running build must equal this to show
    QVector<WhatsNewNote> notes; // newest first
};

// JSON round-trip to an explicit file path. Returns false on IO failure.
bool WriteWhatsNewPayload(const QString& path, const WhatsNewPendingPayload& payload);

// nullopt when the file is absent or cannot be parsed.
[[nodiscard]] std::optional<WhatsNewPendingPayload> ReadWhatsNewPayload(const QString& path);

// Best-effort delete; no-op when the file is absent.
void DeleteWhatsNewPayload(const QString& path);

// Pure decision: show the post-update overlay iff a payload is present, carries
// at least one note, its target equals the running version, and notices are not
// suppressed.
[[nodiscard]] bool ShouldShowWhatsNew(const std::optional<WhatsNewPendingPayload>& payload,
                                      const QString& running_version, bool suppressed);

// Default payload location: <config-dir>/whats-new-pending.json.
[[nodiscard]] QString WhatsNewPayloadPath();

struct WhatsNewConsumption {
    bool show = false;
    QVector<WhatsNewNote> notes; // empty unless show
};

// Read the payload, decide with ShouldShowWhatsNew(), and CLEAR the file — the
// whole one-time contract in one call, so no caller can implement half of it.
//
// The file goes in every case:
//   - shown: it has been shown, and once is what "one-time" means;
//   - target mismatch (first install, downgrade, manual-ZIP update): it does not
//     describe this build and never will;
//   - suppressed: the user asked not to see it, not to be asked again later;
//   - unparseable: keeping it means failing to parse it again on every launch.
[[nodiscard]] WhatsNewConsumption ConsumeWhatsNewPayload(const QString& path, const QString& running_version,
                                                         bool suppressed);

} // namespace exosnap
