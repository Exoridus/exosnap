//! Repository-owned diagnostic identities and advisory report artifacts.

use std::collections::{BTreeMap, BTreeSet};
use std::path::Path;
use std::sync::LazyLock;

use anyhow::Context as _;
use regex::Regex;
use serde::{Deserialize, Serialize};

static DIAGNOSTIC: LazyLock<Regex> = LazyLock::new(|| {
    Regex::new(r"^(.+):(\d+):(\d+): (warning|error|fatal error): (.+) \[([a-zA-Z0-9_.,-]+)\]$")
        .unwrap()
});
static COMPLEXITY: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"^function '(.+)' has cognitive complexity of (\d+)").unwrap());

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Diagnostic {
    pub file: String,
    pub line: u32,
    pub column: u32,
    pub check: String,
    pub message: String,
}

#[derive(Clone, Debug, Default, Serialize, Deserialize)]
pub struct RunMetadata {
    pub commit: String,
    pub working_tree_dirty: bool,
    pub clang_tidy_version: String,
    pub input_count: usize,
    pub translation_unit_count: usize,
    pub started_unix_seconds: u64,
    pub elapsed_seconds: f64,
    pub arguments: Vec<String>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct ComplexitySite {
    pub file: String,
    pub line: u32,
    pub function: String,
    pub score: u32,
}

#[derive(Clone, Debug, Default, Serialize, Deserialize)]
pub struct NormalizedReport {
    pub schema_version: u32,
    pub status: String,
    pub metadata: RunMetadata,
    pub raw_diagnostic_count: usize,
    pub unique_diagnostic_count: usize,
    pub excluded_external_diagnostic_count: usize,
    pub by_check: BTreeMap<String, usize>,
    pub by_directory: BTreeMap<String, usize>,
    pub by_check_and_directory: BTreeMap<String, BTreeMap<String, usize>>,
    pub diagnostics: Vec<Diagnostic>,
    pub top_complexity: Vec<ComplexitySite>,
    pub public_engine_api_diagnostics: Vec<Diagnostic>,
    pub public_engine_api_scope: String,
}

/// Uses tracked spelling for display and case-folded Windows identities for lookup.
/// Paths outside the tracked repository never become repository findings.
pub struct RepositoryPaths {
    root: String,
    windows: bool,
    tracked: BTreeMap<String, String>,
}

impl RepositoryPaths {
    pub fn new(root: &Path, tracked: &[String]) -> Self {
        let root = lexical_path(&root.to_string_lossy());
        let windows = root.as_bytes().get(1) == Some(&b':') || root.starts_with("//");
        let key = |path: &str| {
            if windows {
                path.to_lowercase()
            } else {
                path.to_string()
            }
        };
        Self {
            root: key(root.trim_end_matches('/')),
            windows,
            tracked: tracked
                .iter()
                .map(|path| {
                    let path = lexical_path(path);
                    (key(&path), path)
                })
                .collect(),
        }
    }

    pub fn relative(&self, path: &str) -> Option<String> {
        let path = lexical_path(path);
        let path = if self.windows {
            path.to_lowercase()
        } else {
            path
        };
        let relative = path
            .strip_prefix(&format!("{}/", self.root))
            .unwrap_or(&path);
        self.tracked.get(relative).cloned()
    }
}

fn lexical_path(path: &str) -> String {
    let path = path.replace('\\', "/");
    let path = if path
        .get(..8)
        .is_some_and(|prefix| prefix.eq_ignore_ascii_case("//?/UNC/"))
    {
        format!("//{}", &path[8..])
    } else if path.starts_with('/') {
        path.strip_prefix("//?/").unwrap_or(&path).to_string()
    } else {
        path
    };
    // Roots are not removable components: otherwise an external absolute
    // diagnostic can become a relative path matching a tracked source file.
    let (prefix, body, rooted, protected) = if path.starts_with("//") {
        ("//", path.as_str(), true, 2)
    } else if path.starts_with('/') {
        ("/", path.as_str(), true, 0)
    } else if path.as_bytes().first().is_some_and(u8::is_ascii_alphabetic)
        && path.as_bytes().get(1) == Some(&b':')
    {
        let rooted = path.as_bytes().get(2) == Some(&b'/');
        let prefix_len = if rooted { 3 } else { 2 };
        (&path[..prefix_len], &path[prefix_len..], rooted, 0)
    } else {
        ("", path.as_str(), false, 0)
    };
    let mut pieces = Vec::new();
    for part in body.split('/') {
        match part {
            "" | "." => {}
            ".." if pieces.len() > protected && pieces.last().is_some_and(|last| *last != "..") => {
                pieces.pop();
            }
            ".." if rooted => {}
            _ => pieces.push(part),
        }
    }
    format!("{prefix}{}", pieces.join("/"))
}

fn directory(file: &str) -> String {
    let mut parts = file.split('/');
    let first = parts.next().unwrap_or(file);
    match (first, parts.next()) {
        ("libs", Some(subsystem)) => format!("libs/{subsystem}"),
        _ => first.to_string(),
    }
}

/// Fails closed on compiler errors or unrecognized diagnostic records. Notes and
/// ordinary clang-tidy progress output are not diagnostic sites.
pub fn normalize(
    raw: &str,
    paths: &RepositoryPaths,
    metadata: RunMetadata,
    changed_files: Option<&BTreeSet<String>>,
) -> anyhow::Result<NormalizedReport> {
    let mut report = NormalizedReport {
        schema_version: 1,
        status: "completed".into(),
        metadata,
        public_engine_api_scope: if changed_files.is_some() {
            "changed public Engine headers".into()
        } else {
            "all public Engine headers".into()
        },
        ..NormalizedReport::default()
    };
    let mut sites = BTreeMap::<(String, u32, u32, String), Diagnostic>::new();
    for line in raw.lines() {
        let line = line.trim_end();
        let Some(captures) = DIAGNOSTIC.captures(line) else {
            anyhow::ensure!(
                !(line.contains(": warning:")
                    || line.contains(": error:")
                    || line.contains(": fatal error:")
                    || line.starts_with("error:")),
                "unrecognized clang-tidy diagnostic: {line}"
            );
            continue;
        };
        let checks = captures[6]
            .split(',')
            .filter(|check| *check != "-warnings-as-errors")
            .collect::<Vec<_>>();
        anyhow::ensure!(
            !checks.is_empty()
                && !checks.iter().any(|check| {
                    check.starts_with("clang-diagnostic-error")
                        || check.starts_with("clang-diagnostic-fatal")
                }),
            "clang-tidy could not analyze a translation unit: {line}"
        );
        let line_number: u32 = captures[2].parse()?;
        let column: u32 = captures[3].parse()?;
        anyhow::ensure!(
            line_number > 0 && column > 0,
            "invalid diagnostic location: {line}"
        );
        let Some(file) = paths.relative(&captures[1]) else {
            report.excluded_external_diagnostic_count += checks.len();
            continue;
        };
        if file.split('/').any(|part| part == "third_party") {
            report.excluded_external_diagnostic_count += checks.len();
            continue;
        }
        for check in checks {
            report.raw_diagnostic_count += 1;
            let finding = Diagnostic {
                file: file.clone(),
                line: line_number,
                column,
                check: check.into(),
                message: captures[5].into(),
            };
            let key = (file.clone(), line_number, column, check.to_string());
            // Different instantiations can attach different messages to one site.
            // Stable selection avoids making the artifact depend on worker order.
            sites
                .entry(key)
                .and_modify(|existing| {
                    if finding.message < existing.message {
                        existing.message.clone_from(&finding.message);
                    }
                })
                .or_insert(finding);
        }
    }
    report.diagnostics = sites.into_values().collect();
    report.unique_diagnostic_count = report.diagnostics.len();
    for finding in &report.diagnostics {
        let subsystem = directory(&finding.file);
        *report.by_check.entry(finding.check.clone()).or_default() += 1;
        *report.by_directory.entry(subsystem.clone()).or_default() += 1;
        *report
            .by_check_and_directory
            .entry(finding.check.clone())
            .or_default()
            .entry(subsystem)
            .or_default() += 1;
        if finding.check == "readability-function-cognitive-complexity"
            && let Some(captures) = COMPLEXITY.captures(&finding.message)
        {
            report.top_complexity.push(ComplexitySite {
                file: finding.file.clone(),
                line: finding.line,
                function: captures[1].into(),
                score: captures[2].parse()?,
            });
        }
        if finding.check == "bugprone-easily-swappable-parameters"
            && finding.file.starts_with("libs/engine/include/")
            && changed_files.is_none_or(|changed| changed.contains(&finding.file))
        {
            report.public_engine_api_diagnostics.push(finding.clone());
        }
    }
    report.top_complexity.sort_by(|a, b| {
        b.score
            .cmp(&a.score)
            .then(a.file.cmp(&b.file))
            .then(a.line.cmp(&b.line))
    });
    report.top_complexity.truncate(20);
    Ok(report)
}

impl NormalizedReport {
    pub fn summary(&self) -> String {
        let mut text = format!(
            "# clang-tidy advisory measurement\n\nCommit: `{}`; dirty: {}; inputs: {}; translation units: {}.\n\nRaw repository diagnostics: {}. Unique sites: {}. External diagnostics excluded: {}.\n\n## Per check\n\n| Check | Unique sites |\n| --- | ---: |\n",
            self.metadata.commit,
            self.metadata.working_tree_dirty,
            self.metadata.input_count,
            self.metadata.translation_unit_count,
            self.raw_diagnostic_count,
            self.unique_diagnostic_count,
            self.excluded_external_diagnostic_count,
        );
        for (check, count) in &self.by_check {
            text.push_str(&format!("| {check} | {count} |\n"));
        }
        text.push_str("\n## Per subsystem\n\n| Subsystem | Unique sites |\n| --- | ---: |\n");
        for (subsystem, count) in &self.by_directory {
            text.push_str(&format!("| {subsystem} | {count} |\n"));
        }
        text.push_str("\n## Highest cognitive complexity\n\nScores are decision aids. Device, Win32 and state-machine code requires ownership-aware review.\n\n| Function | Location | Score |\n| --- | --- | ---: |\n");
        for finding in &self.top_complexity {
            text.push_str(&format!(
                "| {} | {}:{} | {} |\n",
                finding.function.replace('|', "\\|"),
                finding.file,
                finding.line,
                finding.score
            ));
        }
        text.push_str(&format!(
            "\n## Public Engine parameter design\n\nScope: {}. Findings: {}. Non-blocking.\n",
            self.public_engine_api_scope,
            self.public_engine_api_diagnostics.len()
        ));
        for finding in &self.public_engine_api_diagnostics {
            text.push_str(&format!(
                "\n- {}:{}:{}: {}\n",
                finding.file, finding.line, finding.column, finding.message
            ));
        }
        text
    }

    /// The supplied path names the raw artifact. Siblings use `.json` and `.md`.
    pub fn write(&self, raw_path: &Path, raw: &str) -> anyhow::Result<()> {
        write_raw(raw_path, raw)?;
        std::fs::write(
            raw_path.with_extension("json"),
            serde_json::to_vec_pretty(self)?,
        )?;
        std::fs::write(raw_path.with_extension("md"), self.summary())?;
        Ok(())
    }
}

pub fn write_raw(path: &Path, raw: &str) -> anyhow::Result<()> {
    if let Some(parent) = path
        .parent()
        .filter(|parent| !parent.as_os_str().is_empty())
    {
        std::fs::create_dir_all(parent)
            .with_context(|| format!("could not create {}", parent.display()))?;
    }
    std::fs::write(path, raw).with_context(|| format!("could not write {}", path.display()))
}

/// Replaces the measurement state without inventing a diagnostic count.
pub fn write_status(path: &Path, status: &str, detail: &str) -> anyhow::Result<()> {
    anyhow::ensure!(
        !matches!(
            path.extension().and_then(|part| part.to_str()),
            Some("json" | "md")
        ),
        "raw advisory report path must not use .json or .md"
    );
    if let Some(parent) = path
        .parent()
        .filter(|parent| !parent.as_os_str().is_empty())
    {
        std::fs::create_dir_all(parent)?;
    }
    std::fs::write(
        path.with_extension("json"),
        serde_json::to_vec_pretty(
            &serde_json::json!({"schema_version": 1, "status": status, "detail": detail}),
        )?,
    )?;
    std::fs::write(
        path.with_extension("md"),
        format!("# Advisory measurement\n\nStatus: {status}. {detail}\n"),
    )?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn paths() -> RepositoryPaths {
        RepositoryPaths::new(
            Path::new("C:/Source/Repo"),
            &["libs/engine/include/API.h".into(), "app/main.cpp".into()],
        )
    }

    fn report(raw: &str) -> NormalizedReport {
        normalize(raw, &paths(), RunMetadata::default(), None).unwrap()
    }

    #[test]
    fn repeats_deduplicate_but_columns_remain_distinct() {
        let line = "C:\\Source\\Repo\\libs\\engine\\include\\API.h:12:3: warning: stale alias [misc-unused-alias-decls]\n";
        let result = report(&format!("{line}{line}{}", line.replace(":12:3:", ":12:4:")));
        assert_eq!(result.raw_diagnostic_count, 3);
        assert_eq!(result.unique_diagnostic_count, 2);
        assert_eq!(result.by_check["misc-unused-alias-decls"], 2);
        assert_eq!(result.by_directory["libs/engine"], 2);
        assert_eq!(
            result.by_check_and_directory["misc-unused-alias-decls"]["libs/engine"],
            2
        );
    }

    #[test]
    fn windows_case_separators_extended_prefix_and_dot_components_share_identity() {
        for file in [
            "c:/source/repo/LIBS/engine/include/api.H",
            "C:\\Source\\Repo\\libs\\engine\\src\\..\\include\\API.h",
            "\\\\?\\C:\\Source\\Repo\\libs\\engine\\include\\API.h",
            "./libs/engine/include/API.h",
        ] {
            assert_eq!(
                paths().relative(file).as_deref(),
                Some("libs/engine/include/API.h")
            );
        }
        assert!(
            paths()
                .relative("C:/Source/Repository/libs/engine/include/API.h")
                .is_none()
        );
        assert!(paths().relative("C:/SDK/include/API.h").is_none());
    }

    #[test]
    fn rooted_paths_cannot_turn_into_relative_repository_paths() {
        assert!(
            paths()
                .relative("C:/other/../../libs/engine/include/API.h")
                .is_none()
        );
        assert_eq!(lexical_path("C:/../../outside.h"), "C:/outside.h");
        assert_eq!(lexical_path("/../../outside.h"), "/outside.h");
        assert_eq!(lexical_path("../../outside.h"), "../../outside.h");
        assert_eq!(lexical_path("C:../outside.h"), "C:../outside.h");
        assert_eq!(lexical_path("//?/C:/../../outside.h"), "C:/outside.h");
        assert_eq!(
            lexical_path("//server/share/../../outside.h"),
            "//server/share/outside.h"
        );
    }

    #[test]
    fn extended_unc_paths_keep_their_server_and_share_identity() {
        let paths = RepositoryPaths::new(
            Path::new("//server/share/repo"),
            &["libs/engine/include/API.h".into()],
        );
        assert_eq!(
            paths.relative("\\\\?\\UNC\\SERVER\\share\\repo\\libs\\engine\\include\\API.h"),
            Some("libs/engine/include/API.h".into())
        );
        assert!(
            paths
                .relative("//server/share/repo/../../libs/engine/include/API.h")
                .is_none()
        );
    }

    #[test]
    fn malformed_or_compiler_error_output_cannot_report_zero() {
        for raw in [
            "app/main.cpp:12:x: warning: broken [misc-unused-alias-decls]",
            "app/main.cpp:0:1: warning: broken [misc-unused-alias-decls]",
            "app/main.cpp:12:1: error: header missing [clang-diagnostic-error]",
            "error: no compile database",
            "app/main.cpp:12:1: warning: unknown format",
        ] {
            assert!(
                normalize(raw, &paths(), RunMetadata::default(), None).is_err(),
                "{raw}"
            );
        }
        assert_eq!(
            report("Processing file app/main.cpp\n10 warnings generated.\n")
                .unique_diagnostic_count,
            0
        );
    }

    #[test]
    fn external_diagnostics_and_notes_are_not_repository_sites() {
        let result = report(
            "C:/SDK/a.h:2:1: warning: alias [misc-unused-alias-decls]\napp/main.cpp:8:1: note: here\napp/main.cpp:9:2: error: stale [misc-unused-using-decls,-warnings-as-errors]\n",
        );
        assert_eq!(result.excluded_external_diagnostic_count, 1);
        assert_eq!(result.raw_diagnostic_count, 1);
        assert_eq!(result.by_directory["app"], 1);
        assert_eq!(result.by_check["misc-unused-using-decls"], 1);
    }

    #[test]
    fn tracked_vendor_headers_are_excluded() {
        let paths = RepositoryPaths::new(
            Path::new("C:/repo"),
            &["libs/update/third_party/vendor.h".into()],
        );
        let result = normalize(
            "libs/update/third_party/vendor.h:1:1: warning: unused [misc-unused-alias-decls]",
            &paths,
            RunMetadata::default(),
            None,
        )
        .unwrap();
        assert_eq!(result.unique_diagnostic_count, 0);
        assert_eq!(result.excluded_external_diagnostic_count, 1);
    }

    #[test]
    fn design_reports_are_ranked_and_target_changed_public_headers() {
        let raw = "app/main.cpp:8:1: warning: function 'device' has cognitive complexity of 80 (threshold 0) [readability-function-cognitive-complexity]\nlibs/engine/include/API.h:4:1: warning: function 'state' has cognitive complexity of 24 (threshold 0) [readability-function-cognitive-complexity]\nlibs/engine/include/API.h:5:1: warning: adjacent parameters [bugprone-easily-swappable-parameters]\napp/main.cpp:5:1: warning: adjacent parameters [bugprone-easily-swappable-parameters]\n";
        let result = report(raw);
        assert_eq!(result.top_complexity[0].score, 80);
        assert_eq!(result.public_engine_api_diagnostics.len(), 1);
        let changed = BTreeSet::from(["app/main.cpp".into()]);
        let result = normalize(raw, &paths(), RunMetadata::default(), Some(&changed)).unwrap();
        assert!(result.public_engine_api_diagnostics.is_empty());
    }

    #[test]
    fn message_choice_and_serialization_ignore_worker_order() {
        let one = "app/main.cpp:1:1: warning: alpha [misc-unused-parameters]\n";
        let two = one.replace("alpha", "zeta");
        assert_eq!(
            serde_json::to_value(report(&format!("{one}{two}"))).unwrap(),
            serde_json::to_value(report(&format!("{two}{one}"))).unwrap()
        );
    }

    #[test]
    fn failure_replaces_previous_success_without_claiming_zero_findings() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("report.txt");
        report("app/main.cpp:1:2: warning: stale [misc-unused-parameters]\n")
            .write(&path, "raw finding")
            .unwrap();
        write_status(&path, "failed", "required tool unavailable").unwrap();
        let value: serde_json::Value =
            serde_json::from_slice(&std::fs::read(path.with_extension("json")).unwrap()).unwrap();
        assert_eq!(value["status"], "failed");
        assert!(value.get("raw_diagnostic_count").is_none());
        assert_eq!(std::fs::read_to_string(path).unwrap(), "raw finding");
    }
}
