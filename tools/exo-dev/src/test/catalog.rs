//! What a build tree registers with CTest, and whether its execution phases
//! let a phase selection mean anything.

use std::ffi::OsString;
use std::path::Path;

use serde_json::Value;

/// One execution phase. Every registered test declares exactly one, as a
/// `phase.<name>` label (see `cmake/exosnap_testing.cmake`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Phase {
    /// Nothing outside the process: no hardware, no network, no desktop.
    Hermetic,
    /// Real CPU work or real time. Deterministic in result, not duration.
    Cpu,
    /// Needs a graphics adapter. Without one these can only skip.
    Gpu,
    /// Needs an interactive desktop: a window, focus, a Qt surface.
    Desktop,
    /// Needs a disposable machine of its own.
    Vm,
    /// Needs a person to do something no API can do.
    Human,
}

impl Phase {
    pub const ALL: [Phase; 6] = [
        Phase::Hermetic,
        Phase::Cpu,
        Phase::Gpu,
        Phase::Desktop,
        Phase::Vm,
        Phase::Human,
    ];

    pub fn name(self) -> &'static str {
        match self {
            Phase::Hermetic => "hermetic",
            Phase::Cpu => "cpu",
            Phase::Gpu => "gpu",
            Phase::Desktop => "desktop",
            Phase::Vm => "vm",
            Phase::Human => "human",
        }
    }

    pub fn from_name(name: &str) -> Option<Phase> {
        Phase::ALL.into_iter().find(|phase| phase.name() == name)
    }
}

/// The `-LE` pattern for an excluded label. ctest reads `-LE` as a regular
/// expression, so an unanchored "live" also matches "live_verify" and
/// silently drops suites that query no hardware at all. A plain label word is
/// anchored. Anything else is the caller's own pattern and passes unchanged.
pub fn exclude_pattern(label: &str) -> String {
    let plain = !label.is_empty() && label.chars().all(|c| c.is_ascii_alphanumeric() || c == '_');
    if plain {
        format!("^{label}$")
    } else {
        label.to_string()
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CatalogTest {
    pub name: String,
    pub labels: Vec<String>,
    pub disabled: bool,
}

/// Reads `ctest --show-only=json-v1` output. That format is a documented
/// contract with names, labels and the DISABLED property. The human `-N`
/// listing has none of that structure. Every way the text can fail to be a
/// test list is a stated reason, never an empty list: a ctest that could not
/// list the tree is not a tree with no tests.
pub fn parse_catalog(text: &str) -> Result<Vec<CatalogTest>, String> {
    let document: Value = serde_json::from_str(text)
        .map_err(|error| format!("ctest --show-only output is not JSON: {error}"))?;
    let tests = match document.get("tests") {
        Some(Value::Array(tests)) => tests.as_slice(),
        Some(Value::Null) => &[],
        _ => return Err("ctest --show-only output has no test list".to_string()),
    };
    Ok(tests
        .iter()
        .map(|test| {
            let mut labels = Vec::new();
            let mut disabled = false;
            for property in test["properties"].as_array().into_iter().flatten() {
                match property["name"].as_str() {
                    Some("LABELS") => labels = strings(&property["value"]),
                    Some("DISABLED") => disabled = truthy(&property["value"]),
                    _ => {}
                }
            }
            CatalogTest {
                name: test["name"].as_str().unwrap_or_default().to_string(),
                labels,
                disabled,
            }
        })
        .collect())
}

fn strings(value: &Value) -> Vec<String> {
    match value {
        Value::Array(items) => items
            .iter()
            .filter_map(Value::as_str)
            .map(str::to_string)
            .collect(),
        Value::String(single) => vec![single.clone()],
        _ => Vec::new(),
    }
}

fn truthy(value: &Value) -> bool {
    match value {
        Value::Bool(flag) => *flag,
        Value::String(text) => !text.is_empty(),
        Value::Number(number) => number.as_f64().is_some_and(|n| n != 0.0),
        _ => false,
    }
}

/// The tests `build_dir` registers under `selection` (ctest's own `-R`, `-LE`
/// and `-L` arguments), read in `env`.
pub fn query(
    build_dir: &Path,
    config: &str,
    selection: &[String],
    env: &[(OsString, OsString)],
) -> Result<Vec<CatalogTest>, String> {
    let mut command = crate::process::command("ctest");
    command
        .arg("--test-dir")
        .arg(build_dir)
        .args(["-C", config, "--show-only=json-v1"])
        .args(selection);
    for (key, value) in env {
        command.env(key, value);
    }
    let (code, stdout) = crate::process::query(command)
        .map_err(|error| format!("ctest could not be started: {error}"))?;
    if code != 0 {
        return Err(format!("ctest --show-only exited {code}"));
    }
    parse_catalog(&stdout)
}

/// Every registered test declares exactly one known execution phase.
///
/// Checked per test, not as a total. Two totals can agree while one test
/// carries no phase and another carries two, and `--phase hermetic` would then
/// silently leave the first one out. Missing, duplicated and misspelled are
/// reported separately because they are three different mistakes in the CMake
/// beside the test.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct PhaseViolations {
    pub missing: Vec<String>,
    /// `name [phase.a, phase.b]`.
    pub multiple: Vec<String>,
    /// `name [phase.label]`.
    pub unknown: Vec<String>,
}

impl PhaseViolations {
    pub fn ok(&self) -> bool {
        self.missing.is_empty() && self.multiple.is_empty() && self.unknown.is_empty()
    }
}

/// A label counts as a phase declaration whatever its case, but only the
/// exact lower-case vocabulary is known: the selection is ctest's
/// case-sensitive `-L ^phase\.<name>$`, so `phase.Hermetic` would be declared
/// and still selected by nothing.
pub fn check_phases(tests: &[CatalogTest]) -> PhaseViolations {
    let mut violations = PhaseViolations::default();
    for test in tests {
        let phases: Vec<&String> = test
            .labels
            .iter()
            .filter(|label| label.to_ascii_lowercase().starts_with("phase."))
            .collect();
        if phases.is_empty() {
            violations.missing.push(test.name.clone());
            continue;
        }
        if phases.len() > 1 {
            let list: Vec<&str> = phases.iter().map(|p| p.as_str()).collect();
            violations
                .multiple
                .push(format!("{} [{}]", test.name, list.join(", ")));
        }
        for phase in phases {
            let known = phase
                .strip_prefix("phase.")
                .and_then(Phase::from_name)
                .is_some();
            if !known {
                violations.unknown.push(format!("{} [{phase}]", test.name));
            }
        }
    }
    violations
}

#[cfg(test)]
mod tests {
    use super::*;

    fn test(name: &str, labels: &[&str]) -> CatalogTest {
        CatalogTest {
            name: name.into(),
            labels: labels.iter().map(|l| l.to_string()).collect(),
            disabled: false,
        }
    }

    #[test]
    fn a_plain_label_is_anchored_and_a_pattern_passes_through() {
        assert_eq!(exclude_pattern("live"), "^live$");
        assert_eq!(exclude_pattern("live_verify"), "^live_verify$");
        assert_eq!(
            exclude_pattern("^(live|live_verify)$"),
            "^(live|live_verify)$"
        );
        assert_eq!(exclude_pattern("live.*"), "live.*");
        assert_eq!(exclude_pattern(""), "");
    }

    #[test]
    fn the_phase_vocabulary_round_trips() {
        for phase in Phase::ALL {
            assert_eq!(Phase::from_name(phase.name()), Some(phase));
        }
        assert_eq!(Phase::from_name("gpu-ish"), None);
        assert_eq!(Phase::from_name(""), None);
    }

    #[test]
    fn the_catalog_reads_names_labels_and_disabled() {
        let json = r#"{"kind":"ctestInfo","tests":[
            {"name":"a","properties":[{"name":"LABELS","value":["live","phase.gpu"]}]},
            {"name":"b","properties":[{"name":"LABELS","value":["phase.hermetic"]},
                                      {"name":"DISABLED","value":true}]},
            {"name":"c"}
        ]}"#;
        let tests = parse_catalog(json).unwrap();
        assert_eq!(tests.len(), 3);
        assert_eq!(tests[0].labels, vec!["live", "phase.gpu"]);
        assert!(!tests[0].disabled);
        assert!(tests[1].disabled);
        assert!(tests[2].labels.is_empty());
    }

    #[test]
    fn an_unreadable_catalog_is_a_reason_never_an_empty_list() {
        assert!(
            parse_catalog("not json")
                .unwrap_err()
                .contains("is not JSON")
        );
        assert_eq!(
            parse_catalog(r#"{"kind":"ctestInfo"}"#).unwrap_err(),
            "ctest --show-only output has no test list"
        );
        assert_eq!(parse_catalog(r#"{"tests":[]}"#).unwrap(), Vec::new());
    }

    #[test]
    fn missing_duplicated_and_misspelled_phases_are_told_apart() {
        let violations = check_phases(&[
            test("no_phase", &["quick"]),
            test("two_phases", &["phase.hermetic", "phase.gpu"]),
            test("typo", &["phase.hermitic"]),
            test("wrong_case", &["Phase.hermetic"]),
            test("fine", &["phase.hermetic", "live"]),
        ]);
        assert!(!violations.ok());
        assert_eq!(violations.missing, vec!["no_phase"]);
        assert_eq!(
            violations.multiple,
            vec!["two_phases [phase.hermetic, phase.gpu]"]
        );
        assert_eq!(
            violations.unknown,
            vec!["typo [phase.hermitic]", "wrong_case [Phase.hermetic]"]
        );
        assert!(check_phases(&[test("fine", &["phase.cpu"])]).ok());
    }
}
