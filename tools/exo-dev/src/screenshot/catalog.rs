//! Named screenshot sets and the product values a request is checked against.
//!
//! The app renders an unknown record state as a plain page and an unknown
//! accent in the default accent, and still exits 0. Both lists are therefore
//! read from the product sources that define them, so a typo is refused here
//! instead of producing a plausible screenshot of the wrong thing.

use std::path::Path;

use anyhow::{Context, bail};

use super::invocation::{Page, Popup, ShotSpec};

const RECORD_STATES_SOURCE: &str = "app/visual_tests/RecordVisualStateNames.h";
const EDIT_STATES_SOURCE: &str = "app/visual_tests/EditVisualStateNames.h";
const ACCENTS_SOURCE: &str = "app/ui/theme/ExoSnapThemes.h";

/// The values the current source tree accepts.
#[derive(Clone, Debug)]
pub struct ProductValues {
    pub record_states: Vec<String>,
    pub edit_states: Vec<String>,
    pub accents: Vec<String>,
}

impl ProductValues {
    pub fn load(repo_root: &Path) -> anyhow::Result<ProductValues> {
        let read = |relative: &str| {
            std::fs::read_to_string(repo_root.join(relative))
                .with_context(|| format!("could not read {relative}"))
        };
        let record_states = record_states_from(&read(RECORD_STATES_SOURCE)?);
        let edit_states = record_states_from(&read(EDIT_STATES_SOURCE)?);
        if edit_states.is_empty() {
            bail!("no editor states found in {EDIT_STATES_SOURCE}");
        }
        let accents = accents_from(&read(ACCENTS_SOURCE)?);
        if record_states.len() < 3 || accents.is_empty() {
            bail!(
                "found {} record state(s) in {RECORD_STATES_SOURCE} and {} accent(s) in \
                 {ACCENTS_SOURCE}; the source layout changed and this reader needs updating",
                record_states.len(),
                accents.len()
            );
        }
        Ok(ProductValues {
            record_states,
            edit_states,
            accents,
        })
    }

    pub fn check(&self, spec: &ShotSpec, accents: &[String]) -> anyhow::Result<()> {
        if let Some(state) = &spec.record_state
            && !self.record_states.contains(state)
        {
            bail!(
                "unknown record state '{state}'. Known: {}",
                self.record_states.join(", ")
            );
        }
        for accent in accents {
            if !self.accents.contains(accent) {
                bail!(
                    "unknown accent '{accent}'. Known: {}",
                    self.accents.join(", ")
                );
            }
        }
        if let Some(scenario) = &spec.edit
            && !self.edit_states.contains(scenario)
        {
            bail!(
                "unknown edit fixture '{scenario}'. Known: {}",
                self.edit_states.join(", ")
            );
        }
        Ok(())
    }
}

fn record_states_from(text: &str) -> Vec<String> {
    regex::Regex::new(r#"constexpr const char\*\s+k\w+\s*=\s*"([a-z0-9-]+)""#)
        .unwrap()
        .captures_iter(text)
        .map(|c| c[1].to_string())
        .collect()
}

/// The first string of every entry in the `kExoAccents` array, which is the id.
fn accents_from(text: &str) -> Vec<String> {
    let Some(start) = text.find("kExoAccents") else {
        return Vec::new();
    };
    let body = &text[start..];
    let end = body.find("}};").unwrap_or(body.len());
    regex::Regex::new(r#"\{\s*"([a-z0-9-]+)""#)
        .unwrap()
        .captures_iter(&body[..end])
        .map(|c| c[1].to_string())
        .collect()
}

pub const SET_NAMES: &[&str] = &[
    "pages",
    "settings",
    "profile",
    "workspace",
    "record",
    "picker",
    "edit",
    "diagnostics",
    "logs",
    "dialogs",
    "popups",
    "overlays",
    "hud",
    "all",
];

const DIAGNOSTICS_LIVE: &[&str] = &[
    "healthy",
    "degraded",
    "paused",
    "after-stop",
    "in-depth",
    "opt-in-unelevated",
    "present-no-data",
];

/// In-window surfaces raised over the page.
const OVERLAY_STATES: &[&str] = &[
    "whats-new",
    "whats-new-post-update",
    "recovery",
    "recovery-multi",
    "recording-error",
    "crash-report",
    "crash-report-recording",
    "crash-report-no-dump",
    "crash-report-no-folder",
    "crash-report-remember",
    "crash-report-expanded",
    "crash-report-long",
];

/// The capture-excluded desktop windows: recording HUD variants, one toast
/// per advisory status and the notification hub. Their overlay grabs make up
/// the composed overlay sheet.
pub const HUD_STATES: &[&str] = &[
    "region-selection-empty",
    "region-selection-drawn",
    "hud-recording",
    "hud-paused",
    "hud-warning",
    "hud-countdown",
    "hud-diagnostics",
    "hud-diagnostics-technical",
    "hud-controls",
    "hud-all",
    "toast-success",
    "toast-caution",
    "toast-error",
    "toast-info",
    "notifications",
];

/// The shots of one named set, each with its file-name stem.
pub fn set(name: &str, values: &ProductValues) -> anyhow::Result<Vec<(String, ShotSpec)>> {
    let page = |page: Page| ShotSpec {
        page: Some(page),
        ..ShotSpec::default()
    };
    let named = |spec: ShotSpec| (spec.derived_name(), spec);

    let shots = match name {
        "pages" => [
            Page::Record,
            Page::Edit,
            Page::Settings,
            Page::Diagnostics,
            Page::Logs,
            Page::About,
        ]
        .into_iter()
        .map(|p| named(page(p)))
        .collect(),
        "settings" => [false, true]
            .into_iter()
            .map(|full_page| {
                named(ShotSpec {
                    full_page,
                    ..page(Page::Settings)
                })
            })
            .collect(),
        "profile" => {
            let mut shots = Vec::new();
            for section in [
                "preset",
                "format",
                "quality",
                "audio",
                "microphone",
                "audio-encoding",
                "output",
                "webcam",
            ] {
                for expert in [false, true] {
                    let arrangement = ShotSpec {
                        profile: Some(section.to_string()),
                        expert,
                        collapse_all: true,
                        ..page(Page::Settings)
                    };
                    let full_pages: &[bool] =
                        if ["preset", "microphone", "audio"].contains(&section) {
                            &[false, true]
                        } else {
                            &[false]
                        };
                    for &full_page in full_pages {
                        let shot = ShotSpec {
                            full_page,
                            ..arrangement.clone()
                        };
                        shots.push(named(shot.clone()));
                        if ["microphone", "webcam", "audio"].contains(&section) {
                            shots.push(named(ShotSpec {
                                expand_all: true,
                                collapse_all: false,
                                ..shot
                            }));
                        }
                    }
                }
            }
            shots
        }
        "workspace" => {
            let mut shots: Vec<_> = ["overlay-menu", "gamepad", "gamepad-hidden"]
                .into_iter()
                .map(|panel| {
                    named(ShotSpec {
                        record_panel: Some(panel.to_string()),
                        record_state: Some("ready".to_string()),
                        ..page(Page::Record)
                    })
                })
                .collect();
            for (state, panel) in [
                ("recording", "overlay-menu"),
                ("paused", "overlay-menu"),
                ("recording", "gamepad-hidden"),
            ] {
                shots.push(named(ShotSpec {
                    record_panel: Some(panel.to_string()),
                    record_state: Some(state.to_string()),
                    ..page(Page::Record)
                }));
            }
            shots
        }
        "record" => {
            let mut shots: Vec<_> = values
                .record_states
                .iter()
                .map(|state| {
                    named(ShotSpec {
                        record_state: Some(state.clone()),
                        ..page(Page::Record)
                    })
                })
                .collect();
            shots.push(named(ShotSpec {
                record_state: Some("ready".to_string()),
                countdown_menu: true,
                ..page(Page::Record)
            }));
            shots
        }
        "picker" => [
            ("single-display", Popup::SourcePicker),
            ("displays-only", Popup::SourcePicker),
            ("few-windows", Popup::SourcePicker),
            ("few-windows", Popup::SourcePickerWindows),
            ("many-windows", Popup::SourcePicker),
            ("many-windows", Popup::SourcePickerWindows),
            ("many-windows", Popup::SourcePickerSearch),
            ("many-windows", Popup::SourcePickerFilters),
            ("no-targets", Popup::SourcePicker),
            ("no-targets", Popup::SourcePickerWindows),
        ]
        .into_iter()
        .map(|(scenario, popup)| {
            named(ShotSpec {
                popup: Some(popup),
                sources: Some(scenario.to_string()),
                ..page(Page::Record)
            })
        })
        .collect(),
        "edit" => values
            .edit_states
            .iter()
            .map(|scenario| {
                named(ShotSpec {
                    edit: Some(scenario.to_string()),
                    ..page(Page::Edit)
                })
            })
            .collect(),
        "diagnostics" => {
            let mut shots = Vec::new();
            for expanded in [false, true] {
                let arrangement = ShotSpec {
                    expand_all: expanded,
                    collapse_all: !expanded,
                    ..page(Page::Diagnostics)
                };
                for full_page in [false, true] {
                    shots.push(named(ShotSpec {
                        full_page,
                        ..arrangement.clone()
                    }));
                }
                shots.push(named(ShotSpec {
                    diag: Some("issues".to_string()),
                    ..arrangement.clone()
                }));
                shots.extend(DIAGNOSTICS_LIVE.iter().map(|kind| {
                    named(ShotSpec {
                        diag_live: Some(kind.to_string()),
                        ..arrangement.clone()
                    })
                }));
            }
            shots
        }
        "logs" => {
            let mut shots = vec![named(page(Page::Logs))];
            shots.extend(["empty", "long-message"].iter().map(|scenario| {
                named(ShotSpec {
                    log: Some(scenario.to_string()),
                    ..page(Page::Logs)
                })
            }));
            shots
        }
        "dialogs" => {
            let mut shots = vec![named(ShotSpec {
                record_state: Some("recording".to_string()),
                dialog: Some("close-guard".to_string()),
                ..ShotSpec::default()
            })];
            shots.extend(
                ["preset-delete", "preset-rename", "preset-save-as"]
                    .iter()
                    .map(|dialog| {
                        named(ShotSpec {
                            dialog: Some(dialog.to_string()),
                            ..page(Page::Settings)
                        })
                    }),
            );
            shots
        }
        "popups" => vec![
            named(ShotSpec {
                popup: Some(Popup::SourcePicker),
                sources: Some("many-windows".to_string()),
                ..page(Page::Record)
            }),
            named(ShotSpec {
                popup: Some(Popup::NotificationHub),
                ..page(Page::Record)
            }),
            named(ShotSpec {
                popup: Some(Popup::NotificationHub),
                notifications: Some("many".to_string()),
                ..page(Page::Record)
            }),
        ],
        "overlays" | "hud" => (if name == "overlays" {
            OVERLAY_STATES
        } else {
            HUD_STATES
        })
        .iter()
        .map(|state| {
            named(ShotSpec {
                overlay_state: Some(state.to_string()),
                ..page(Page::Record)
            })
        })
        .collect(),
        "all" => {
            // Sets overlap (the bare Settings page is in `pages` and in
            // `settings`), and a repeated name would overwrite its own file.
            let mut shots: Vec<(String, ShotSpec)> = Vec::new();
            for name in SET_NAMES.iter().filter(|n| **n != "all") {
                for shot in set(name, values)? {
                    if !shots.iter().any(|(existing, _)| *existing == shot.0) {
                        shots.push(shot);
                    }
                }
            }
            shots
        }
        other => bail!("unknown set '{other}'. Known: {}", SET_NAMES.join(", ")),
    };
    Ok(shots)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn values() -> ProductValues {
        ProductValues {
            record_states: vec![
                "ready".to_string(),
                "recording".to_string(),
                "paused".to_string(),
            ],
            edit_states: record_states_from(include_str!(
                "../../../../app/visual_tests/EditVisualStateNames.h"
            )),
            accents: vec!["aqua".to_string(), "violet".to_string()],
        }
    }

    #[test]
    fn record_states_are_read_from_the_shared_constants() {
        let text = r#"
            inline constexpr const char* kNoSource = "no-source";
            inline constexpr const char* kRecordingAudioDegraded = "recording-audio-degraded";
        "#;
        assert_eq!(
            record_states_from(text),
            ["no-source", "recording-audio-degraded"]
        );
    }

    #[test]
    fn accents_are_the_first_string_of_each_entry() {
        let text = r##"
            inline constexpr std::array<ExoAccent, 2> kExoAccents = {{
                {
                    "aqua",
                    "Aqua",
                    "#9BD9D2",
                },
                {"violet", "Violet", "#000000"},
            }};
            inline constexpr int kOther = {{ {"not-an-accent"} }};
        "##;
        assert_eq!(accents_from(text), ["aqua", "violet"]);
    }

    #[test]
    fn the_real_sources_still_parse() {
        let repo_root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
        let values = ProductValues::load(&repo_root).unwrap();
        assert!(values.record_states.iter().any(|s| s == "recording"));
        assert!(values.accents.iter().any(|a| a == "aqua"));
    }

    #[test]
    fn unknown_values_are_refused() {
        let spec = ShotSpec {
            record_state: Some("warning".to_string()),
            ..ShotSpec::default()
        };
        assert!(values().check(&spec, &["aqua".to_string()]).is_err());
        assert!(
            values()
                .check(&ShotSpec::default(), &["teal".to_string()])
                .is_err()
        );
        assert!(
            values()
                .check(&ShotSpec::default(), &["violet".to_string()])
                .is_ok()
        );
    }

    #[test]
    fn edit_fixtures_cover_the_workspace_and_reject_unknown_states() {
        let shots = set("edit", &values()).unwrap();
        for scenario in [
            "edit-empty",
            "edit-multitrack",
            "edit-monitoring",
            "edit-panels-pinned",
            "edit-export-review",
        ] {
            assert!(
                shots
                    .iter()
                    .any(|(_, shot)| shot.edit.as_deref() == Some(scenario)),
                "{scenario}"
            );
        }
        let spec = ShotSpec {
            edit: Some("edit-misspelled".into()),
            ..ShotSpec::default()
        };
        assert!(values().check(&spec, &[]).is_err());
    }

    #[test]
    fn edit_is_a_page_and_its_fixtures_select_it() {
        let pages = set("pages", &values()).unwrap();
        assert!(
            pages
                .iter()
                .any(|(name, spec)| { name == "edit_default" && spec.page == Some(Page::Edit) })
        );
        for (name, spec) in set("edit", &values()).unwrap() {
            assert_eq!(spec.page, Some(Page::Edit));
            assert!(name.starts_with("edit_"));
        }
    }

    #[test]
    fn review_sets_cover_current_surfaces_without_obsolete_routes() {
        let values = values();
        let edits = set("edit", &values).unwrap();
        assert!(
            !edits
                .iter()
                .any(|(_, shot)| shot.edit.as_deref() == Some("edit-transitions-panel"))
        );
        assert!(
            edits
                .iter()
                .any(|(_, shot)| shot.edit.as_deref() == Some("edit-folders-empty"))
        );
        for (_, shot) in set("dialogs", &values).unwrap() {
            if shot
                .dialog
                .as_deref()
                .is_some_and(|dialog| dialog.starts_with("preset-"))
            {
                assert_eq!(shot.page, Some(Page::Settings));
            }
        }
        assert!(
            set("profile", &values)
                .unwrap()
                .iter()
                .all(|(_, shot)| shot.page == Some(Page::Settings))
        );
        for (_, shot) in set("workspace", &values).unwrap() {
            assert!(matches!(
                shot.record_panel.as_deref(),
                Some("overlay-menu" | "gamepad" | "gamepad-hidden")
            ));
        }
        let diagnostics = set("diagnostics", &values).unwrap();
        assert!(diagnostics.iter().all(|(_, shot)| !shot.expert));
        for live in DIAGNOSTICS_LIVE {
            for expanded in [false, true] {
                assert!(diagnostics.iter().any(|(_, shot)| {
                    shot.diag_live.as_deref() == Some(live) && shot.expand_all == expanded
                }));
            }
        }
    }
    #[test]
    fn overlays_include_both_release_note_modes() {
        let shots = set("overlays", &values()).unwrap();
        for state in ["whats-new", "whats-new-post-update"] {
            assert!(
                shots
                    .iter()
                    .any(|(_, shot)| shot.overlay_state.as_deref() == Some(state)),
                "{state}"
            );
        }
    }

    #[test]
    fn desktop_overlays_include_both_region_drafts() {
        let shots = set("hud", &values()).unwrap();
        for state in ["region-selection-empty", "region-selection-drawn"] {
            assert!(
                shots
                    .iter()
                    .any(|(_, shot)| shot.overlay_state.as_deref() == Some(state)),
                "{state}"
            );
        }
    }

    #[test]
    fn every_set_has_unique_names() {
        for name in SET_NAMES {
            let shots = set(name, &values()).unwrap();
            assert!(!shots.is_empty(), "{name} is empty");
            let mut names: Vec<_> = shots.iter().map(|(n, _)| n.clone()).collect();
            names.sort();
            let before = names.len();
            names.dedup();
            assert_eq!(before, names.len(), "{name} repeats a shot name");
        }
    }
}
