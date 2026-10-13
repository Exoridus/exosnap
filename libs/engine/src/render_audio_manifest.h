#pragma once

#include <exosnap/engine/edit_timeline_render.h>

#include <memory>
#include <vector>

namespace exosnap::engine {

struct RenderAudioPreflight {
    AudioManifest1 manifest;
    std::vector<std::shared_ptr<void>> source_leases;
};

// Selections must already be validated against leased source assets.
Outcome<AudioManifest1> BuildRenderAudioManifest(const std::vector<MonitorSelectionSnapshot>& validated_selections);
Outcome<RenderAudioPreflight> PrepareRenderAudioManifest(const TimelineRenderSnapshot& snapshot);

} // namespace exosnap::engine
