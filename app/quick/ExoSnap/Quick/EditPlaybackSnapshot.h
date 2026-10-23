#pragma once

#include "models/EditRenderSnapshot.h"
#include "models/EditWorkspace.h"
#include <exosnap/engine/edit_timeline_render.h>

#include <memory>
#include <set>
#include <vector>

namespace exosnap::quick {

inline bool SamePlaybackMediaLayout(const engine::TimelineSnapshot& before, const engine::TimelineSnapshot& after) {
    if (before.duration_us != after.duration_us || before.output_fps_num != after.output_fps_num ||
        before.output_fps_den != after.output_fps_den || before.output_width != after.output_width ||
        before.output_height != after.output_height || before.clips.size() != after.clips.size() ||
        before.tracks.size() != after.tracks.size() || before.junctions != after.junctions ||
        before.crossfades.size() != after.crossfades.size())
        return false;
    const auto clips_equal =
        std::equal(before.clips.begin(), before.clips.end(), after.clips.begin(), [](auto a, auto b) {
            if (a.audio && b.audio) {
                a.enabled = b.enabled = true;
                a.gain_db = b.gain_db = 0;
            }
            return a == b;
        });
    const auto tracks_equal =
        std::equal(before.tracks.begin(), before.tracks.end(), after.tracks.begin(), [](auto a, auto b) {
            if (a.audio && b.audio) {
                a.enabled = b.enabled = true;
                a.gain_db = b.gain_db = 0;
            }
            return a == b;
        });
    return clips_equal && tracks_equal &&
           std::equal(before.crossfades.begin(), before.crossfades.end(), after.crossfades.begin(),
                      [](const auto& a, const auto& b) {
                          return a.outgoing == b.outgoing && a.incoming == b.incoming && a.duration_us == b.duration_us;
                      });
}

class EditPlaybackSnapshot {
  public:
    explicit EditPlaybackSnapshot(const edit::Workspace& workspace,
                                  std::vector<engine::TimelineMediaSource> sources = {},
                                  const std::set<edit::Id>& unavailable = {})
        : authored_(workspace.snapshot()), assets_(workspace.assets()), timeline_(edit::RenderTimeline(*authored_)),
          sources_(std::move(sources)) {
        for (auto& asset : assets_)
            if (unavailable.contains(asset.id))
                asset.state = edit::AssetState::Failed;
        for (auto& clip : timeline_.clips)
            if (unavailable.contains(clip.asset))
                clip.enabled = false;
    }

    [[nodiscard]] const std::shared_ptr<const edit::EditSnapshot>& authored() const {
        return authored_;
    }
    [[nodiscard]] edit::Revision revision() const {
        return authored_->revision;
    }
    [[nodiscard]] edit::Time duration() const {
        return timeline_.duration_us;
    }
    [[nodiscard]] const auto& assets() const {
        return assets_;
    }
    [[nodiscard]] const auto& clips() const {
        return authored_->clips;
    }
    [[nodiscard]] const auto& tracks() const {
        return authored_->tracks;
    }
    [[nodiscard]] const auto& transitions() const {
        return authored_->junctions;
    }
    [[nodiscard]] const engine::TimelineSnapshot& timeline() const {
        return timeline_;
    }
    [[nodiscard]] const auto& sources() const {
        return sources_;
    }
    [[nodiscard]] bool retainsMedia(const EditPlaybackSnapshot& next) const {
        const auto same_monitor = [](const auto& before, const auto& after) {
            if (before.has_value() != after.has_value())
                return false;
            if (!before)
                return true;
            if (before->media_id != after->media_id || before->revision != after->revision ||
                before->overlap_confirmed != after->overlap_confirmed ||
                before->unverified_asset_binding != after->unverified_asset_binding ||
                before->tracks.size() != after->tracks.size())
                return false;
            return std::equal(
                before->tracks.begin(), before->tracks.end(), after->tracks.begin(), [](const auto& a, const auto& b) {
                    return a.track_id == b.track_id && a.gain_linear == b.gain_linear && a.muted == b.muted;
                });
        };
        // Proof arrival refines the same held bytes without changing decoder ownership.
        return SamePlaybackMediaLayout(timeline_, next.timeline_) && assets_.size() == next.assets_.size() &&
               std::equal(assets_.begin(), assets_.end(), next.assets_.begin(),
                          [&](const auto& a, const auto& b) {
                              return a.id == b.id && a.path == b.path && a.state == b.state &&
                                     a.source_version == b.source_version &&
                                     same_monitor(a.monitor_selection, b.monitor_selection);
                          }) &&
               sources_.size() == next.sources_.size() &&
               std::equal(sources_.begin(), sources_.end(), next.sources_.begin(), [&](const auto& a, const auto& b) {
                   return a.id == b.id && a.path == b.path && a.access == b.access &&
                          same_monitor(a.monitor_selection, b.monitor_selection);
               });
    }
    [[nodiscard]] const edit::Asset* asset(edit::Id id) const {
        const auto found =
            std::find_if(assets_.begin(), assets_.end(), [id](const auto& value) { return value.id == id; });
        return found == assets_.end() ? nullptr : &*found;
    }
    [[nodiscard]] const edit::Clip* clip(edit::Id id) const {
        const auto& values = authored_->clips;
        const auto found =
            std::find_if(values.begin(), values.end(), [id](const auto& value) { return value.id == id; });
        return found == values.end() ? nullptr : &*found;
    }
    [[nodiscard]] std::shared_ptr<const EditPlaybackSnapshot>
    withMonitorSelection(edit::Id id, const engine::MonitorSelectionSnapshot& selection) const {
        auto next = std::make_shared<EditPlaybackSnapshot>(*this);
        for (auto& asset : next->assets_)
            if (asset.id == id)
                asset.monitor_selection = selection;
        return next;
    }

  private:
    std::shared_ptr<const edit::EditSnapshot> authored_;
    std::vector<edit::Asset> assets_;
    engine::TimelineSnapshot timeline_;
    std::vector<engine::TimelineMediaSource> sources_;
};

} // namespace exosnap::quick
