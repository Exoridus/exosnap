#pragma once

#include <capability/audio_ui_state.h>

#include "PresentationState.h"

namespace exosnap {

// ---------------------------------------------------------------------------
// PresentationStateBuilder
// ---------------------------------------------------------------------------
// Pure stateless builder that derives presentation snapshots from canonical
// application state. Has no Qt dependencies and no side effects.
//
// Required invariant for the final enabled state of each audio control:
//
//   controls_enabled = visible && available && !controls_locked
//
// This invariant holds regardless of the order in which audio state and lock
// state are delivered — both paths call BuildAudioConfiguration with the
// current values of both inputs.

class PresentationStateBuilder {
  public:
    // Derive a complete AudioConfigurationSnapshot from an AudioUiState and a
    // recording-lock flag.  The result describes exactly which rows are shown,
    // available, enabled, and interactable — ready for a single atomic widget
    // application by the consumer.
    [[nodiscard]] static AudioConfigurationSnapshot BuildAudioConfiguration(const capability::AudioUiState& audio_state,
                                                                            bool controls_locked);
};

} // namespace exosnap
