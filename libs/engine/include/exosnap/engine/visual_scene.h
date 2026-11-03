#pragma once

#include <exosnap/engine/recording_media_types.h>
#include <exosnap/engine/webcam_placement.h>

namespace exosnap::engine {

using VisualSourceId = OpaqueId<struct VisualSourceIdTag>;
using SceneItemId = OpaqueId<struct SceneItemIdTag>;
using VisualRuntimeGeneration = ScalarValue<struct VisualRuntimeGenerationTag>;
using SourceRequestRevision = ScalarValue<struct SourceRequestRevisionTag>;
using CaptureTime100ns = ScalarValue<struct CaptureTime100nsTag>;

struct VisualAddress {
    VisualSourceId source;
    VisualRuntimeGeneration runtime;
    auto operator<=>(const VisualAddress&) const = default;
};

struct SourceToken {
    VisualAddress address;
    SourceRequestRevision request;
    auto operator<=>(const SourceToken&) const = default;
};

struct SceneItemProperties {
    WebcamPlacement placement;
    float opacity = 1.0f;
    bool output_visible = true;
    bool locked = false;
    bool aspect_ratio_locked = true;
    bool user_placed = false;
    int32_t order = 0;
};

} // namespace exosnap::engine
