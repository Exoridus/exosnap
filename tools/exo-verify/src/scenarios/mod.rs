//! The scenario registry, organised by product capability.

pub mod app;
pub mod audio;
pub mod capture;
pub mod common;
pub mod display;
pub mod dist;
pub mod env;
pub mod fse;
pub mod install;
pub mod journey;
pub mod present;
pub mod record;
pub mod schema;
pub mod update;
pub mod visual;

use crate::scenario::Scenario;

pub fn registry() -> Vec<Scenario> {
    let mut all = Vec::new();
    all.extend(dist::scenarios());
    all.extend(env::scenarios());
    all.extend(fse::scenarios());
    all.extend(app::scenarios());
    all.extend(audio::scenarios());
    all.extend(capture::scenarios());
    all.extend(display::scenarios());
    all.extend(install::scenarios());
    all.extend(journey::scenarios());
    all.extend(present::scenarios());
    all.extend(record::scenarios());
    all.extend(schema::scenarios());
    all.extend(update::scenarios());
    all.extend(visual::scenarios());
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
            assert!(!s.claim.is_empty() && !s.title.is_empty());
            assert!(!s.also.contains(&s.lane));
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
        let install: Vec<_> = registry()
            .into_iter()
            .filter(|s| s.lane == crate::scenario::Lane::CiInstall)
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
