//! The scenario registry, organised by product capability.

pub mod app;
pub mod audio;
pub mod capture;
mod chocolatey_worker;
pub mod clean;
#[cfg(test)]
mod command_names;
pub mod common;
pub mod display;
pub mod dist;
pub mod env;
pub mod fse;
pub mod handoff;
pub mod install;
pub mod journey;
pub mod overlay;
pub mod present;
pub mod preview;
pub mod record;
pub mod schema;
pub mod update;
pub mod visual;
pub mod webcam;

use crate::scenario::Scenario;

/// Runs the Chocolatey rehearsal for one prepared package tree against one
/// local MSI. This is the distribution-side entry point: it does not need the
/// lane runner or a checkout, only the frozen package and its installer, and
/// it writes the same result document the release lane produces.
pub fn rehearse_chocolatey(
    package_source: &std::path::Path,
    installer: &std::path::Path,
    out: &std::path::Path,
) -> anyhow::Result<serde_json::Value> {
    use anyhow::Context as _;
    std::fs::create_dir_all(out)?;
    let (hash, _) = crate::bundle::sha256_file(installer)?;
    let staging = out.join("staging");
    let evidence = out.join("evidence");
    let document = match chocolatey_worker::run_rehearsal(
        &staging,
        package_source,
        installer,
        &hash,
        &evidence,
    ) {
        Ok(result) => serde_json::to_value(&result)?,
        Err(error) => {
            let fatal = serde_json::json!({
                "ok": false,
                "msiPath": installer.display().to_string(),
                "msiSha256": hash,
                "restoreRan": false,
                "steps": [],
                "fatal": error.to_string(),
            });
            crate::write_json(&out.join("chocolatey-rehearsal.json"), &fatal)?;
            return Err(error).context("the Chocolatey rehearsal could not run");
        }
    };
    update::chocolatey_verdict(&document)
        .map_err(|stop| anyhow::anyhow!("Chocolatey rehearsal verdict failed: {stop:?}"))?;
    crate::write_json(&out.join("chocolatey-rehearsal.json"), &document)?;
    Ok(document)
}

pub fn registry() -> Vec<Scenario> {
    let mut all = Vec::new();
    all.extend(dist::scenarios());
    all.extend(env::scenarios());
    all.extend(clean::scenarios());
    all.extend(fse::scenarios());
    all.extend(app::scenarios());
    all.extend(audio::scenarios());
    all.extend(capture::scenarios());
    all.extend(display::scenarios());
    all.extend(install::scenarios());
    all.extend(journey::scenarios());
    all.extend(overlay::scenarios());
    all.extend(present::scenarios());
    all.extend(preview::scenarios());
    all.extend(record::scenarios());
    all.extend(schema::scenarios());
    all.extend(update::scenarios());
    all.extend(handoff::scenarios());
    all.extend(visual::scenarios());
    all.extend(webcam::scenarios());
    all
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashSet;

    #[test]
    fn ids_are_unique_and_well_formed() {
        let mut seen = HashSet::new();
        for s in registry() {
            assert!(seen.insert(s.id), "duplicate scenario id {}", s.id);
            assert!(
                s.id.contains('.')
                    && s.id.chars().all(|c| c.is_ascii_lowercase()
                        || c.is_ascii_digit()
                        || c == '.'
                        || c == '-'),
                "{}",
                s.id
            );
            assert!(s.revision >= 1);
            assert!(!s.contract.is_empty() && !s.title.is_empty());
            assert!(!s.also.contains(&s.lane));
        }
    }

    #[test]
    fn classes_are_consistent_with_their_requirements() {
        use crate::capability::Capability;
        use crate::scenario::ScenarioClass;
        for s in registry() {
            match s.class {
                ScenarioClass::Installer => assert!(
                    s.requires.contains(&Capability::DisposableOs),
                    "{} mutates the OS without a disposable one",
                    s.id
                ),
                ScenarioClass::Regression => assert!(
                    !s.requires.contains(&Capability::DisposableOs)
                        && !s.requires.contains(&Capability::Operator),
                    "{} is a regression check yet needs more than known inputs",
                    s.id
                ),
                _ => {}
            }
        }
    }

    #[test]
    fn development_lanes_never_own_release_evidence() {
        for s in registry() {
            if matches!(
                s.lane,
                crate::scenario::Lane::Quick | crate::scenario::Lane::Preflight
            ) {
                assert_eq!(
                    s.tier,
                    crate::plan::Tier::Recommended,
                    "{} is a development check",
                    s.id
                );
            }
        }
    }

    #[test]
    fn install_lane_requires_disposable_admin_desktop() {
        // A capability check qualifies the install machine and installs nothing.
        let install: Vec<_> = registry()
            .into_iter()
            .filter(|s| {
                s.lane == crate::scenario::Lane::CiInstall
                    && s.class != crate::scenario::ScenarioClass::Capability
            })
            .collect();
        assert!(!install.is_empty(), "install lane has no scenarios");
        for scenario in install {
            let requirements: Vec<_> = scenario.requires.iter().map(|c| c.name()).collect();
            for required in ["windows", "admin", "interactive-desktop", "disposable-os"] {
                assert!(
                    requirements.contains(&required),
                    "{} lacks {required}",
                    scenario.id
                );
            }
        }
    }

    #[test]
    fn gpu_lane_has_a_recording_oracle() {
        let scenario = registry()
            .into_iter()
            .find(|s| s.id == "record.ddx-h264-mkv")
            .expect("the GPU lane needs a decoded recording scenario");
        assert_eq!(scenario.lane, crate::scenario::Lane::Gpu);
        assert_eq!(scenario.tier, crate::plan::Tier::Required);
        for capability in [
            crate::capability::Capability::DxgiDuplication,
            crate::capability::Capability::Nvenc,
            crate::capability::Capability::Ffprobe,
            crate::capability::Capability::Ffmpeg,
        ] {
            assert!(scenario.requires.contains(&capability));
        }
        let window = registry()
            .into_iter()
            .find(|s| s.id == "record.wgc-hevc-mp4")
            .expect("the GPU lane needs a WGC window scenario");
        assert_eq!(window.lane, crate::scenario::Lane::Gpu);
        assert!(
            window
                .requires
                .contains(&crate::capability::Capability::Wgc)
        );
    }
}
