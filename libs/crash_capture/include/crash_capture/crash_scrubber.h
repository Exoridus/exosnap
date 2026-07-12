#pragma once

// crash_scrubber.h — Client-side event scrubber for ExoSnap crash reports.
//
// The before_send hook calls ScrubEvent() before any event is sent to Sentry
// (or written to the local crash dir).
//
// Scrubbing rules (ADR 0017):
//   REMOVE from event:
//     - %USERPROFILE% / %USERNAME% / machine name anywhere in string fields
//     - Any absolute Windows path (C:\..., UNC \\...)
//     - Output paths and recording filenames set via SetTag / extra context
//
//   ALLOWLIST (only these structured fields survive):
//     - os.name, os.version (OS build info)
//     - gpu.model, gpu.vendor, gpu.driver (hardware ID, no user data)
//     - app.version (from CMake project version)
//     - encoder_backend, container, video_codec, audio_codec
//     - correlation_id (random per-report, NOT persistent install ID)
//     - exception.type, exception.value (crash reason, paths stripped)
//     - stacktrace (module offsets only — sentry server resolves via PDB)
//
// This header exposes the scrubbing logic as pure functions so it can be
// unit-tested without linking sentry-native.

#include <array>
#include <span>
#include <string>
#include <string_view>

namespace exosnap::crash_capture {

// ---------------------------------------------------------------------------
// Allow-listed tag keys (structured context allowed through before_send).
// Any tag whose key does NOT appear here is stripped before upload.
//
// This is the SINGLE source of truth (ADR 0045): it used to also live as a
// literal, hand-repeated brace-list inside BeforeSendHook (crash_capture.cpp),
// which could drift from this array. BeforeSendHook now iterates
// AllowedTagKeys() instead. The markers below are also parsed by
// scripts/validate-privacy-allowlist.ps1, which checks that every key here is
// documented in PRIVACY.md and docs/product-spec.md §14 (and vice versa) — do
// not reformat the array declaration in a way the script's regex would miss
// (see that script's header comment for the exact pattern it expects).
// PRIVACY-ALLOWLIST-BEGIN
inline constexpr std::array<std::string_view, 10> kAllowedTagKeys = {
    "os.name",     "os.version",      "gpu.model", "gpu.vendor",  "gpu.driver",
    "app.version", "encoder_backend", "container", "video_codec", "audio_codec",
};
// PRIVACY-ALLOWLIST-END

// Accessor for consumers that want a span rather than the raw array (e.g. the
// before_send hook, which no longer keeps its own copy of the key list).
inline constexpr std::span<const std::string_view> AllowedTagKeys() {
    return kAllowedTagKeys;
}

// ---------------------------------------------------------------------------
// Scrub a single string value.
//
// Replaces substrings that contain:
//   - The current user's home directory (%USERPROFILE%)
//   - The current username (%USERNAME% / GetUserNameW)
//   - The machine name (GetComputerNameW)
//   - Any absolute Windows path pattern (C:\... or \\...)
//
// Replacements use a fixed neutral placeholder:
//   paths → "[path]"
//   username → "[user]"
//   machine  → "[machine]"
//
// The function is pure (stateless) and safe to call from any thread.
// ---------------------------------------------------------------------------
std::string ScrubString(std::string_view input);

// ---------------------------------------------------------------------------
// Returns true if the given tag key is in the allow-list.
// Tags not in the allow-list are silently dropped by the before_send hook.
// ---------------------------------------------------------------------------
bool IsAllowedTagKey(std::string_view key);

// ---------------------------------------------------------------------------
// Generate a random per-report correlation ID (UUID v4, lowercase hex).
// Each call produces a new value; these are NOT persisted between sessions.
// ---------------------------------------------------------------------------
std::string GenerateCorrelationId();

// ---------------------------------------------------------------------------
// SensitiveValueCache — lazily resolved, refreshed per-process.
// Holds the current user's home dir, username, and machine name so ScrubString
// does not call WinAPI on every invocation.
//
// Exposed for testing; production code uses ScrubString which calls the cache
// internally.
// ---------------------------------------------------------------------------
struct SensitiveValueCache {
    std::string userprofile;  // e.g. C:\Users\Alice
    std::string username;     // e.g. Alice
    std::string machine_name; // e.g. DESKTOP-XYZ123

    // Populate from the current Windows environment / WinAPI.
    // Safe to call multiple times; subsequent calls refresh the values.
    void Refresh();

    // Singleton for production use. Reset() clears it (for tests).
    static SensitiveValueCache& Instance();
    static void Reset();
};

} // namespace exosnap::crash_capture
