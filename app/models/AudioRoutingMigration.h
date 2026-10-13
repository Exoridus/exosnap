#pragma once

#include <capability/audio_ui_state.h>
#include <exosnap/engine/audio_routing_model.h>
#include <functional>

namespace exosnap {

[[nodiscard]] engine::LegacyRows25Intent CreateLegacyAudioIntent(const capability::AudioUiState& audio,
                                                                 engine::PresetNamespaceId id_namespace);
[[nodiscard]] engine::TrackGraph1Intent MakeDefaultAudioIntent();
[[nodiscard]] engine::TrackGraph1Intent MakeNewAudioIntent();
void ProjectLegacyAudioIntent(const engine::LegacyRows25Intent& intent, capability::AudioUiState& audio);
void ProjectNativeAudioIntent(const engine::TrackGraph1Intent& intent, capability::AudioUiState& audio);
[[nodiscard]] engine::Outcome<engine::LegacyRows25Intent>
SynchronizeLegacyAudioRows(const engine::LegacyRows25Intent& intent, const capability::AudioUiState& audio);

struct AudioIntentConversion {
    engine::AudioIntent before;
    engine::AudioIntent after;
};

[[nodiscard]] engine::Outcome<AudioIntentConversion>
PrepareAudioIntentConversion(const engine::LegacyRows25Intent& intent,
                             const engine::AudioEnvironmentSnapshot& environment,
                             const std::function<void(engine::TrackGraph1Intent&)>& edit);

} // namespace exosnap
