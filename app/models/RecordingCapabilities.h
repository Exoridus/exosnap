#pragma once

namespace exosnap::product {

// Enabling creation requires inclusive per-job memory and media/isolation
// qualification. No runtime configuration may override this product boundary.
inline constexpr bool kRecordingClipsAvailable = false;
inline constexpr const char* kRecordingClipsUnavailableReason =
    "Recording Clips are unavailable in this product version";

} // namespace exosnap::product
