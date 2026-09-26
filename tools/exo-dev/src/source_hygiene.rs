//! Finds development provenance in source comments and doc comments: a task
//! tracker reference, a commit hash, an issue/PR number, a private workspace
//! path, a machine-specific absolute or UNC path, prose narrating how a change
//! was reached, a branch name, or non-ASCII punctuation. The policy is one
//! paragraph long and lives in the contributor rules: a comment carries the
//! durable technical reason a change is the way it is, never the history of
//! how it was arrived at. This module is the mechanical half.
//!
//! It reads COMMENTS ONLY. A task identifier in a string literal (a
//! diagnostic ID, a rack position, a test name) is data and is left alone; the
//! same token inside a comment is a pointer into a tracker that outlives
//! nothing.
//!
//! `.rs` files ARE scanned: unlike `drift.rs`'s Qt/SDK-version rules, which
//! deliberately exempt Rust because a Qt version embedded in a test fixture
//! string is evidence, not build machinery, these rules police comment
//! *provenance*, and a task ID, commit hash or agent-authorship note in a Rust
//! comment is exactly as much provenance as the same note in C++ or QML. There
//! is no carve-out for this port's own source.
//!
//! Two scopes: `Diff` evaluates only the lines a change adds or touches (the
//! diff-scoped path blocks locally and in CI's per-PR run: the rules stay
//! adoptable by never first demanding a sweep of everything written before
//! them). `All` evaluates every tracked, scannable line (the informational
//! backlog report on a push or workflow dispatch, never a gate).

use std::collections::BTreeMap;
use std::path::Path;

use crate::git::Git;

const TRACKER_PREFIXES: &[&str] = &["QCR", "BUG", "TC", "VR", "TASK", "TICKET", "ISSUE", "JIRA"];

/// Rules that report but do not yet block: the tree they were written for
/// already carries a backlog under these, so a gate red on arrival would get
/// switched off rather than obeyed. `user-request-phrasing` is advisory for a
/// different reason: it cannot tell "the user asked for X" (provenance) from
/// "the user asked the recorder to do X" (product behavior) apart by pattern
/// alone, so it reports for a human to read and never fails a run.
pub const ADVISORY_RULES: &[&str] = &["task-id", "non-ascii-punctuation", "user-request-phrasing"];

/// Test fixtures are excluded for the reason `drift.rs` excludes them: a rule
/// with no rejected fixture has never been shown to reject anything, so the
/// fixtures contain, by construction, every shape the rules reject.
const EXCLUDED_PATTERN: &str = r"(?i)^scripts/tests/";

/// The directories this check scans. Documentation and top-level metadata
/// outside them legitimately narrate history and are not checked.
const SCANNED_ROOTS: &[&str] = &[
    "app/", "libs/", "apps/", "tools/", "scripts/", "cmake/", "tests/",
];

pub enum Scope {
    /// Only lines added or changed relative to `base`, plus uncommitted
    /// worktree and staged changes.
    Diff { base: String },
    /// Every line of every tracked, scannable file.
    All,
}

pub struct Finding {
    pub rule: &'static str,
    pub file: String,
    pub line: usize,
    pub value: String,
    pub fix: &'static str,
}

impl Finding {
    pub fn advisory(&self) -> bool {
        ADVISORY_RULES.contains(&self.rule)
    }
}

pub struct HygieneReport {
    pub findings: Vec<Finding>,
}

impl HygieneReport {
    pub fn blocking(&self) -> impl Iterator<Item = &Finding> {
        self.findings.iter().filter(|f| !f.advisory())
    }

    pub fn advisory(&self) -> impl Iterator<Item = &Finding> {
        self.findings.iter().filter(|f| f.advisory())
    }
}

pub fn check(repo_root: &Path, scope: Scope, only: Option<&str>) -> anyhow::Result<HygieneReport> {
    let git = Git::new(repo_root);
    // A missing/broken git repository is a refusal, discovered before any rule
    // runs, never an empty, falsely-clean finding list.
    let tracked = git.ls_files()?;

    let changed = match &scope {
        Scope::All => None,
        Scope::Diff { base } => Some(git.changed_line_ranges(base)?),
    };

    let mut files: Vec<String> = match &changed {
        None => tracked
            .into_iter()
            .filter(|f| in_scanned_scope(f))
            .collect(),
        Some(ranges) => ranges
            .keys()
            .filter(|f| in_scanned_scope(f))
            .cloned()
            .collect(),
    };
    files.sort();

    let excluded = regex::Regex::new(EXCLUDED_PATTERN).unwrap();
    let rules = hygiene_rules(&git, only);

    let mut findings = Vec::new();
    for relative in files {
        if excluded.is_match(&relative) {
            continue;
        }
        let Some(syntax) = comment_syntax_for(&relative) else {
            continue;
        };
        let path = repo_root.join(&relative);
        if !path.is_file() {
            continue;
        }
        let Ok(text) = std::fs::read_to_string(&path) else {
            continue;
        };
        if text.trim().is_empty() {
            continue;
        }

        let regions = comment_regions(&text, &syntax);
        for (number, comment) in &regions {
            if let Some(ranges) = &changed {
                let in_scope = ranges
                    .get(&relative)
                    .is_some_and(|rs| rs.iter().any(|r| r.contains(number)));
                if !in_scope {
                    continue;
                }
            }
            for rule in &rules {
                if let Some(finding) = rule.first_match(&relative, *number, comment) {
                    findings.push(finding);
                }
            }
        }
    }

    Ok(HygieneReport { findings })
}

fn in_scanned_scope(path: &str) -> bool {
    SCANNED_ROOTS.iter().any(|root| path.starts_with(root))
}

// ---------------------------------------------------------------------------
// Comment extraction
// ---------------------------------------------------------------------------

struct Syntax {
    line: &'static str,
    block: Option<(&'static str, &'static str)>,
}

fn comment_syntax_for(relative: &str) -> Option<Syntax> {
    let name = relative.rsplit(['/', '\\']).next().unwrap_or(relative);
    if name.eq_ignore_ascii_case("CMakeLists.txt") {
        return Some(Syntax {
            line: "#",
            block: None,
        });
    }
    let extension = name
        .rsplit_once('.')
        .map(|(_, ext)| ext.to_ascii_lowercase())?;
    match extension.as_str() {
        "cpp" | "cc" | "cxx" | "h" | "hpp" | "hxx" | "inl" | "cs" | "qml" | "js" | "mjs" | "ts"
        | "rs" => Some(Syntax {
            line: "//",
            block: Some(("/*", "*/")),
        }),
        "ps1" | "psm1" | "psd1" => Some(Syntax {
            line: "#",
            block: Some(("<#", "#>")),
        }),
        "cmake" | "py" | "yml" | "yaml" | "toml" | "cfg" => Some(Syntax {
            line: "#",
            block: None,
        }),
        _ => None,
    }
}

/// The comment text of a file, as a line number -> comment text map.
/// Deliberately a scanner, not a parser: it tracks block-comment state and
/// skips string literals well enough that a URL in code does not read as a
/// line comment, and does not attempt correctness for a raw string literal
/// containing a comment opener, which fails closed (a missed comment) rather
/// than open (a false report).
fn comment_regions(text: &str, syntax: &Syntax) -> BTreeMap<usize, String> {
    let mut regions = BTreeMap::new();
    let line_marker: Vec<char> = syntax.line.chars().collect();
    let block_markers = syntax.block.map(|(open, close)| {
        (
            open.chars().collect::<Vec<char>>(),
            close.chars().collect::<Vec<char>>(),
        )
    });

    let mut in_block = false;
    for (idx, line) in text.lines().enumerate() {
        let number = idx + 1;
        let chars: Vec<char> = line.chars().collect();
        let mut pos = 0usize;
        let mut comment = String::new();

        while pos < chars.len() {
            if in_block {
                let close = block_markers.as_ref().map(|(_, close)| close);
                let found = close.and_then(|close| find_at(&chars, pos, close));
                match found {
                    Some(at) => {
                        comment.extend(chars[pos..at].iter());
                        pos = at + close.unwrap().len();
                        in_block = false;
                    }
                    None => {
                        comment.extend(chars[pos..].iter());
                        pos = chars.len();
                    }
                }
                continue;
            }

            let ch = chars[pos];
            if ch == '"' || ch == '\'' {
                // Skip over string literals so their contents are never
                // treated as comment text: a task identifier in a string is
                // data.
                let quote = ch;
                pos += 1;
                while pos < chars.len() {
                    if chars[pos] == '\\' && quote == '"' {
                        pos += 2;
                        continue;
                    }
                    if chars[pos] == quote {
                        pos += 1;
                        break;
                    }
                    pos += 1;
                }
                continue;
            }

            if !line_marker.is_empty() && starts_with_at(&chars, pos, &line_marker) {
                comment.extend(chars[pos..].iter());
                break;
            }

            if let Some((open, _)) = &block_markers
                && starts_with_at(&chars, pos, open)
            {
                pos += open.len();
                in_block = true;
                continue;
            }

            pos += 1;
        }

        if !comment.trim().is_empty() {
            regions.insert(number, comment);
        }
    }
    regions
}

fn starts_with_at(chars: &[char], pos: usize, needle: &[char]) -> bool {
    pos + needle.len() <= chars.len() && chars[pos..pos + needle.len()] == *needle
}

fn find_at(chars: &[char], from: usize, needle: &[char]) -> Option<usize> {
    if needle.is_empty() || from > chars.len().saturating_sub(needle.len()) {
        return None;
    }
    (from..=chars.len() - needle.len()).find(|&i| chars[i..i + needle.len()] == *needle)
}

// ---------------------------------------------------------------------------
// Rules
// ---------------------------------------------------------------------------

enum Matcher {
    Regex(regex::Regex),
    /// A run of two to four backslashes, not immediately followed by the
    /// Win32 device-namespace or named-pipe prefix, then a non-blank path
    /// segment and a closing backslash. Ported as a function rather than as a
    /// single pattern because expressing the exclusion needs a negative
    /// lookahead, which the `regex` crate does not support; see
    /// `unc_path_matches`.
    UncPath,
}

/// Regex alone cannot tell a short hash from an ordinary hex-looking word;
/// `commit-hash` asks git whether a candidate really names an object instead.
type Confirm<'a> = Box<dyn Fn(&str) -> bool + 'a>;

struct Rule<'a> {
    name: &'static str,
    matcher: Matcher,
    fix: &'static str,
    confirm: Option<Confirm<'a>>,
}

impl Rule<'_> {
    fn first_match(&self, file: &str, line: usize, comment: &str) -> Option<Finding> {
        let candidates: Vec<String> = match &self.matcher {
            Matcher::Regex(re) => re
                .find_iter(comment)
                .map(|m| m.as_str().trim().to_string())
                .collect(),
            Matcher::UncPath => unc_path_matches(comment),
        };
        for value in candidates {
            if let Some(confirm) = &self.confirm
                && !confirm(&value)
            {
                continue;
            }
            return Some(Finding {
                rule: self.name,
                file: file.to_string(),
                line,
                value,
                fix: self.fix,
            });
        }
        None
    }
}

fn unc_path_matches(text: &str) -> Vec<String> {
    let run = regex::Regex::new(r"\\{2,4}").unwrap();
    let tail = regex::Regex::new(r"^[^\\\s]+\\").unwrap();
    let mut results = Vec::new();
    for m in run.find_iter(text) {
        let after = &text[m.end()..];
        let excluded = match after.chars().next() {
            Some('.') | Some('?') => after[1..].starts_with('\\'),
            _ => after.starts_with("pipe\\"),
        };
        if excluded {
            continue;
        }
        if let Some(t) = tail.find(after) {
            results.push(format!("{}{}", m.as_str(), t.as_str()));
        }
    }
    results
}

fn hygiene_rules<'a>(git: &'a Git, only: Option<&str>) -> Vec<Rule<'a>> {
    let trackers = TRACKER_PREFIXES
        .iter()
        .map(|p| regex::escape(p))
        .collect::<Vec<_>>()
        .join("|");

    let mut rules = vec![
        Rule {
            name: "task-id",
            matcher: Matcher::Regex(
                regex::Regex::new(&format!(r"\b(?:{trackers})-\d{{1,4}}\b")).unwrap(),
            ),
            fix: "Remove the tracker reference; keep the technical reason it produced.",
            confirm: None,
        },
        Rule {
            name: "commit-hash",
            matcher: Matcher::Regex(regex::Regex::new(r"\b[0-9a-fA-F]{7,40}\b").unwrap()),
            fix: "Remove the commit reference; a comment outlives the history it points at.",
            // Regex alone cannot tell a short hash from an ordinary hex-looking
            // word; asking git removes the guess.
            confirm: Some(Box::new(move |value: &str| is_commit(git, value))),
        },
        Rule {
            name: "issue-reference",
            matcher: Matcher::Regex(regex::Regex::new(r"(?:^|[\s(\[])#\d+\b").unwrap()),
            fix: "Remove the issue or PR number; state the constraint it describes instead.",
            confirm: None,
        },
        Rule {
            name: "private-workspace",
            matcher: Matcher::Regex(
                regex::Regex::new(r"(?:^|[\s(\\/])\.workspace(?:[\\/]|\b)").unwrap(),
            ),
            fix: ".workspace/ is untracked planning context; committed source must not point at it.",
            confirm: None,
        },
        Rule {
            name: "absolute-path",
            matcher: Matcher::Regex(regex::Regex::new(r"\b[A-Za-z]:[\\/]").unwrap()),
            fix: "Remove the machine-specific path; name the thing, not where it sits on one machine.",
            confirm: None,
        },
        Rule {
            name: "unc-path",
            matcher: Matcher::UncPath,
            fix: "Remove the network path; it is meaningless outside one network.",
            confirm: None,
        },
        Rule {
            name: "conversation-provenance",
            matcher: Matcher::Regex(
                regex::Regex::new(
                    r"\b(?:user (?:said|decided|mentioned)|as (?:discussed|requested)|per (?:our|the) (?:discussion|conversation)|(?:[Mm]y|[Oo]ur) previous attempt|earlier session)\b",
                )
                .unwrap(),
            ),
            fix: "Drop how the decision was reached; keep what the decision is.",
            confirm: None,
        },
        Rule {
            name: "user-request-phrasing",
            matcher: Matcher::Regex(regex::Regex::new(r"\buser (?:asked|requested|wanted)\b").unwrap()),
            fix: "If this records who asked for the change, drop it; if it describes what an end user did, keep it.",
            confirm: None,
        },
        Rule {
            name: "agent-provenance",
            matcher: Matcher::Regex(
                regex::Regex::new(
                    r"\b(?:Claude|Codex|ChatGPT|Copilot|Gemini)\b\s*(?:Code\s*)?(?:said|suggested|decided|added|implemented|changed|wrote|generated)\b",
                )
                .unwrap(),
            ),
            fix: "Source does not record who wrote it.",
            confirm: None,
        },
    ];

    let branches = repo_branch_names(git);
    if !branches.is_empty() {
        let pattern = format!(r"\b(?:{})\b", branches.join("|"));
        rules.push(Rule {
            name: "branch-reference",
            matcher: Matcher::Regex(regex::Regex::new(&pattern).unwrap()),
            fix: "Remove the branch name; branches are deleted, the code is not.",
            confirm: None,
        });
    }

    rules.push(Rule {
        name: "non-ascii-punctuation",
        matcher: Matcher::Regex(
            regex::Regex::new("[\u{2010}-\u{2015}\u{2018}\u{2019}\u{201C}\u{201D}\u{2026}]")
                .unwrap(),
        ),
        fix: "Developer-facing source documentation uses plain ASCII punctuation.",
        confirm: None,
    });

    if let Some(only) = only {
        rules.retain(|r| r.name == only);
    }
    rules
}

/// True when `candidate` names an object in `git`'s repository.
fn is_commit(git: &Git, candidate: &str) -> bool {
    let spec = format!("{candidate}^{{commit}}");
    git.run(&["rev-parse", "--verify", "--quiet", &spec]).0 == 0
}

/// Distinctive local/remote branch names: `main`/`master`/`HEAD` name
/// themselves in prose constantly and are excluded, and anything shorter than
/// six characters is too common a word fragment to mean a branch.
fn repo_branch_names(git: &Git) -> Vec<String> {
    let (_, stdout) = git.run(&[
        "for-each-ref",
        "--format=%(refname:short)",
        "refs/heads",
        "refs/remotes",
    ]);
    let mut names: Vec<String> = stdout
        .lines()
        .filter_map(|line| {
            let name = line.trim();
            if name.is_empty() {
                return None;
            }
            let short = name.strip_prefix("origin/").unwrap_or(name);
            if matches!(short, "main" | "master" | "HEAD") || short.len() < 6 {
                return None;
            }
            Some(regex::escape(short))
        })
        .collect();
    names.sort();
    names.dedup();
    names
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_support::{fixture_repo, fixture_repo_committed, write_files};
    use std::path::Path;

    fn run_git(dir: &Path, args: &[&str]) -> String {
        let output = std::process::Command::new("git")
            .args(args)
            .current_dir(dir)
            .output()
            .unwrap();
        String::from_utf8_lossy(&output.stdout).trim().to_string()
    }

    fn diff_at_head() -> Scope {
        Scope::Diff {
            base: "HEAD".into(),
        }
    }

    // -- rule coverage, by comment syntax --------------------------------

    #[test]
    fn task_id_in_a_line_comment_is_advisory() {
        let dir = fixture_repo_committed(&[("app/thing.cpp", "int x = 0;\n")]);
        write_files(
            dir.path(),
            &[("app/thing.cpp", "// BUG-42: fix later\nint x = 0;\n")],
        );
        let report = check(dir.path(), diff_at_head(), None).unwrap();
        assert!(report.blocking().next().is_none());
        assert!(
            report
                .advisory()
                .any(|f| f.rule == "task-id" && f.value == "BUG-42")
        );
    }

    #[test]
    fn task_id_in_a_hash_comment_is_advisory() {
        let dir = fixture_repo_committed(&[("scripts/thing.ps1", "Write-Host 'ok'\n")]);
        write_files(
            dir.path(),
            &[("scripts/thing.ps1", "# TICKET-7 pending\nWrite-Host 'ok'\n")],
        );
        let report = check(dir.path(), diff_at_head(), None).unwrap();
        assert!(report.advisory().any(|f| f.rule == "task-id"));
    }

    #[test]
    fn task_id_inside_a_block_comment_is_found() {
        let dir = fixture_repo_committed(&[("app/thing.h", "int x;\n")]);
        write_files(
            dir.path(),
            &[("app/thing.h", "/* JIRA-99 still open */\nint x;\n")],
        );
        let report = check(dir.path(), diff_at_head(), None).unwrap();
        assert!(
            report
                .advisory()
                .any(|f| f.rule == "task-id" && f.value == "JIRA-99")
        );
    }

    #[test]
    fn task_id_inside_a_string_literal_is_not_a_violation() {
        let dir = fixture_repo_committed(&[("app/thing.cpp", "int x = 0;\n")]);
        write_files(
            dir.path(),
            &[(
                "app/thing.cpp",
                "const char* id = \"BUG-42\";\nint x = 0;\n",
            )],
        );
        let report = check(dir.path(), diff_at_head(), None).unwrap();
        assert!(report.findings.is_empty());
    }

    #[test]
    fn a_real_commit_hash_in_a_comment_is_rejected() {
        let dir = fixture_repo_committed(&[("app/thing.cpp", "int x = 0;\n")]);
        let head = run_git(dir.path(), &["rev-parse", "HEAD"]);
        write_files(
            dir.path(),
            &[(
                "app/thing.cpp",
                &format!("// see {head} for the previous shape\nint x = 0;\n"),
            )],
        );
        let report = check(dir.path(), diff_at_head(), None).unwrap();
        assert!(
            report
                .blocking()
                .any(|f| f.rule == "commit-hash" && f.value == head)
        );
    }

    #[test]
    fn a_hex_looking_word_that_is_not_a_real_commit_is_not_rejected() {
        let dir = fixture_repo_committed(&[("app/thing.cpp", "int x = 0;\n")]);
        write_files(
            dir.path(),
            &[(
                "app/thing.cpp",
                "// deadbeef is not a commit here\nint x = 0;\n",
            )],
        );
        let report = check(dir.path(), diff_at_head(), None).unwrap();
        assert!(report.blocking().all(|f| f.rule != "commit-hash"));
    }

    #[test]
    fn an_issue_reference_is_rejected() {
        let dir = fixture_repo_committed(&[("app/thing.cpp", "int x = 0;\n")]);
        write_files(
            dir.path(),
            &[("app/thing.cpp", "// fixes #123\nint x = 0;\n")],
        );
        let report = check(dir.path(), diff_at_head(), None).unwrap();
        assert!(report.blocking().any(|f| f.rule == "issue-reference"));
    }

    #[test]
    fn a_workspace_path_reference_is_rejected() {
        let dir = fixture_repo_committed(&[("app/thing.cpp", "int x = 0;\n")]);
        write_files(
            dir.path(),
            &[(
                "app/thing.cpp",
                "// see .workspace/plan.md for the design\nint x = 0;\n",
            )],
        );
        let report = check(dir.path(), diff_at_head(), None).unwrap();
        assert!(report.blocking().any(|f| f.rule == "private-workspace"));
    }

    #[test]
    fn an_absolute_windows_path_is_rejected() {
        let dir = fixture_repo_committed(&[("app/thing.cpp", "int x = 0;\n")]);
        write_files(
            dir.path(),
            &[(
                "app/thing.cpp",
                "// lives at C:/Users/dev/scratch\nint x = 0;\n",
            )],
        );
        let report = check(dir.path(), diff_at_head(), None).unwrap();
        assert!(report.blocking().any(|f| f.rule == "absolute-path"));
    }

    #[test]
    fn a_unc_network_path_is_rejected() {
        let dir = fixture_repo_committed(&[("app/thing.cpp", "int x = 0;\n")]);
        write_files(
            dir.path(),
            &[(
                "app/thing.cpp",
                "// copy from \\\\buildserver\\share\\artifact\nint x = 0;\n",
            )],
        );
        let report = check(dir.path(), diff_at_head(), None).unwrap();
        assert!(report.blocking().any(|f| f.rule == "unc-path"));
    }

    #[test]
    fn a_named_pipe_path_is_not_a_unc_violation() {
        let dir = fixture_repo_committed(&[("app/thing.cpp", "int x = 0;\n")]);
        write_files(
            dir.path(),
            &[(
                "app/thing.cpp",
                "// talks to \\\\.\\pipe\\exosnap-control\nint x = 0;\n",
            )],
        );
        let report = check(dir.path(), diff_at_head(), None).unwrap();
        assert!(report.blocking().all(|f| f.rule != "unc-path"));
    }

    #[test]
    fn conversation_provenance_phrasing_is_rejected() {
        let dir = fixture_repo_committed(&[("app/thing.cpp", "int x = 0;\n")]);
        write_files(
            dir.path(),
            &[(
                "app/thing.cpp",
                "// as discussed, this stays synchronous\nint x = 0;\n",
            )],
        );
        let report = check(dir.path(), diff_at_head(), None).unwrap();
        assert!(
            report
                .blocking()
                .any(|f| f.rule == "conversation-provenance")
        );
    }

    #[test]
    fn user_request_phrasing_is_advisory_only() {
        let dir = fixture_repo_committed(&[("app/thing.cpp", "int x = 0;\n")]);
        write_files(
            dir.path(),
            &[(
                "app/thing.cpp",
                "// user requested a slower fade here\nint x = 0;\n",
            )],
        );
        let report = check(dir.path(), diff_at_head(), None).unwrap();
        assert!(report.blocking().all(|f| f.rule != "user-request-phrasing"));
        assert!(report.advisory().any(|f| f.rule == "user-request-phrasing"));
    }

    #[test]
    fn agent_provenance_is_rejected() {
        let dir = fixture_repo_committed(&[("app/thing.cpp", "int x = 0;\n")]);
        write_files(
            dir.path(),
            &[("app/thing.cpp", "// Claude added this guard\nint x = 0;\n")],
        );
        let report = check(dir.path(), diff_at_head(), None).unwrap();
        assert!(report.blocking().any(|f| f.rule == "agent-provenance"));
    }

    #[test]
    fn a_distinctive_branch_name_in_a_comment_is_rejected() {
        let dir = fixture_repo_committed(&[("app/thing.cpp", "int x = 0;\n")]);
        run_git(dir.path(), &["branch", "phase-b-bulk-port"]);
        write_files(
            dir.path(),
            &[(
                "app/thing.cpp",
                "// landed on phase-b-bulk-port originally\nint x = 0;\n",
            )],
        );
        let report = check(dir.path(), diff_at_head(), None).unwrap();
        assert!(report.blocking().any(|f| f.rule == "branch-reference"));
    }

    #[test]
    fn a_short_branch_name_never_becomes_a_rule() {
        let dir = fixture_repo_committed(&[("app/thing.cpp", "int x = 0;\n")]);
        run_git(dir.path(), &["branch", "abc"]);
        write_files(
            dir.path(),
            &[("app/thing.cpp", "// see abc for context\nint x = 0;\n")],
        );
        let report = check(dir.path(), diff_at_head(), None).unwrap();
        assert!(report.findings.iter().all(|f| f.rule != "branch-reference"));
    }

    #[test]
    fn non_ascii_dash_is_advisory() {
        let dir = fixture_repo_committed(&[("app/thing.cpp", "int x = 0;\n")]);
        write_files(
            dir.path(),
            &[(
                "app/thing.cpp",
                "// note \u{2014} keep this short\nint x = 0;\n",
            )],
        );
        let report = check(dir.path(), diff_at_head(), None).unwrap();
        assert!(report.advisory().any(|f| f.rule == "non-ascii-punctuation"));
    }

    // -- scope --------------------------------------------------------------

    #[test]
    fn an_untouched_line_in_a_touched_file_is_not_flagged_in_diff_scope() {
        let dir =
            fixture_repo_committed(&[("app/thing.cpp", "// fixes #999\nint x = 0;\nint y = 0;\n")]);
        write_files(
            dir.path(),
            &[("app/thing.cpp", "// fixes #999\nint x = 1;\nint y = 0;\n")],
        );
        let report = check(dir.path(), diff_at_head(), None).unwrap();
        assert!(report.findings.iter().all(|f| f.rule != "issue-reference"));
    }

    #[test]
    fn the_same_untouched_line_is_flagged_in_whole_tree_scope() {
        let dir =
            fixture_repo_committed(&[("app/thing.cpp", "// fixes #999\nint x = 0;\nint y = 0;\n")]);
        write_files(
            dir.path(),
            &[("app/thing.cpp", "// fixes #999\nint x = 1;\nint y = 0;\n")],
        );
        let report = check(dir.path(), Scope::All, None).unwrap();
        assert!(report.findings.iter().any(|f| f.rule == "issue-reference"));
    }

    #[test]
    fn a_changed_line_in_diff_scope_is_flagged() {
        let dir = fixture_repo_committed(&[("app/thing.cpp", "int x = 0;\n")]);
        write_files(
            dir.path(),
            &[("app/thing.cpp", "// fixes #999\nint x = 0;\n")],
        );
        let report = check(dir.path(), diff_at_head(), None).unwrap();
        assert!(report.blocking().any(|f| f.rule == "issue-reference"));
    }

    #[test]
    fn a_staged_change_is_visible_to_diff_scope() {
        let dir = fixture_repo_committed(&[("app/thing.cpp", "int x = 0;\n")]);
        write_files(
            dir.path(),
            &[("app/thing.cpp", "// fixes #999\nint x = 0;\n")],
        );
        run_git(dir.path(), &["add", "-A"]);
        let report = check(dir.path(), diff_at_head(), None).unwrap();
        assert!(report.blocking().any(|f| f.rule == "issue-reference"));
    }

    #[test]
    fn only_restricts_to_the_named_rule() {
        let dir = fixture_repo_committed(&[("app/thing.cpp", "int x = 0;\n")]);
        write_files(
            dir.path(),
            &[(
                "app/thing.cpp",
                "// fixes #999, see .workspace/plan.md\nint x = 0;\n",
            )],
        );
        let report = check(dir.path(), diff_at_head(), Some("issue-reference")).unwrap();
        assert!(!report.findings.is_empty());
        assert!(report.findings.iter().all(|f| f.rule == "issue-reference"));
    }

    #[test]
    fn scripts_tests_directory_is_excluded_entirely() {
        let dir = fixture_repo(&[(
            "scripts/tests/fixture.ps1",
            "# fixes #999\nWrite-Host 'ok'\n",
        )]);
        let report = check(dir.path(), diff_at_head(), None).unwrap();
        assert!(report.findings.is_empty());
    }

    #[test]
    fn files_outside_the_scanned_roots_are_ignored() {
        let dir = fixture_repo(&[("docs/history.md", "not scanned anyway (no comment syntax)")]);
        let report = check(dir.path(), diff_at_head(), None).unwrap();
        assert!(report.findings.is_empty());
    }

    #[test]
    fn a_rust_source_comment_is_scanned_like_any_other_language() {
        let dir = fixture_repo_committed(&[("tools/exo-dev/src/thing.rs", "fn f() {}\n")]);
        write_files(
            dir.path(),
            &[("tools/exo-dev/src/thing.rs", "// fixes #999\nfn f() {}\n")],
        );
        let report = check(dir.path(), diff_at_head(), None).unwrap();
        assert!(
            report
                .blocking()
                .any(|f| f.rule == "issue-reference" && f.file.ends_with("thing.rs"))
        );
    }

    #[test]
    fn a_clean_change_reports_nothing() {
        let dir = fixture_repo_committed(&[("app/thing.cpp", "int x = 0;\n")]);
        write_files(
            dir.path(),
            &[(
                "app/thing.cpp",
                "// keeps the invariant that x starts at zero\nint x = 0;\n",
            )],
        );
        let report = check(dir.path(), diff_at_head(), None).unwrap();
        assert!(report.findings.is_empty());
    }

    #[test]
    fn a_directory_that_is_not_a_git_repository_is_a_hard_error_not_zero_findings() {
        let dir = tempfile::tempdir().unwrap();
        let result = check(dir.path(), Scope::All, None);
        assert!(result.is_err());
    }
}
