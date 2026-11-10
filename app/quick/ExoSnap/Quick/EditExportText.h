#pragma once

#include <exosnap/engine/edit_export_issue.h>

#include <QCoreApplication>
#include <QString>

namespace exosnap::quick {

inline QString TranslateEditExportIssue(const engine::EditExportIssue& issue, bool original_copy_qualified) {
    using engine::EditExportIssueCode;
    switch (issue.code) {
    case EditExportIssueCode::EmptySpan:
        return QCoreApplication::translate("EditExportAdapter", "The selected export range is empty.");
    case EditExportIssueCode::InvalidSelection:
        return QCoreApplication::translate("EditExportAdapter", "Select clips or a valid time range for export.");
    case EditExportIssueCode::PartialJunctionOwnership:
        return QCoreApplication::translate("EditExportAdapter",
                                           "The selected clips contain only one side of a transition.");
    case EditExportIssueCode::InvalidTimeline:
        return QCoreApplication::translate("EditExportAdapter", "The timeline contains an invalid clip.");
    case EditExportIssueCode::MissingSource:
        return QCoreApplication::translate("EditExportAdapter", "The timeline contains unavailable media.");
    case EditExportIssueCode::SourceChanged:
        return QCoreApplication::translate("EditExportAdapter",
                                           "A source changed after it was imported. Import it again.");
    case EditExportIssueCode::MixSelectionRequired:
        return QCoreApplication::translate(
            "EditExportAdapter", "Choose which audio streams of the recording form the mix before exporting.");
    case EditExportIssueCode::OriginalRecipeChanged:
        return QCoreApplication::translate("EditExportAdapter",
                                           "Original streams can only be kept for unedited source cuts. Export "
                                           "the finished result instead.");
    case EditExportIssueCode::UnsupportedAudioLayout:
        return QCoreApplication::translate("EditExportAdapter",
                                           "An audio stream has a channel layout that cannot be mixed to stereo.");
    case EditExportIssueCode::BoundaryConsentRequired:
        return QCoreApplication::translate("EditExportAdapter",
                                           "Copying would move the cut boundaries. Export a rendered result instead.");
    case EditExportIssueCode::ResourceLimitExceeded:
        return QCoreApplication::translate("EditExportAdapter", "The export needs more resources than are available.");
    case EditExportIssueCode::DestinationAliasesSource:
        return QCoreApplication::translate("EditExportAdapter",
                                           "The destination would replace an original source. Choose another file.");
    case EditExportIssueCode::Mp4AudioUnavailable:
        return QCoreApplication::translate("EditExportAdapter", "MP4 audio encoding is unavailable. Choose MKV.");
    case EditExportIssueCode::UnsupportedProfile:
        break;
    }
    if (issue.scope == engine::EditExportIssueScope::Audio)
        return QCoreApplication::translate("EditExportAdapter",
                                           "The timeline audio cannot be exported with this output.");
    return original_copy_qualified
               ? QCoreApplication::translate("EditExportAdapter",
                                             "This edit cannot be rendered here. To copy the source cut, keep the "
                                             "original streams in the export details.")
               : QCoreApplication::translate("EditExportAdapter",
                                             "The timeline video cannot be exported with this output.");
}

} // namespace exosnap::quick
