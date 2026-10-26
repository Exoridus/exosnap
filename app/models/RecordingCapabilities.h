#pragma once

namespace exosnap::product {

// Release still requires inclusive per-job memory and media/isolation
// qualification. No runtime configuration may override this product boundary.
inline constexpr bool kRecordingClipsAvailable = true;
inline constexpr const char* kRecordingClipsUnavailableReason =
    "Recording Clips are unavailable in this product version";

} // namespace exosnap::product
