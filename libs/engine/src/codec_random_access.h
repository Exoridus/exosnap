#pragma once

#include <exosnap/engine/recording_media_types.h>

#include <span>
#include <string_view>

namespace exosnap::engine {

// Certifies a self-contained video access point from the encoded sample and its decoder configuration.
// A container key flag alone never establishes a closed entry.
[[nodiscard]] RandomAccessKind QualifyClosedRandomAccess(std::string_view codec_id,
                                                         std::span<const uint8_t> codec_private,
                                                         std::span<const uint8_t> packet, bool advertised_key) noexcept;

} // namespace exosnap::engine
