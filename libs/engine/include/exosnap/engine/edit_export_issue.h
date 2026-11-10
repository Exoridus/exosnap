#pragma once

#include <cstdint>
#include <vector>

namespace exosnap::engine {

enum class EditExportIssueCode {
    EmptySpan,
    InvalidTimeline,
    MissingSource,
    SourceChanged,
    MixSelectionRequired,
    OriginalRecipeChanged,
    UnsupportedProfile,
    UnsupportedAudioLayout,
    BoundaryConsentRequired,
    ResourceLimitExceeded,
    DestinationAliasesSource,
    InvalidSelection,
    PartialJunctionOwnership,
    Mp4AudioUnavailable
};
enum class EditExportIssueScope { Timeline, Source, Video, Audio, Destination };
struct EditExportIssue {
    EditExportIssueCode code = EditExportIssueCode::InvalidTimeline;
    EditExportIssueScope scope = EditExportIssueScope::Timeline;
    std::vector<uint64_t> affected_ids;
};

} // namespace exosnap::engine
