#pragma once

// SettingsSnapshot.h -- the serialization boundary for "what has ExoSnap been
// told to do", at the three levels where that question has three different
// answers.
//
//   requested  The user's own selection, as it is stored: RecordingPresetConfig
//              plus the app-level PersistedAppSettings. This is what the Settings
//              surface shows and what is written to disk.
//   effective  The same configuration after SanitizePresetConfig and the
//              capability resolver have had their say -- container x codec
//              reconciliation (ADR 0010), the 10-bit demotion (ADR 0032), the
//              4:4:4 snap, the MP4 CFR constraint, and the hardware-gated
//              fallbacks. This is what the next recording would actually use.
//   running    What the encoder was ACTUALLY initialized with, from
//              EncoderInitInfo. Only present once an encoder has been configured.
//
// The three are only interesting because they can disagree, so the payload
// reports the disagreement rather than leaving a client to diff two large
// objects: `differences` lists the fields where requested and effective part
// company, with the reason the resolver gave.
//
// Nothing here is a settings owner. It reads the models the product already has
// and answers with them; the reconciliation itself stays in
// capability::SettingsResolver and models::SanitizePresetConfig, where the
// Settings surface and the recording path both already read it.

#include "models/RecordingPreset.h"
#include "settings/AppSettingsStore.h"

#include <capability/resolver.h>
#include <recorder_core/pipeline_diagnostics.h>

#include <QJsonObject>
#include <QString>

namespace exosnap::observability {

struct SettingsSnapshotInputs {
    // The user's stored selection and the reconciled result. `effective` is
    // produced by the caller through the product's own reconciliation path, not
    // re-derived here.
    RecordingPresetConfig requested;
    RecordingPresetConfig effective;

    // The capability resolver's verdict on the effective configuration:
    // adjustments it had to make, warnings, and any field it considers invalid.
    // Meaningful only once the capability probe has landed.
    capability::ResolveResult resolution;
    bool capabilities_probed = false;

    // The encoder that is actually running (or that ran last). `valid == false`
    // means no encoder has been configured in this process, which is the honest
    // answer before the first recording -- never a fabricated copy of `effective`.
    recorder_core::EncoderInitInfo running;
    // True while the pipeline is in the recording/paused lifecycle. False with a
    // valid `running` means "the last session ran with this", which is a
    // different statement and is reported as one.
    bool running_live = false;

    // App-level settings: what the user told ExoSnap about ExoSnap, as opposed to
    // about a recording. Grouped separately in the payload for that reason.
    PersistedAppSettings app;

    // Whether a settings file is configured at all. The PATH is deliberately not
    // part of the payload -- see the privacy note in the implementation.
    QString settingsFilePath;
};

[[nodiscard]] QJsonObject SettingsSnapshotToJson(const SettingsSnapshotInputs& inputs);

// The recording-configuration half on its own, in the shape both `requested` and
// `effective` use. Exposed for tests and for the difference computation.
[[nodiscard]] QJsonObject RecordingConfigToJson(const RecordingPresetConfig& config);

} // namespace exosnap::observability
