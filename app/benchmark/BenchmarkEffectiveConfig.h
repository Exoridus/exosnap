#pragma once

// The recording configuration a benchmark run ACTUALLY committed, not the one it
// was asked for.
//
// A previous A/B campaign compared two runs that had been launched with the same
// command line and were nevertheless recording differently: the Widgets path
// committed the CLI settings, while the Qt Quick path seeded
// OutputSettingsModel::Defaults() on its way to StartRecording. Comparing the
// flags could never have caught that. This module reads back the RecorderConfig
// the engine was handed (RecordingCoordinator::LastCommittedRecorderConfig) and
// reduces it to a normalized field list plus a digest, so two runs can be
// declared comparable — or rejected — mechanically.
//
// The vocabulary here is deliberately its own: short, stable tokens rather than
// the user-facing codec labels. A UI wording change must never alter a
// fingerprint, or every archived baseline stops matching new runs.

#include <QString>
#include <QStringList>

namespace exosnap::engine {
struct RecorderConfig;
}

namespace exosnap::benchmark {

struct EffectiveRecordingConfig {
    // False when no session was prepared or an attached host scene could not
    // be sampled. Unavailable configurations cannot establish a match, because
    // "no config" must not compare equal to "no config" and license an A/B pair.
    bool available = false;

    // Canonical "key=value" lines in a fixed order. Emitted into the report in
    // full: a fingerprint mismatch is only actionable if a reader can see which
    // field diverged.
    QStringList fields;

    // Truncated SHA-256 over the joined fields. Short enough to appear in a run
    // table, wide enough that two genuinely different configurations will not
    // collide in a campaign of a few dozen runs.
    QString fingerprint;
};

// Samples the current host scene on the control side. This does not identify
// the scene used by any particular recorded frame. Excludes transient handles,
// source identities, pointers, revisions and publication sequences.
[[nodiscard]] EffectiveRecordingConfig DescribeEffectiveConfig(const exosnap::engine::RecorderConfig& config);

[[nodiscard]] EffectiveRecordingConfig UnavailableEffectiveConfig();

} // namespace exosnap::benchmark
