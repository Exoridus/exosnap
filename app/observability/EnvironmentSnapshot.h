#pragma once

// EnvironmentSnapshot.h -- the serialization boundary for what ExoSnap OBSERVES
// about the machine, as opposed to what it has been told to do.
//
// Read-only, in Wave C and by design here: nothing in this file can change a
// Windows-global state, and none of the values below has a setter anywhere near
// it. HDR, ACM, refresh rate and default audio endpoints are reported as facts,
// never as controls.
//
// Every group carries its own truth class rather than a bare boolean, because
// the honest answers are not binary:
//
//   available          measured, and this is the value
//   unavailable        measurable in principle, nothing measured yet
//   unsupported        structurally impossible on this machine/configuration
//   requiresElevation  the measurement exists but needs an elevated process
//   requiresOptIn      the measurement exists but the user has not opted in
//
// "HDR off" and "HDR unknown" are therefore two different payloads, which is the
// whole point: a check that treats an unprobed adapter list as "no GPU" is a
// check that reports a hardware fault whenever a probe has not finished.
//
// Free of Qt GUI types on purpose -- screens arrive as plain data so the pure
// serialization is testable without a display server.

#include "diagnostics/PresentProvider.h"

#include <capability/adapter_capability.h>
#include <capability/adapter_enum.h>
#include <capability/capability_set.h>

#include <QJsonObject>
#include <QString>

#include <optional>
#include <vector>

namespace exosnap::observability {

// One display as Qt sees it. Deliberately separate from capability::DisplayHdrFacts,
// which is what DXGI sees: the two describe the same monitor from two different
// APIs and are matched by name where possible, never merged into one guess.
struct ScreenFacts {
    QString name;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    double device_pixel_ratio = 1.0;
    double refresh_hz = 0.0;
    bool primary = false;
};

// One audio endpoint. A local struct rather than the notifier's own snapshot type
// so this stays a pure serialization unit with no service dependency -- and so
// the endpoint ID, which is a machine-specific string nothing here needs, is left
// behind at the mapping boundary instead of being carried in and then filtered.
struct AudioEndpointFacts {
    QString name;
    bool is_default = false;
};

// The present-diagnostics provider's own state, which is what makes an absent
// present measurement explainable rather than merely missing.
struct PresentObservation {
    bool opt_in = false;
    bool elevated = false;
    bool available = false;
    // A real sample, only when `available`.
    std::optional<diagnostics::PresentSample> sample;
};

struct EnvironmentSnapshotInputs {
    // System-wide capability facts (OS build, NVENC probe, DXGI display facts).
    capability::CapabilitySet capabilities;

    // Per-adapter enumeration. `adapters_scanned` is false until the Diagnostics
    // hardware panel has been opened once -- an empty list before that is "not
    // scanned", not "no GPU", and the payload says which.
    bool adapters_scanned = false;
    std::vector<capability::AdapterInfo> adapters;
    std::vector<capability::AdapterEncoderCapability> adapter_capabilities; // parallel to `adapters`
    int active_adapter_index = -1;

    std::vector<ScreenFacts> screens;

    // Endpoints as the notifier last observed them.
    std::vector<AudioEndpointFacts> audio_inputs;
    std::vector<AudioEndpointFacts> audio_outputs;
    bool audio_observed = false;

    bool elevated = false;
    PresentObservation present;
};

[[nodiscard]] QJsonObject EnvironmentSnapshotToJson(const EnvironmentSnapshotInputs& inputs);

} // namespace exosnap::observability
