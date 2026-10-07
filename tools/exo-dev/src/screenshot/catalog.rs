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
const ACCENTS_SOURCE: &str = "app/ui/theme/ExoSnapThemes.h";

/// The values the current source tree accepts.
#[derive(Clone, Debug)]
pub struct ProductValues {
    pub record_states: Vec<String>,
    pub accents: Vec<String>,
}

impl ProductValues {
    pub fn load(repo_root: &Path) -> anyhow::Result<ProductValues> {
        let read = |relative: &str| {
            std::fs::read_to_string(repo_root.join(relative))
                .with_context(|| format!("could not read {relative}"))
        };
        let record_states = record_states_from(&read(RECORD_STATES_SOURCE)?);
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

/// Edit workspace fixtures. `edit-default` matches no branch in the app and
/// therefore renders the default fixture.
const EDIT_SCENARIOS: &[&str] = &[
    "edit-default",
    "edit-trimmed",
    "edit-timeline-multitrack",
    "edit-timeline-loading",
    "edit-timeline-unavailable",
    "edit-export-running",
    "edit-export-done",
    "edit-export-failed",
    "edit-report-warning",
    "edit-long-filename",
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

/// Simple and Expert, each as the window, as the whole page and as the whole
/// page with every collapsed element open.
fn arrangements(page: Page) -> Vec<ShotSpec> {
    let mut specs = Vec::new();
    for expert in [false, true] {
        for (full_page, expand_all) in [(false, false), (true, false), (true, true)] {
            specs.push(ShotSpec {
                page: Some(page),
                expert,
                full_page,
                expand_all,
                ..ShotSpec::default()
            });
        }
    }
    specs
}

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
        "settings" => arrangements(Page::Settings)
            .into_iter()
            .map(named)
            .collect(),
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
        "edit" => EDIT_SCENARIOS
            .iter()
            .map(|scenario| {
                named(ShotSpec {
                    edit: Some(scenario.to_string()),
                    ..page(Page::Edit)
                })
            })
            .collect(),
        "diagnostics" => {
            let mut shots: Vec<_> = arrangements(Page::Diagnostics)
                .into_iter()
                .map(named)
                .collect();
            shots.push(named(ShotSpec {
                diag: Some("issues".to_string()),
                ..page(Page::Diagnostics)
            }));
            shots.extend(DIAGNOSTICS_LIVE.iter().map(|kind| {
                named(ShotSpec {
                    diag_live: Some(kind.to_string()),
                    ..page(Page::Diagnostics)
                })
            }));
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
