#pragma once

// UpdateFeedOverride.h -- the dev feed override CLI opt-in.
//
// The standalone updater has had `--base-url` since it existed, and it is
// documented as the dev override: the trust anchor is the ed25519 signature over
// the manifest, not the origin of the JSON. The application had no equivalent,
// so nothing could exercise the app's own check -- and therefore the whole
// app-to-updater handoff -- against anything but the real GitHub feed. That is
// the one seam an end-to-end update test cannot work around.
//
// This adds the symmetric flag:
//
//     exosnap.exe --update-base-url https://<host>/<path>
//
// Rules, and they are the point:
//   * https only, and validated here rather than at the fetch, so a typo is a
//     refused launch instead of a check that quietly fails later.
//   * Refused outright in an OFFICIAL build. A shipped artifact must not be
//     pointable at another feed by a command line; the flag exists to test a
//     dev build, not to reconfigure a release.
//   * Not persisted anywhere: read from argv, held in memory, gone on restart.
//     No settings key, no environment variable, no UI switch.
//
// It does NOT weaken the signature or hash checks, and it does not touch the
// EXOSNAP_OFFICIAL_BUILD gate itself. What it does is let the app run the check
// MECHANICS (CheckAgainstFeed) against a named feed that is, by definition, not
// the production feed that gate is about.
//
// Pure parse seam (mirrors VerifyReinstallMode): no Win32, no I/O.

#include <QString>
#include <QStringList>

namespace exosnap::services {

// Command-line flag spelling. Exposed for tests and for the launcher wiring.
inline constexpr const char* kUpdateFeedOverrideFlag = "--update-base-url";

struct UpdateFeedOverride {
    bool requested = false;
    QString base_url;
    // Set when the flag was present but its value was missing, malformed or not
    // permitted in this build. The entry point must refuse to start rather than
    // silently fall back to the production feed: a test that believes it is
    // pointed at a fixture and is actually talking to GitHub reports the wrong
    // thing, and could act on a real release.
    QString error;
};

// Pure: inspects `args` (typically QCoreApplication::arguments(), argv[0]
// included). Exact flag match only -- no prefix and no "=value" form, so a
// longer unrelated flag can never enable it.
[[nodiscard]] UpdateFeedOverride ParseUpdateFeedOverride(const QStringList& args);

// True for a URL this override accepts: an https:// URL with a non-empty host.
// Exposed so the rule is testable without building an argv.
[[nodiscard]] bool IsAcceptableFeedUrl(const QString& url);

} // namespace exosnap::services
