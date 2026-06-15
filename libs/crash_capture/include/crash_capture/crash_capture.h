#pragma once

// crash_capture.h — Public API for ExoSnap crash-capture engine.
//
// Design constraints (ADR 0017 / CLAUDE.md):
//   - UI-agnostic: no Qt, no Windows message loop dependencies.
//   - EXOSNAP_OFFICIAL_BUILD gate: DSN is only compiled in when the flag is defined.
//     Self-builds never phone home.
//   - Consent gate: nothing is uploaded until the user explicitly grants consent
//     (call GiveUserConsent). Consent may be revoked at any time.
//   - before_send scrubbing: all sensitive paths, usernames, machine names, and
//     output filenames are stripped before any event leaves the process.
//   - Recovery-manifest coordination: the handler writes a "dump_handled" sentinel
//     so the recovery overlay and the crash dialog do not double-report.

#include <functional>
#include <string>
#include <string_view>

namespace exosnap::crash_capture {

// ---------------------------------------------------------------------------
// Configuration passed to Initialize().
// ---------------------------------------------------------------------------
struct CrashCaptureConfig {
    // Directory where minidumps are written. The caller must ensure this
    // directory is writable before calling Initialize().
    // Typical value: %LOCALAPPDATA%\ExoSnap\crashes
    std::string crash_dir;

    // Absolute path to the Crashpad handler executable (crashpad_handler.exe).
    // Must be co-located with the application binary at runtime.
    std::string handler_exe_path;

    // Human-readable application version (e.g., "0.4.0").
    // Used to build the release tag "exosnap@<version>".
    std::string app_version;

    // If true, enable verbose sentry debug output (dev builds only; must be
    // false in Release builds baked by the CI pipeline).
    bool debug_mode = false;
};

// ---------------------------------------------------------------------------
// Initialize the crash capture engine.
//
// Must be called once at process startup, before any other crash_capture API.
// May be called multiple times (idempotent after first successful call).
//
// When EXOSNAP_OFFICIAL_BUILD is defined:
//   - The hardcoded EU DSN is compiled in.
//   - Uploads are still gated by user consent (require_user_consent=1).
//
// When EXOSNAP_OFFICIAL_BUILD is NOT defined:
//   - No DSN is configured; Crashpad writes local minidumps only.
//   - No network traffic ever occurs.
//
// Returns true on success.  On failure, crash capture is silently disabled
// (the process continues normally).
// ---------------------------------------------------------------------------
bool Initialize(const CrashCaptureConfig& config);

// ---------------------------------------------------------------------------
// Shutdown the crash capture engine gracefully.
// Call before process exit to flush any pending events.
// ---------------------------------------------------------------------------
void Shutdown();

// ---------------------------------------------------------------------------
// Consent gate — governs whether captured events are uploaded to Sentry.
//
// On first use, consent is UNSET (nothing is uploaded).
// The UI layer calls GiveUserConsent() when the user opts in via the
// crash-reporting dialog.  RevokeUserConsent() resets to UNSET.
//
// These functions are safe to call from any thread after Initialize().
// ---------------------------------------------------------------------------
void GiveUserConsent();
void RevokeUserConsent();

// ---------------------------------------------------------------------------
// Attach metadata to the current scope. Safe to call after Initialize().
// Used by the app layer to record runtime context for crashes.
//   key  — tag name; must appear in the allow-list in crash_scrubber.h
//   value — tag value; will be scrubbed by before_send if sensitive
// ---------------------------------------------------------------------------
void SetTag(std::string_view key, std::string_view value);

// ---------------------------------------------------------------------------
// Record encoder backend (e.g., "nvenc", "amf", "sw") and container/codec
// as structured context that survives into the crash report.
// ---------------------------------------------------------------------------
void SetEncoderContext(std::string_view encoder_backend, std::string_view container, std::string_view video_codec,
                       std::string_view audio_codec);

// ---------------------------------------------------------------------------
// Query whether the crash capture engine is currently active.
// Returns false before Initialize() or after Shutdown().
// ---------------------------------------------------------------------------
bool IsActive() noexcept;

// ---------------------------------------------------------------------------
// "Dump handled" sentinel coordination with Recovery.
//
// WriteDumpHandledSentinel() writes a small JSON file next to the crash dir
// so the recovery overlay knows a crash dialog will be shown (and should not
// double-report the same recording session as "needs recovery").
//
// ReadAndClearDumpHandledSentinel() reads and deletes the sentinel, returning
// true if it was present. Called by the recovery overlay at startup.
//
// The sentinel file is: <crash_dir>/dump_handled.json
// ---------------------------------------------------------------------------
bool WriteDumpHandledSentinel(const std::string& crash_dir);
bool ReadAndClearDumpHandledSentinel(const std::string& crash_dir);

// ---------------------------------------------------------------------------
// Resolve the platform crash directory.
//
// On Windows:
//   If EXOSNAP_CONFIG_DIR env var is set → use that as base (test isolation).
//   Otherwise → %LOCALAPPDATA%\ExoSnap\crashes
//
// Returns an empty string on failure (e.g., LOCALAPPDATA not set).
// The caller is responsible for creating the directory if it does not exist.
// ---------------------------------------------------------------------------
std::string ResolveCrashDir();

// ---------------------------------------------------------------------------
// Resolve the crashpad_handler.exe path relative to the running executable.
//
// On Windows, uses GetModuleFileNameW to find the app directory, then appends
// "crashpad_handler.exe".  Returns empty string if the exe cannot be located.
// ---------------------------------------------------------------------------
std::string ResolveHandlerExePath();

} // namespace exosnap::crash_capture
