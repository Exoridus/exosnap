#pragma once

namespace exosnap::diagnostics {

// Injectable interface for querying whether the current process runs with an
// elevated (administrator) token.  Production code uses Win32ElevationProvider;
// tests inject a stub returning a fixed value.  Mirrors the provider pattern of
// IDiskSpaceProvider / IFilesystemProvider (ADR 0033).
class IElevationProvider {
  public:
    virtual ~IElevationProvider() = default;

    // Returns true when the current process token reports an elevated integrity
    // level (full administrator token).  On query failure returns false (treat
    // as non-elevated — the safe default for the on-demand "relaunch to unlock"
    // posture; we never assume elevation we cannot prove).
    [[nodiscard]] virtual bool IsElevated() const = 0;
};

// Win32-backed implementation using OpenProcessToken + GetTokenInformation with
// TokenElevation.  Deliberately NOT IsUserAnAdmin (deprecated and reports group
// membership, not the actual token elevation state).
class Win32ElevationProvider final : public IElevationProvider {
  public:
    [[nodiscard]] bool IsElevated() const override;
};

} // namespace exosnap::diagnostics
