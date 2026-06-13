#pragma once

#include "config_types.h"

#include <string_view>

// ---------------------------------------------------------------------------
// ContainerCompatRegistry — ADR 0010 container × video-codec × audio-codec
// compatibility classification.
//
// This is the single source of truth for whether a container/video/audio
// combination is permitted, recommended, or prohibited.  The CapabilitySet
// queries this registry for container-level gating; no other component should
// duplicate these rules.
//
// Classification meanings (ADR 0010 §Compatibility classification):
//   Recommended   — vetted combination with a tested player/editor matrix
//   Allowed       — works but with caveats; UI shows a warning
//   Experimental  — technically possible; not yet tested at scale
//   Fallback      — used when a preferred combination fails; not user-selectable
//   Prohibited    — must not appear in the UI under any circumstance
// ---------------------------------------------------------------------------

namespace exosnap::capability {

// ---------------------------------------------------------------------------
// ContainerCompatLevel
// ---------------------------------------------------------------------------

enum class ContainerCompatLevel {
    Recommended,
    Allowed,
    Experimental,
    Fallback,
    Prohibited,
};

[[nodiscard]] std::string_view ToString(ContainerCompatLevel level) noexcept;

// Returns true when the level permits the combination to be offered in the UI.
// Recommended, Allowed, and Experimental are selectable; Fallback and Prohibited
// are not user-selectable.
[[nodiscard]] inline constexpr bool IsContainerCompatSelectable(ContainerCompatLevel level) noexcept {
    return level == ContainerCompatLevel::Recommended || level == ContainerCompatLevel::Allowed ||
           level == ContainerCompatLevel::Experimental;
}

// Returns true when the level is a hard block — must not be recorded.
// Only Prohibited maps to this.
[[nodiscard]] inline constexpr bool IsContainerCompatProhibited(ContainerCompatLevel level) noexcept {
    return level == ContainerCompatLevel::Prohibited;
}

// ---------------------------------------------------------------------------
// ContainerCompatEntry
// ---------------------------------------------------------------------------

struct ContainerCompatEntry {
    ContainerCompatLevel level = ContainerCompatLevel::Prohibited;
    // Human-readable explanation shown in validation errors / warnings.
    std::string_view reason;
};

// ---------------------------------------------------------------------------
// ContainerCompatRegistry
// ---------------------------------------------------------------------------

// Pure, stateless registry.  All methods are static; the registry has no
// mutable state and no Qt or UI dependencies.  It is safe to call from any
// thread.
class ContainerCompatRegistry {
  public:
    // Query the compatibility classification for a container × video × audio
    // combination.  Returns a Prohibited entry for any unrecognised enum value.
    [[nodiscard]] static ContainerCompatEntry Query(Container container, VideoCodec video, AudioCodec audio) noexcept;

    // Returns the preferred audio codec for a given container.  Used by
    // reconciliation logic when an incompatible audio codec must be replaced.
    [[nodiscard]] static AudioCodec PreferredAudioCodec(Container container) noexcept;

    // Returns the preferred video codec for a given container.  Used by
    // reconciliation logic when an incompatible video codec must be replaced.
    [[nodiscard]] static VideoCodec PreferredVideoCodec(Container container) noexcept;

    // Reconciles audio (and optionally video) codecs in `output` to a
    // permitted combination for its container.  Modifies `output` in place.
    // This is the single call-site for preset/config sanitization; callers
    // that previously contained ad-hoc switch/if chains should call this
    // instead.
    //
    // Rules applied, in order:
    //   1. If the current container × video × audio is Recommended or Allowed,
    //      leave it unchanged.
    //   2. If only audio is incompatible, replace audio with the preferred
    //      codec for the container.
    //   3. If video is also incompatible (e.g. WebM + H.264), replace both
    //      video and audio with the preferred codecs for the container.
    static void ReconcileCodecs(Container container, VideoCodec& video, AudioCodec& audio) noexcept;
};

} // namespace exosnap::capability
