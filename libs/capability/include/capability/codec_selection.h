#pragma once

#include "capability_set.h"
#include "config_types.h"

#include <optional>
#include <string_view>

// ---------------------------------------------------------------------------
// codec_selection — pure, UI-agnostic "best available video codec" resolver.
//
// This is the SINGLE source of truth for "use the best codec your GPU supports"
// selection. Both the rec.profile.codec recommendation (Diagnostics) and the
// MainWindow FixAction handler call BestAvailableVideoCodec so the picked codec
// can never drift between the recommendation and the applied fix.
//
// No Qt, no UI, no engine-session state — only the static/runtime CapabilitySet
// and the container-compat registry.
// ---------------------------------------------------------------------------

namespace exosnap::capability {

// Returns the best GPU-supported, container-valid video codec for `container`,
// or nullopt when none qualifies.
//
// Preference order: AV1 -> HEVC -> H.264 (best quality/efficiency first).
// A codec qualifies when it is BOTH:
//   (a) GPU-supported   — caps.QueryVideoCodec(codec) is Available or
//                         ValidUnvalidated (IsSelectable), AND
//   (b) container-valid — ContainerCompatRegistry::Query(container, codec,
//                         PreferredAudioCodec(container)) is NOT Prohibited.
[[nodiscard]] std::optional<VideoCodec> BestAvailableVideoCodec(const CapabilitySet& caps,
                                                                Container container) noexcept;

// Canonical user-visible short label for a video codec ("AV1", "HEVC", "H.264").
// Pure (no Qt) so the engine/diagnostics layer can render codec names without
// pulling in the UI. The Qt CodecLabels.h videoCodecLabel() delegates here so
// there is one spelling canon (feedback_codec_naming_canon).
[[nodiscard]] std::string_view VisibleVideoCodecLabel(VideoCodec codec) noexcept;

} // namespace exosnap::capability
