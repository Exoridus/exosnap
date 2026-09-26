//! Compares the ruleset payloads this repository declares under
//! `.github/rulesets/*.json` against what GitHub actually enforces, and
//! reports the difference. Branch and tag protection lives server-side,
//! invisible to review and drifting without a commit; this is the only
//! place that reads both sides and says whether they still agree. It never
//! writes: applying a ruleset is a separately authorized act, and the
//! README next to the payloads names the command that does it.
//!
//! Matched on target plus the refs a ruleset applies to, never on the name:
//! a ruleset's name is editable in the web UI, while what it protects is the
//! thing that has to stay true.

use std::fmt::Display;
use std::path::{Path, PathBuf};

use anyhow::Context;
use serde_json::Value;

use crate::process;

/// Where the live rulesets are read from.
pub enum LiveSource {
    /// `gh api repos/:owner/:repo/rulesets`, then one call per entry: the
    /// index omits `rules` and `bypass_actors`, so each ruleset has to be
    /// read again.
    GhApi,
    /// The array `gh api repos/:owner/:repo/rulesets` returns, with every
    /// entry already expanded, read from a file instead of the network.
    File(PathBuf),
}

pub struct FieldComparison {
    pub field: &'static str,
    pub current: String,
    pub wanted: String,
}

impl FieldComparison {
    pub fn differs(&self) -> bool {
        self.current != self.wanted
    }
}

pub struct EntryComparison {
    pub file: String,
    pub key: String,
    /// Whether a live ruleset protecting the same refs was found at all.
    /// `fields` is empty when this is false.
    pub matched: bool,
    pub fields: Vec<FieldComparison>,
}

pub struct UndeclaredLive {
    pub name: String,
    pub key: String,
}

pub struct RulesetReport {
    pub entries: Vec<EntryComparison>,
    pub undeclared: Vec<UndeclaredLive>,
    pub differences: Vec<String>,
}

impl RulesetReport {
    pub fn ok(&self) -> bool {
        self.differences.is_empty()
    }
}

/// The outcome of a run. `Unreadable` covers every case the intended state or
/// the live state could not be established at all: no desired directory, no
/// `*.json` payload under it, or a live-state read failure (a missing file, a
/// parse failure, or `gh` missing or failing). It is deliberately not an
/// `Err`: a caller maps it to its own exit code rather than treating it the
/// same as an unexpected bug.
pub enum CheckOutcome {
    Compared(RulesetReport),
    Unreadable(String),
}

pub fn check(declared_dir: &Path, live: LiveSource) -> anyhow::Result<CheckOutcome> {
    if !declared_dir.is_dir() {
        return Ok(CheckOutcome::Unreadable(format!(
            "No ruleset directory at '{}'.",
            declared_dir.display()
        )));
    }

    let mut paths: Vec<PathBuf> = std::fs::read_dir(declared_dir)
        .with_context(|| format!("could not list {}", declared_dir.display()))?
        .filter_map(|entry| entry.ok())
        .map(|entry| entry.path())
        .filter(|path| path.extension().and_then(|ext| ext.to_str()) == Some("json"))
        .collect();
    paths.sort();

    let mut desired = Vec::new();
    for path in &paths {
        let text = std::fs::read_to_string(path)
            .with_context(|| format!("could not read {}", path.display()))?;
        let payload: Value = serde_json::from_str(&text)
            .with_context(|| format!("could not parse {} as JSON", path.display()))?;
        let name = path
            .file_name()
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_default();
        desired.push((name, payload));
    }

    if desired.is_empty() {
        return Ok(CheckOutcome::Unreadable(format!(
            "No ruleset payloads under '{}'.",
            declared_dir.display()
        )));
    }

    let live = match live {
        LiveSource::File(path) => read_live_file(&path),
        LiveSource::GhApi => read_live_gh_api(),
    };
    let live = match live {
        Ok(live) => live,
        Err(message) => {
            return Ok(CheckOutcome::Unreadable(format!(
                "{message}\nCould not read the live rulesets. This is not a pass."
            )));
        }
    };

    Ok(CheckOutcome::Compared(compare(&desired, &live)))
}

/// Renders a report the way the comparison prints it: every declared entry's
/// header, always; the field-by-field comparison, only when `quiet` is false;
/// then the difference count and, when there is one, the reminder that
/// applying a ruleset is separately authorized.
pub fn render(report: &RulesetReport, quiet: bool) -> String {
    let mut out = String::new();

    for entry in &report.entries {
        out.push('\n');
        out.push_str(&format!("{}  [{}]\n", entry.file, entry.key));
        if !entry.matched {
            out.push_str("  no live ruleset protects these refs\n");
            continue;
        }
        if quiet {
            continue;
        }
        for field in &entry.fields {
            let marker = if field.differs() { '!' } else { ' ' };
            out.push_str(&format!(
                "  {marker} {:<38} current: {}\n",
                field.field, field.current
            ));
            if field.differs() {
                out.push_str(&format!("    {:<38} desired: {}\n", "", field.wanted));
            }
        }
    }

    for undeclared in &report.undeclared {
        out.push('\n');
        out.push_str(&format!(
            "undeclared live ruleset '{}'  [{}]\n",
            undeclared.name, undeclared.key
        ));
    }

    out.push('\n');
    out.push_str(&"-".repeat(60));
    out.push('\n');
    if report.differences.is_empty() {
        out.push_str("Rulesets match what the repository declares.\n");
    } else {
        out.push_str(&format!("{} difference(s):\n", report.differences.len()));
        for difference in &report.differences {
            out.push_str(&format!("  {difference}\n"));
        }
        out.push('\n');
        out.push_str(
            "Applying a ruleset is a separately authorized act; see .github/rulesets/README.md.\n",
        );
    }
    out
}

// ---------------------------------------------------------------------------
// Live state
// ---------------------------------------------------------------------------

fn read_live_file(path: &Path) -> Result<Vec<Value>, String> {
    let text = std::fs::read_to_string(path)
        .map_err(|error| format!("cannot read '{}': {error}", path.display()))?;
    let value: Value = serde_json::from_str(&text)
        .map_err(|error| format!("could not parse '{}' as JSON: {error}", path.display()))?;
    Ok(match value {
        Value::Array(items) => items,
        other => vec![other],
    })
}

fn read_live_gh_api() -> Result<Vec<Value>, String> {
    let index_text = run_gh(&["api", "repos/:owner/:repo/rulesets"])?;
    let index: Vec<Value> = serde_json::from_str(&index_text)
        .map_err(|error| format!("could not parse the rulesets index: {error}"))?;

    let mut live = Vec::with_capacity(index.len());
    for entry in &index {
        let id = entry
            .get("id")
            .ok_or_else(|| "a ruleset index entry has no id".to_string())?;
        let detail_text = run_gh(&["api", &format!("repos/:owner/:repo/rulesets/{id}")])?;
        let detail: Value = serde_json::from_str(&detail_text)
            .map_err(|error| format!("could not parse ruleset {id}: {error}"))?;
        live.push(detail);
    }
    Ok(live)
}

fn run_gh(args: &[&str]) -> Result<String, String> {
    let mut command = process::command("gh");
    command.args(args);
    match command.output() {
        Ok(output) if output.status.success() => {
            Ok(String::from_utf8_lossy(&output.stdout).into_owned())
        }
        Ok(output) => {
            let mut combined = String::from_utf8_lossy(&output.stdout).into_owned();
            combined.push_str(&String::from_utf8_lossy(&output.stderr));
            Err(format!("gh {} failed: {}", args.join(" "), combined.trim()))
        }
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            Err("gh is not installed or not on PATH.".to_string())
        }
        Err(error) => Err(format!("could not run gh: {error}")),
    }
}

// ---------------------------------------------------------------------------
// Comparison
// ---------------------------------------------------------------------------

fn compare(desired: &[(String, Value)], live: &[Value]) -> RulesetReport {
    let mut entries = Vec::new();
    let mut differences = Vec::new();

    for (file, wanted) in desired {
        let key = match_key(wanted);
        let current = live.iter().find(|candidate| match_key(candidate) == key);
        let entry = compare_entry(file, &key, wanted, current);
        if !entry.matched {
            differences.push(format!("{file} :: the repository has no ruleset for {key}"));
        } else {
            for field in &entry.fields {
                if field.differs() {
                    differences.push(format!(
                        "{file} :: {} : current '{}', desired '{}'",
                        field.field, field.current, field.wanted
                    ));
                }
            }
        }
        entries.push(entry);
    }

    let mut undeclared = Vec::new();
    for set in live {
        let key = match_key(set);
        let declared = desired.iter().any(|(_, wanted)| match_key(wanted) == key);
        if declared {
            continue;
        }
        let name = set
            .get("name")
            .and_then(Value::as_str)
            .unwrap_or_default()
            .to_string();
        differences.push(format!(
            "live :: ruleset '{name}' for {key} is not declared under .github/rulesets"
        ));
        undeclared.push(UndeclaredLive { name, key });
    }

    RulesetReport {
        entries,
        undeclared,
        differences,
    }
}

fn compare_entry(
    file: &str,
    key: &str,
    wanted: &Value,
    current: Option<&Value>,
) -> EntryComparison {
    let Some(current) = current else {
        return EntryComparison {
            file: file.to_string(),
            key: key.to_string(),
            matched: false,
            fields: Vec::new(),
        };
    };

    let mut fields = Vec::new();
    fields.push(field(
        "enforcement",
        current.get("enforcement").and_then(Value::as_str),
        wanted.get("enforcement").and_then(Value::as_str),
    ));
    fields.push(field_str(
        "rule types",
        rule_types(current),
        rule_types(wanted),
    ));
    fields.push(field_str(
        "bypass actors",
        format_bypass(current),
        format_bypass(wanted),
    ));

    let current_checks = rule_parameters(current, "required_status_checks");
    let wanted_checks = rule_parameters(wanted, "required_status_checks");
    if current_checks.is_some() || wanted_checks.is_some() {
        fields.push(field_str(
            "required status checks",
            format_contexts(current_checks),
            format_contexts(wanted_checks),
        ));
        fields.push(field(
            "strict (base must be current)",
            current_checks.map(strict_text),
            wanted_checks.map(strict_text),
        ));
    }

    let current_pr = rule_parameters(current, "pull_request");
    let wanted_pr = rule_parameters(wanted, "pull_request");
    if current_pr.is_some() || wanted_pr.is_some() {
        fields.push(field_str(
            "pull request required",
            if current_pr.is_some() { "yes" } else { "no" },
            if wanted_pr.is_some() { "yes" } else { "no" },
        ));
        if let (Some(current_pr), Some(wanted_pr)) = (current_pr, wanted_pr) {
            fields.push(field(
                "required approving reviews",
                current_pr.get("required_approving_review_count"),
                wanted_pr.get("required_approving_review_count"),
            ));
        }
    }

    EntryComparison {
        file: file.to_string(),
        key: key.to_string(),
        matched: true,
        fields,
    }
}

fn strict_text(parameters: &Value) -> String {
    let strict = parameters
        .get("strict_required_status_checks_policy")
        .and_then(Value::as_bool)
        .unwrap_or(false);
    display_bool(strict)
}

fn field(
    name: &'static str,
    current: Option<impl Display>,
    wanted: Option<impl Display>,
) -> FieldComparison {
    FieldComparison {
        field: name,
        current: current
            .map(|v| v.to_string())
            .unwrap_or_else(|| "(absent)".to_string()),
        wanted: wanted
            .map(|v| v.to_string())
            .unwrap_or_else(|| "(absent)".to_string()),
    }
}

fn field_str(
    name: &'static str,
    current: impl Into<String>,
    wanted: impl Into<String>,
) -> FieldComparison {
    FieldComparison {
        field: name,
        current: current.into(),
        wanted: wanted.into(),
    }
}

fn match_key(ruleset: &Value) -> String {
    let target = ruleset.get("target").and_then(Value::as_str).unwrap_or("");
    let mut includes: Vec<String> = ruleset
        .pointer("/conditions/ref_name/include")
        .and_then(Value::as_array)
        .map(|items| {
            items
                .iter()
                .filter_map(Value::as_str)
                .map(str::to_string)
                .collect()
        })
        .unwrap_or_default();
    includes.sort();
    format!("{target}|{}", includes.join(","))
}

fn rule_parameters<'a>(ruleset: &'a Value, rule_type: &str) -> Option<&'a Value> {
    ruleset
        .get("rules")
        .and_then(Value::as_array)?
        .iter()
        .find(|rule| rule.get("type").and_then(Value::as_str) == Some(rule_type))
        .and_then(|rule| rule.get("parameters"))
}

fn rule_types(ruleset: &Value) -> String {
    let mut types: Vec<String> = ruleset
        .get("rules")
        .and_then(Value::as_array)
        .map(|rules| {
            rules
                .iter()
                .filter_map(|rule| rule.get("type"))
                .filter_map(Value::as_str)
                .map(str::to_string)
                .collect()
        })
        .unwrap_or_default();
    types.sort();
    types.join(", ")
}

fn format_contexts(parameters: Option<&Value>) -> String {
    let Some(parameters) = parameters else {
        return "(rule absent)".to_string();
    };
    let mut contexts: Vec<String> = parameters
        .get("required_status_checks")
        .and_then(Value::as_array)
        .map(|checks| {
            checks
                .iter()
                .filter_map(|check| check.get("context"))
                .filter_map(Value::as_str)
                .map(str::to_string)
                .collect()
        })
        .unwrap_or_default();
    contexts.sort();
    if contexts.is_empty() {
        "(none)".to_string()
    } else {
        contexts.join(", ")
    }
}

fn format_bypass(ruleset: &Value) -> String {
    let mut actors: Vec<String> = ruleset
        .get("bypass_actors")
        .and_then(Value::as_array)
        .map(|actors| {
            actors
                .iter()
                .map(|actor| {
                    let actor_type = actor
                        .get("actor_type")
                        .and_then(Value::as_str)
                        .unwrap_or("");
                    let actor_id = actor.get("actor_id").map(display_json).unwrap_or_default();
                    let bypass_mode = actor
                        .get("bypass_mode")
                        .and_then(Value::as_str)
                        .unwrap_or("");
                    format!("{actor_type}#{actor_id}:{bypass_mode}")
                })
                .collect()
        })
        .unwrap_or_default();
    actors.sort();
    if actors.is_empty() {
        "(none)".to_string()
    } else {
        actors.join(", ")
    }
}

fn display_bool(value: bool) -> String {
    if value {
        "True".to_string()
    } else {
        "False".to_string()
    }
}

/// The scalar's display text: a bare string without its JSON quoting, a
/// capitalized boolean matching how the legacy PowerShell checker printed one,
/// and a number as its plain digits.
fn display_json(value: &Value) -> String {
    match value {
        Value::String(s) => s.clone(),
        Value::Bool(b) => display_bool(*b),
        Value::Null => String::new(),
        other => other.to_string(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn new_desired_ruleset(contexts: &[&str], strict: bool) -> Value {
        json!({
            "name": "Block push on main",
            "target": "branch",
            "enforcement": "active",
            "conditions": { "ref_name": { "include": ["~DEFAULT_BRANCH"], "exclude": [] } },
            "bypass_actors": [
                { "actor_id": 5, "actor_type": "RepositoryRole", "bypass_mode": "pull_request" }
            ],
            "rules": [
                { "type": "deletion" },
                { "type": "non_fast_forward" },
                { "type": "pull_request", "parameters": { "required_approving_review_count": 0 } },
                {
                    "type": "required_status_checks",
                    "parameters": {
                        "strict_required_status_checks_policy": strict,
                        "required_status_checks": contexts.iter().map(|c| json!({ "context": c })).collect::<Vec<_>>()
                    }
                }
            ]
        })
    }

    struct Fixture {
        _dir: tempfile::TempDir,
        desired_dir: PathBuf,
        live_file: PathBuf,
    }

    fn write_fixture(desired: &[Value], live: &[Value]) -> Fixture {
        let dir = tempfile::tempdir().unwrap();
        let desired_dir = dir.path().join("desired");
        std::fs::create_dir_all(&desired_dir).unwrap();
        for (index, payload) in desired.iter().enumerate() {
            std::fs::write(
                desired_dir.join(format!("set{index}.json")),
                serde_json::to_string_pretty(payload).unwrap(),
            )
            .unwrap();
        }
        let live_file = dir.path().join("live.json");
        std::fs::write(
            &live_file,
            serde_json::to_string(&Value::Array(live.to_vec())).unwrap(),
        )
        .unwrap();
        Fixture {
            _dir: dir,
            desired_dir,
            live_file,
        }
    }

    fn run_checker(desired: &[Value], live: &[Value]) -> (RulesetReport, String) {
        let fixture = write_fixture(desired, live);
        match check(&fixture.desired_dir, LiveSource::File(fixture.live_file)).unwrap() {
            CheckOutcome::Compared(report) => {
                let output = render(&report, false);
                (report, output)
            }
            CheckOutcome::Unreadable(message) => panic!("expected a comparison, got: {message}"),
        }
    }

    // -- the ten ported legacy cases -----------------------------------------

    #[test]
    fn a_live_state_matching_the_declared_one_is_accepted() {
        let wanted = new_desired_ruleset(&["ci-required"], true);
        let (report, _) = run_checker(std::slice::from_ref(&wanted), std::slice::from_ref(&wanted));
        assert!(report.ok(), "{:?}", report.differences);
    }

    #[test]
    fn a_required_context_missing_from_the_live_state_is_reported() {
        let wanted = new_desired_ruleset(&["ci-required", "crash-capture-required"], true);
        let live = new_desired_ruleset(&["ci-required"], true);
        let (report, output) = run_checker(&[wanted], &[live]);
        assert!(!report.ok());
        assert!(output.contains("required status checks"), "{output}");
    }

    #[test]
    fn a_live_state_that_dropped_the_pull_request_rule_is_reported() {
        let wanted = new_desired_ruleset(&["ci-required"], true);
        let mut live = new_desired_ruleset(&["ci-required"], true);
        let rules = live.get_mut("rules").unwrap().as_array_mut().unwrap();
        rules.retain(|r| r.get("type").and_then(Value::as_str) != Some("pull_request"));
        let (report, output) = run_checker(&[wanted], &[live]);
        assert!(!report.ok());
        assert!(output.contains("pull request required"), "{output}");
    }

    #[test]
    fn a_wider_bypass_than_declared_is_reported() {
        let wanted = new_desired_ruleset(&["ci-required"], true);
        let mut live = new_desired_ruleset(&["ci-required"], true);
        live["bypass_actors"] = json!([
            { "actor_id": 2, "actor_type": "RepositoryRole", "bypass_mode": "always" },
            { "actor_id": 5, "actor_type": "RepositoryRole", "bypass_mode": "always" }
        ]);
        let (report, output) = run_checker(&[wanted], &[live]);
        assert!(!report.ok());
        assert!(output.contains("bypass actors"), "{output}");
    }

    #[test]
    fn a_non_strict_live_policy_is_reported() {
        let wanted = new_desired_ruleset(&["ci-required"], true);
        let live = new_desired_ruleset(&["ci-required"], false);
        let (report, output) = run_checker(&[wanted], &[live]);
        assert!(!report.ok());
        assert!(output.contains("strict"), "{output}");
    }

    #[test]
    fn refs_nobody_protects_are_reported() {
        let wanted = new_desired_ruleset(&["ci-required"], true);
        let mut live = new_desired_ruleset(&["ci-required"], true);
        live["conditions"] =
            json!({ "ref_name": { "include": ["refs/heads/something-else"], "exclude": [] } });
        let (report, output) = run_checker(&[wanted], &[live]);
        assert!(!report.ok());
        assert!(
            output.contains("no live ruleset protects these refs"),
            "{output}"
        );
        assert!(output.contains("not declared"), "{output}");
    }

    #[test]
    fn a_live_state_that_cannot_be_read_is_not_a_pass() {
        let fixture = write_fixture(&[new_desired_ruleset(&["ci-required"], true)], &[]);
        let missing = fixture.desired_dir.join("no-such-live.json");
        let outcome = check(&fixture.desired_dir, LiveSource::File(missing)).unwrap();
        assert!(matches!(outcome, CheckOutcome::Unreadable(_)));
    }

    #[test]
    fn the_payloads_this_repository_ships_are_well_formed_and_complete() {
        let dir = repo_rulesets_dir();
        let shipped: Vec<_> = std::fs::read_dir(&dir)
            .unwrap()
            .filter_map(|e| e.ok())
            .map(|e| e.path())
            .filter(|p| p.extension().and_then(|e| e.to_str()) == Some("json"))
            .collect();
        assert!(
            shipped.len() >= 2,
            "expected at least two ruleset payloads, found {}",
            shipped.len()
        );
        for path in &shipped {
            let payload: Value =
                serde_json::from_str(&std::fs::read_to_string(path).unwrap()).unwrap();
            let name = path.file_name().unwrap().to_string_lossy();
            assert!(
                payload
                    .get("name")
                    .and_then(Value::as_str)
                    .is_some_and(|n| !n.is_empty()),
                "{name}: no name"
            );
            let target = payload.get("target").and_then(Value::as_str);
            assert!(
                matches!(target, Some("branch") | Some("tag")),
                "{name}: target {target:?}"
            );
            assert_eq!(
                payload.get("enforcement").and_then(Value::as_str),
                Some("active"),
                "{name}: enforcement"
            );
            let refs = payload
                .pointer("/conditions/ref_name/include")
                .and_then(Value::as_array);
            assert!(refs.is_some_and(|r| !r.is_empty()), "{name}: no refs");
            let rules = payload.get("rules").and_then(Value::as_array);
            assert!(rules.is_some_and(|r| !r.is_empty()), "{name}: no rules");
            for actor in payload
                .get("bypass_actors")
                .and_then(Value::as_array)
                .into_iter()
                .flatten()
            {
                let mode = actor.get("bypass_mode").and_then(Value::as_str);
                assert!(
                    matches!(mode, Some("always") | Some("pull_request")),
                    "{name}: bypass_mode {mode:?}"
                );
            }
        }
    }

    #[test]
    fn the_required_contexts_name_jobs_that_exist_and_always_report() {
        let main_ruleset: Value = serde_json::from_str(
            &std::fs::read_to_string(repo_rulesets_dir().join("main-branch.json")).unwrap(),
        )
        .unwrap();
        let checks = rule_parameters(&main_ruleset, "required_status_checks")
            .and_then(|p| p.get("required_status_checks"))
            .and_then(Value::as_array)
            .cloned()
            .unwrap_or_default();
        let contexts: Vec<String> = checks
            .iter()
            .filter_map(|c| c.get("context"))
            .filter_map(Value::as_str)
            .map(str::to_string)
            .collect();
        assert!(!contexts.is_empty(), "no required contexts declared");

        let workflows_dir = repo_rulesets_dir().join("../workflows");
        let mut workflow_text = String::new();
        for entry in std::fs::read_dir(&workflows_dir).unwrap() {
            let path = entry.unwrap().path();
            if path.extension().and_then(|e| e.to_str()) == Some("yml") {
                workflow_text.push_str(&std::fs::read_to_string(&path).unwrap());
                workflow_text.push('\n');
            }
        }

        for context in &contexts {
            let name_pattern =
                regex::Regex::new(&format!(r"(?m)^\s+name:\s+{}\s*$", regex::escape(context)))
                    .unwrap();
            let name_match = name_pattern.find(&workflow_text);
            assert!(
                name_match.is_some(),
                "required context '{context}' is not produced by any job"
            );
            let tail_start = name_match.unwrap().start();
            let tail_end = (tail_start + 600).min(workflow_text.len());
            let job_block = &workflow_text[tail_start..tail_end];
            assert!(
                job_block.contains("if: always()"),
                "required context '{context}' is produced by a job that can skip itself"
            );
        }
    }

    #[test]
    fn main_and_next_have_distinct_protection_and_only_publish_refs_allow_actions_bypass() {
        let dir = repo_rulesets_dir();
        let read = |name: &str| -> Value {
            serde_json::from_str(&std::fs::read_to_string(dir.join(name)).unwrap()).unwrap()
        };
        let main = read("main-branch.json");
        let next = read("next-branch.json");
        let tags = read("version-tags.json");

        assert!(
            main.pointer("/conditions/ref_name/include")
                .and_then(Value::as_array)
                .unwrap()
                .iter()
                .any(|v| v.as_str() == Some("refs/heads/main")),
            "main ruleset does not protect main explicitly"
        );
        assert!(
            next.pointer("/conditions/ref_name/include")
                .and_then(Value::as_array)
                .unwrap()
                .iter()
                .any(|v| v.as_str() == Some("refs/heads/next")),
            "next ruleset does not protect next explicitly"
        );

        for (name, entry) in [("main", &main), ("tags", &tags)] {
            let has_actions_bypass = entry
                .get("bypass_actors")
                .and_then(Value::as_array)
                .unwrap()
                .iter()
                .filter(|a| {
                    a.get("actor_type").and_then(Value::as_str) == Some("Integration")
                        && a.get("actor_id").and_then(Value::as_i64) == Some(15368)
                        && a.get("bypass_mode").and_then(Value::as_str) == Some("always")
                })
                .count();
            assert_eq!(
                has_actions_bypass, 1,
                "{name}: publish cannot write using the GitHub Actions integration"
            );
        }

        let next_actions_bypass = next
            .get("bypass_actors")
            .and_then(Value::as_array)
            .unwrap()
            .iter()
            .filter(|a| a.get("actor_type").and_then(Value::as_str) == Some("Integration"))
            .count();
        assert_eq!(
            next_actions_bypass, 0,
            "development branch accepts a workflow bypass"
        );
    }

    fn repo_rulesets_dir() -> PathBuf {
        Path::new(env!("CARGO_MANIFEST_DIR")).join("../../.github/rulesets")
    }

    // -- additional coverage of the surrounding contract ---------------------

    #[test]
    fn a_missing_desired_directory_is_unreadable() {
        let dir = tempfile::tempdir().unwrap();
        let outcome = check(
            &dir.path().join("no-such-dir"),
            LiveSource::File(dir.path().join("live.json")),
        )
        .unwrap();
        assert!(
            matches!(outcome, CheckOutcome::Unreadable(message) if message.contains("No ruleset directory"))
        );
    }

    #[test]
    fn a_desired_directory_with_no_json_payload_is_unreadable() {
        let dir = tempfile::tempdir().unwrap();
        let outcome = check(dir.path(), LiveSource::File(dir.path().join("live.json"))).unwrap();
        assert!(
            matches!(outcome, CheckOutcome::Unreadable(message) if message.contains("No ruleset payloads"))
        );
    }

    #[test]
    fn quiet_suppresses_the_field_by_field_comparison_but_keeps_the_difference() {
        let wanted = new_desired_ruleset(&["ci-required", "crash-capture-required"], true);
        let live = new_desired_ruleset(&["ci-required"], true);
        let fixture = write_fixture(&[wanted], &[live]);
        let report = match check(&fixture.desired_dir, LiveSource::File(fixture.live_file)).unwrap()
        {
            CheckOutcome::Compared(report) => report,
            CheckOutcome::Unreadable(message) => panic!("{message}"),
        };
        let full = render(&report, false);
        let quiet = render(&report, true);
        assert!(full.contains("current: ci-required"), "{full}");
        assert!(!quiet.contains("current: ci-required"), "{quiet}");
        assert!(quiet.contains("difference(s):"), "{quiet}");
        assert!(quiet.contains("required status checks"), "{quiet}");
    }

    #[test]
    fn a_missing_gh_produces_a_clear_unreadable_message() {
        let wanted = new_desired_ruleset(&["ci-required"], true);
        let dir = tempfile::tempdir().unwrap();
        std::fs::write(
            dir.path().join("set0.json"),
            serde_json::to_string(&wanted).unwrap(),
        )
        .unwrap();
        let outcome = check(dir.path(), LiveSource::GhApi).unwrap();
        match outcome {
            CheckOutcome::Unreadable(message) => {
                assert!(message.to_lowercase().contains("gh"), "{message}");
            }
            CheckOutcome::Compared(_) => panic!("gh is not installed in this environment"),
        }
    }
}
