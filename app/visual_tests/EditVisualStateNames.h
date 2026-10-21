#pragma once

#include <array>

namespace exosnap::visual::edit_state {

inline constexpr const char* kEmpty = "edit-empty";
inline constexpr const char* kDefault = "edit-default";
inline constexpr const char* kMultitrack = "edit-multitrack";
inline constexpr const char* kMissingMedia = "edit-missing-media";
inline constexpr const char* kSelectedClip = "edit-selected-clip";
inline constexpr const char* kTransitionSelected = "edit-transition-selected";
inline constexpr const char* kGap = "edit-gap";
inline constexpr const char* kMediaPanel = "edit-media-panel";
inline constexpr const char* kPanelsPinned = "edit-panels-pinned";
inline constexpr const char* kMonitoring = "edit-monitoring";
inline constexpr const char* kCrossfadePopup = "edit-crossfade-popup";
inline constexpr const char* kVolumePopup = "edit-volume-popup";
inline constexpr const char* kExportReview = "edit-export-review";
inline constexpr const char* kExportRunning = "edit-export-running";
inline constexpr const char* kExportDone = "edit-export-done";
inline constexpr const char* kExportFailed = "edit-export-failed";
inline constexpr const char* kLongFilename = "edit-long-filename";
inline constexpr const char* kFoldersEmpty = "edit-folders-empty";
inline constexpr const char* kFoldersExpanded = "edit-folders-expanded";
inline constexpr const char* kRecordings = "edit-recordings";
inline constexpr const char* kMediaEmpty = "edit-media-empty";
inline constexpr const char* kAudioSelected = "edit-audio-selected";
inline constexpr const char* kMultiselection = "edit-multiselection";
inline constexpr const char* kRange = "edit-range";
inline constexpr const char* kLoop = "edit-loop";
inline constexpr const char* kClipFades = "edit-clip-fades";
inline constexpr const char* kHardCutSelected = "edit-hard-cut-selected";
inline constexpr const char* kMovePreview = "edit-move-preview";
inline constexpr const char* kTrimPreview = "edit-trim-preview";
inline constexpr const char* kRemoveTimePreview = "edit-remove-time-preview";
inline constexpr const char* kSpacerSelected = "edit-spacer-selected";
inline constexpr const char* kTracksCollapsed = "edit-tracks-collapsed";
inline constexpr const char* kTrackLocked = "edit-track-locked";
inline constexpr const char* kTrackSolo = "edit-track-solo";
inline constexpr const char* kPreviewOpening = "edit-preview-opening";
inline constexpr const char* kPreviewBuffering = "edit-preview-buffering";
inline constexpr const char* kPreviewDegraded = "edit-preview-degraded";
inline constexpr const char* kPreviewFailed = "edit-preview-failed";
inline constexpr const char* kSourceAudioChoice = "edit-source-audio-choice";
inline constexpr const char* kSourceRelinkReview = "edit-source-relink-review";

inline constexpr std::array kAll{kEmpty,
                                 kDefault,
                                 kMultitrack,
                                 kMissingMedia,
                                 kSelectedClip,
                                 kTransitionSelected,
                                 kGap,
                                 kMediaPanel,
                                 kPanelsPinned,
                                 kMonitoring,
                                 kCrossfadePopup,
                                 kVolumePopup,
                                 kExportReview,
                                 kExportRunning,
                                 kExportDone,
                                 kExportFailed,
                                 kLongFilename,
                                 kFoldersEmpty,
                                 kFoldersExpanded,
                                 kRecordings,
                                 kMediaEmpty,
                                 kAudioSelected,
                                 kMultiselection,
                                 kRange,
                                 kLoop,
                                 kClipFades,
                                 kHardCutSelected,
                                 kMovePreview,
                                 kTrimPreview,
                                 kRemoveTimePreview,
                                 kSpacerSelected,
                                 kTracksCollapsed,
                                 kTrackLocked,
                                 kTrackSolo,
                                 kPreviewOpening,
                                 kPreviewBuffering,
                                 kPreviewDegraded,
                                 kPreviewFailed,
                                 kSourceAudioChoice,
                                 kSourceRelinkReview};

} // namespace exosnap::visual::edit_state
