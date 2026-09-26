//! Guards `app/cli/CommandLineFlags.cpp` against drift from the sources that
//! actually read argv, and from the callers that build argv for the app.
//!
//! `exosnap.exe` rejects a long option it does not recognize, which is only
//! safe while the registry lists every option a parser understands and every
//! option a caller passes. Both sides drift independently: a parser can grow
//! a flag the registry never learns about, and a caller (an acceptance
//! harness, a scenario driver) can pass a flag that was renamed or never
//! existed, in which case the run exits before anything is measured. This
//! scans sources rather than keeping a hand-maintained list, since a
//! hand-maintained list would have exactly the drift problem it exists to
//! prevent.
//!
//! Three independent things are checked against the registry:
//!   - the parser sources that read argv for `exosnap.exe` (the updater is a
//!     separate binary with its own options and is not scanned);
//!   - `-ArgumentList` blocks that pass `--auto-record`/`--auto-edit` in
//!     `scripts/lib/LiveVerifyChecks.ps1`, while that script still exists;
//!   - `launch(&[...])` argument arrays across the exo-verify scenario
//!     sources, which is how that harness invokes the app today.
//!
//! A missing registry or a missing parser source is a hard error: the source
//! list itself needs updating, and silently skipping it would let the guard
//! it exists to provide go dark. A missing harness script is not: that script
//! is owned by a separate merge and this check must not block its removal.

use std::collections::BTreeSet;
use std::path::Path;

use anyhow::Context;

const REGISTRY_PATH: &str = "app/cli/CommandLineFlags.cpp";
const MIN_REGISTERED_FLAGS: usize = 50;

/// The sources that read argv for `exosnap.exe`.
const PARSER_SOURCES: &[&str] = &[
    "app/auto_record/AutoRecordOptions.cpp",
    "app/quick/ExoSnap/Quick/main.cpp",
    "app/quick/ExoSnap/Quick/QuickAutoEditHarness.cpp",
    "app/services/ElevatedRelaunch.h",
    "app/services/UpdateFeedOverride.h",
    "app/services/VerifyReinstallMode.h",
];

const LIVE_VERIFY_HARNESS_SOURCE: &str = "scripts/lib/LiveVerifyChecks.ps1";
const EXO_VERIFY_SRC_PREFIX: &str = "tools/exo-verify/src/";

#[derive(Debug)]
pub struct UnregisteredFlag {
    pub flag: String,
    pub source: String,
}

/// The `scripts/lib/LiveVerifyChecks.ps1` half of the check. `Skipped` means
/// the script has already been removed, which is expected once the harness it
/// belongs to is merged elsewhere, not a failure of this check.
#[derive(Debug)]
pub enum HarnessScan {
    Skipped,
    Checked {
        flags: BTreeSet<String>,
        unregistered: Vec<UnregisteredFlag>,
    },
}

impl HarnessScan {
    fn unregistered(&self) -> &[UnregisteredFlag] {
        match self {
            HarnessScan::Skipped => &[],
            HarnessScan::Checked { unregistered, .. } => unregistered,
        }
    }
}

#[derive(Debug)]
pub struct CliFlagsReport {
    pub registered_count: usize,
    pub duplicate_flags: Vec<String>,
    pub unregistered_in_parsers: Vec<UnregisteredFlag>,
    pub harness_scan: HarnessScan,
    pub unregistered_in_exo_verify: Vec<UnregisteredFlag>,
}

impl CliFlagsReport {
    pub fn ok(&self) -> bool {
        self.duplicate_flags.is_empty()
            && self.unregistered_in_parsers.is_empty()
            && self.harness_scan.unregistered().is_empty()
            && self.unregistered_in_exo_verify.is_empty()
    }
}

pub fn check(repo_root: &Path) -> anyhow::Result<CliFlagsReport> {
    let registry_path = repo_root.join(REGISTRY_PATH);
    let registry_text = std::fs::read_to_string(&registry_path)
        .with_context(|| format!("missing {}", registry_path.display()))?;
    let all_registered = registered_flag_matches(&registry_text);
    anyhow::ensure!(
        all_registered.len() > MIN_REGISTERED_FLAGS,
        "{} parsed to only {} flag(s); the registry scan is broken",
        registry_path.display(),
        all_registered.len()
    );
    let duplicate_flags = duplicates(&all_registered);
    let registered: BTreeSet<String> = all_registered.into_iter().collect();

    let mut unregistered_in_parsers = Vec::new();
    for relative in PARSER_SOURCES {
        let path = repo_root.join(relative);
        let text = std::fs::read_to_string(&path).with_context(|| {
            format!("parser source '{relative}' does not exist; update the list in this check")
        })?;
        for flag in quoted_double(&text) {
            if !registered.contains(&flag) {
                unregistered_in_parsers.push(UnregisteredFlag {
                    flag,
                    source: relative.to_string(),
                });
            }
        }
    }

    let harness_script_path = repo_root.join(LIVE_VERIFY_HARNESS_SOURCE);
    let harness_scan = match std::fs::read_to_string(&harness_script_path) {
        Ok(text) => {
            let flags = harness_argument_flags(&text);
            let unregistered = flags
                .iter()
                .filter(|flag| !registered.contains(*flag))
                .map(|flag| UnregisteredFlag {
                    flag: flag.clone(),
                    source: LIVE_VERIFY_HARNESS_SOURCE.to_string(),
                })
                .collect();
            HarnessScan::Checked {
                flags,
                unregistered,
            }
        }
        Err(_) => HarnessScan::Skipped,
    };

    let git = crate::git::Git::new(repo_root);
    let tracked = git.ls_files()?;
    let mut unregistered_in_exo_verify = Vec::new();
    for relative in tracked
        .iter()
        .filter(|path| path.starts_with(EXO_VERIFY_SRC_PREFIX) && path.ends_with(".rs"))
    {
        let path = repo_root.join(relative);
        let text = std::fs::read_to_string(&path)
            .with_context(|| format!("could not read {}", path.display()))?;
        for flag in launch_argument_flags(&text) {
            if !registered.contains(&flag) {
                unregistered_in_exo_verify.push(UnregisteredFlag {
                    flag,
                    source: relative.clone(),
                });
            }
        }
    }

    Ok(CliFlagsReport {
        registered_count: registered.len(),
        duplicate_flags,
        unregistered_in_parsers,
        harness_scan,
        unregistered_in_exo_verify,
    })
}

pub fn render(report: &CliFlagsReport) -> String {
    let mut out = String::new();
    out.push_str(&format!(
        "CLI flag registry: {} flag(s) registered\n",
        report.registered_count
    ));

    if !report.duplicate_flags.is_empty() {
        out.push('\n');
        out.push_str("duplicate registry entries:\n");
        for flag in &report.duplicate_flags {
            out.push_str(&format!("  {flag}\n"));
        }
    }

    if !report.unregistered_in_parsers.is_empty() {
        out.push('\n');
        out.push_str(
            "unregistered long option(s). Add them to app/cli/CommandLineFlags.cpp, or \
             exosnap.exe will refuse them:\n",
        );
        for entry in &report.unregistered_in_parsers {
            out.push_str(&format!("  {} ({})\n", entry.flag, entry.source));
        }
    }

    match &report.harness_scan {
        HarnessScan::Skipped => {
            out.push('\n');
            out.push_str(&format!(
                "{LIVE_VERIFY_HARNESS_SOURCE}: not present, skipped\n"
            ));
        }
        HarnessScan::Checked { unregistered, .. } => {
            if !unregistered.is_empty() {
                out.push('\n');
                out.push_str(
                    "the acceptance harness passes option(s) exosnap.exe does not know. The run \
                     exits before anything is measured:\n",
                );
                for entry in unregistered {
                    out.push_str(&format!("  {} ({})\n", entry.flag, entry.source));
                }
            }
        }
    }

    if !report.unregistered_in_exo_verify.is_empty() {
        out.push('\n');
        out.push_str(
            "exo-verify launches the app with option(s) it does not know. The run exits before \
             anything is measured:\n",
        );
        for entry in &report.unregistered_in_exo_verify {
            out.push_str(&format!("  {} ({})\n", entry.flag, entry.source));
        }
    }

    out.push('\n');
    if report.ok() {
        out.push_str("CLI flag registry: OK\n");
    } else {
        out.push_str("CLI flag registry: FAILED\n");
    }
    out
}

fn registered_flag_matches(text: &str) -> Vec<String> {
    regex::Regex::new(r#"KnownFlag\{"(--[a-z0-9-]+)"#)
        .unwrap()
        .captures_iter(text)
        .map(|c| c[1].to_string())
        .collect()
}

fn duplicates(all: &[String]) -> Vec<String> {
    let mut counts: std::collections::BTreeMap<&str, usize> = std::collections::BTreeMap::new();
    for flag in all {
        *counts.entry(flag.as_str()).or_default() += 1;
    }
    counts
        .into_iter()
        .filter(|(_, count)| *count > 1)
        .map(|(flag, _)| flag.to_string())
        .collect()
}

fn quoted_double(text: &str) -> BTreeSet<String> {
    regex::Regex::new(r#""(--[a-z0-9-]+)""#)
        .unwrap()
        .captures_iter(text)
        .map(|c| c[1].to_string())
        .collect()
}

/// Every long option inside an `-ArgumentList @( ... )` block that mentions
/// `--auto-record` or `--auto-edit`. Scoped to those blocks on purpose: the
/// same script also builds argv for pwsh, ffprobe and envctl, whose options
/// are none of this registry's business.
fn harness_argument_flags(text: &str) -> BTreeSet<String> {
    let start = regex::Regex::new(r"-ArgumentList\s*@\(").unwrap();
    let quoted_single = regex::Regex::new(r"'(--[a-z0-9-]+)'").unwrap();
    let bytes = text.as_bytes();
    let mut flags = BTreeSet::new();
    for m in start.find_iter(text) {
        let mut depth: i32 = 1;
        let mut i = m.end();
        while i < bytes.len() && depth > 0 {
            match bytes[i] {
                b'(' => depth += 1,
                b')' => depth -= 1,
                _ => {}
            }
            i += 1;
        }
        let block = &text[m.start()..i];
        if !block.contains("--auto-record") && !block.contains("--auto-edit") {
            continue;
        }
        for capture in quoted_single.captures_iter(block) {
            flags.insert(capture[1].to_string());
        }
    }
    flags
}

/// Every long option inside a `launch(&[...])` argument array. Deliberately
/// not a scan of every `Command::new` call: exo-verify also shells out to
/// PresentMon, git, cmake and assorted probes, whose options are not this
/// registry's business.
fn launch_argument_flags(text: &str) -> BTreeSet<String> {
    let call = regex::Regex::new(r"\.launch\(&\[([^\]]*)\]\)").unwrap();
    let mut flags = BTreeSet::new();
    for capture in call.captures_iter(text) {
        flags.extend(quoted_double(&capture[1]));
    }
    flags
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    fn repo_root() -> PathBuf {
        Path::new(env!("CARGO_MANIFEST_DIR")).join("../..")
    }

    // -- real-repository cases: these keep guarding the actual tree --------

    #[test]
    fn the_registry_parses_to_more_than_fifty_flags() {
        let report = check(&repo_root()).unwrap();
        assert!(
            report.registered_count > MIN_REGISTERED_FLAGS,
            "only {} flag(s)",
            report.registered_count
        );
    }

    #[test]
    fn the_registry_lists_no_flag_twice() {
        let report = check(&repo_root()).unwrap();
        assert!(
            report.duplicate_flags.is_empty(),
            "{:?}",
            report.duplicate_flags
        );
    }

    #[test]
    fn every_long_option_in_a_parser_source_is_registered() {
        let report = check(&repo_root()).unwrap();
        assert!(
            report.unregistered_in_parsers.is_empty(),
            "{:?}",
            report
                .unregistered_in_parsers
                .iter()
                .map(|e| format!("{} ({})", e.flag, e.source))
                .collect::<Vec<_>>()
        );
    }

    #[test]
    fn every_option_the_acceptance_harness_passes_is_registered() {
        let report = check(&repo_root()).unwrap();
        match &report.harness_scan {
            HarnessScan::Skipped => {}
            HarnessScan::Checked { unregistered, .. } => {
                assert!(
                    unregistered.is_empty(),
                    "{:?}",
                    unregistered
                        .iter()
                        .map(|e| format!("{} ({})", e.flag, e.source))
                        .collect::<Vec<_>>()
                );
            }
        }
    }

    #[test]
    fn the_harness_scan_actually_reaches_the_invocations() {
        let path = repo_root().join(LIVE_VERIFY_HARNESS_SOURCE);
        let Ok(text) = std::fs::read_to_string(&path) else {
            return;
        };
        let flags = harness_argument_flags(&text);
        assert!(
            flags.contains("--auto-record"),
            "the harness scan found no --auto-record invocation"
        );
        assert!(
            flags.contains("--audio-rows"),
            "the harness scan missed a flag it should have seen"
        );
    }

    #[test]
    fn exo_verify_launches_the_app_with_no_unregistered_option() {
        let report = check(&repo_root()).unwrap();
        assert!(
            report.unregistered_in_exo_verify.is_empty(),
            "{:?}",
            report
                .unregistered_in_exo_verify
                .iter()
                .map(|e| format!("{} ({})", e.flag, e.source))
                .collect::<Vec<_>>()
        );
    }

    #[test]
    fn the_exo_verify_scan_actually_reaches_the_launch_calls() {
        let path = repo_root().join("tools/exo-verify/src/scenarios/visual.rs");
        let text = std::fs::read_to_string(&path).unwrap();
        let flags = launch_argument_flags(&text);
        assert!(
            flags.contains("--overlay-visual-state"),
            "the exo-verify scan missed --overlay-visual-state"
        );
    }

    // -- fixture cases: the guard has to be able to fail ---------------------

    fn fixture_registry(flags: &[&str]) -> String {
        flags
            .iter()
            .map(|flag| format!("KnownFlag{{\"{flag}\", FlagArity::None}},\n"))
            .collect()
    }

    fn many_flags(count: usize) -> Vec<String> {
        (0..count).map(|i| format!("--fixture-flag-{i}")).collect()
    }

    #[test]
    fn an_unregistered_option_in_a_parser_source_is_detected() {
        let padding = many_flags(MIN_REGISTERED_FLAGS + 1);
        let padding_refs: Vec<&str> = padding.iter().map(String::as_str).collect();
        let dir = crate::test_support::fixture_repo(&[
            (
                "app/cli/CommandLineFlags.cpp",
                &fixture_registry(&padding_refs),
            ),
            (
                "app/auto_record/AutoRecordOptions.cpp",
                "if (arg == QStringLiteral(\"--definitely-not-registered\")) {}",
            ),
            ("app/quick/ExoSnap/Quick/main.cpp", ""),
            ("app/quick/ExoSnap/Quick/QuickAutoEditHarness.cpp", ""),
            ("app/services/ElevatedRelaunch.h", ""),
            ("app/services/UpdateFeedOverride.h", ""),
            ("app/services/VerifyReinstallMode.h", ""),
        ]);
        let report = check(dir.path()).unwrap();
        assert!(
            report
                .unregistered_in_parsers
                .iter()
                .any(|e| e.flag == "--definitely-not-registered"),
            "the scanner did not detect the fixture flag"
        );
    }

    #[test]
    fn a_missing_parser_source_is_a_hard_error() {
        let padding = many_flags(MIN_REGISTERED_FLAGS + 1);
        let padding_refs: Vec<&str> = padding.iter().map(String::as_str).collect();
        let dir = crate::test_support::fixture_repo(&[(
            "app/cli/CommandLineFlags.cpp",
            &fixture_registry(&padding_refs),
        )]);
        let error = check(dir.path()).unwrap_err();
        assert!(error.to_string().contains("does not exist"), "{error:#}");
    }

    #[test]
    fn a_missing_registry_is_a_hard_error() {
        let dir = crate::test_support::fixture_repo(&[("README.md", "")]);
        let error = check(dir.path()).unwrap_err();
        assert!(error.to_string().contains("missing"), "{error:#}");
    }

    #[test]
    fn a_registry_that_parses_to_too_few_flags_is_a_hard_error() {
        let dir = crate::test_support::fixture_repo(&[(
            "app/cli/CommandLineFlags.cpp",
            &fixture_registry(&["--only-one-flag"]),
        )]);
        let error = check(dir.path()).unwrap_err();
        assert!(
            error.to_string().contains("registry scan is broken"),
            "{error:#}"
        );
    }

    #[test]
    fn a_duplicate_registry_entry_is_detected() {
        let mut flags = many_flags(MIN_REGISTERED_FLAGS + 1);
        flags.push("--duplicate-flag".to_string());
        flags.push("--duplicate-flag".to_string());
        let refs: Vec<&str> = flags.iter().map(String::as_str).collect();
        let dir = crate::test_support::fixture_repo(&[
            ("app/cli/CommandLineFlags.cpp", &fixture_registry(&refs)),
            ("app/auto_record/AutoRecordOptions.cpp", ""),
            ("app/quick/ExoSnap/Quick/main.cpp", ""),
            ("app/quick/ExoSnap/Quick/QuickAutoEditHarness.cpp", ""),
            ("app/services/ElevatedRelaunch.h", ""),
            ("app/services/UpdateFeedOverride.h", ""),
            ("app/services/VerifyReinstallMode.h", ""),
        ]);
        let report = check(dir.path()).unwrap();
        assert_eq!(report.duplicate_flags, vec!["--duplicate-flag".to_string()]);
        assert!(!report.ok());
    }

    #[test]
    fn a_missing_harness_script_is_skipped_not_an_error() {
        let padding = many_flags(MIN_REGISTERED_FLAGS + 1);
        let padding_refs: Vec<&str> = padding.iter().map(String::as_str).collect();
        let dir = crate::test_support::fixture_repo(&[
            (
                "app/cli/CommandLineFlags.cpp",
                &fixture_registry(&padding_refs),
            ),
            ("app/auto_record/AutoRecordOptions.cpp", ""),
            ("app/quick/ExoSnap/Quick/main.cpp", ""),
            ("app/quick/ExoSnap/Quick/QuickAutoEditHarness.cpp", ""),
            ("app/services/ElevatedRelaunch.h", ""),
            ("app/services/UpdateFeedOverride.h", ""),
            ("app/services/VerifyReinstallMode.h", ""),
        ]);
        let report = check(dir.path()).unwrap();
        assert!(matches!(report.harness_scan, HarnessScan::Skipped));
        assert!(report.ok());
    }

    #[test]
    fn an_unregistered_launch_option_in_exo_verify_is_detected() {
        let padding = many_flags(MIN_REGISTERED_FLAGS + 1);
        let padding_refs: Vec<&str> = padding.iter().map(String::as_str).collect();
        let dir = crate::test_support::fixture_repo(&[
            (
                "app/cli/CommandLineFlags.cpp",
                &fixture_registry(&padding_refs),
            ),
            ("app/auto_record/AutoRecordOptions.cpp", ""),
            ("app/quick/ExoSnap/Quick/main.cpp", ""),
            ("app/quick/ExoSnap/Quick/QuickAutoEditHarness.cpp", ""),
            ("app/services/ElevatedRelaunch.h", ""),
            ("app/services/UpdateFeedOverride.h", ""),
            ("app/services/VerifyReinstallMode.h", ""),
            (
                "tools/exo-verify/src/scenarios/fixture.rs",
                "let mut app = ctx.launch(&[\"--bogus\"])?;\n",
            ),
        ]);
        let report = check(dir.path()).unwrap();
        assert!(
            report
                .unregistered_in_exo_verify
                .iter()
                .any(|e| e.flag == "--bogus"),
            "the scanner did not detect the fixture launch flag"
        );
        assert!(!report.ok());
    }
}
