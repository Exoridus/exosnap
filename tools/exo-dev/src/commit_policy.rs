//! Reads a commit subject the way the changelog and the release notes read it.
//!
//! One parser, three consumers: `check` (this module's own CLI/verify-step
//! wrapper), the changelog assembler and the release-notes renderer. The
//! repository squash-merges with the pull request title alone, so a merged
//! commit has no body: everything the cut ever reads has to be in the
//! subject. That is why the trailing pull request number is part of the
//! grammar rather than a convention, and why a breaking change is marked
//! with `!` -- a `BREAKING CHANGE:` footer would have nowhere to survive the
//! squash.
//!
//! Grandfathering is mechanical rather than a date someone maintains: the
//! policy takes effect at the commit that added
//! `scripts/lib/CommitPolicy.psm1` (`POLICY_EPOCH`), and anything reachable
//! only from that commit's parent is out of scope. A tree where that commit
//! is not reachable has nothing in scope at all, which is the honest answer
//! during the change that introduces it, not a pass.
//!
//! One subject line, three points in its life, and the trailing pull request
//! number belongs to exactly one of them:
//!
//!   local commit         `type(scope): summary`            no number
//!   pull request title   `type(scope): summary`            no number
//!   merged subject        `type(scope): summary (#N)`      exactly one number
//!
//! The number does not exist until the pull request is opened, and the squash
//! merge appends it unconditionally, so requiring it of the title would
//! demand a value that has to be filled in after creation. `own_pull_request`
//! is the title mode: it rejects a title that already carries its own number,
//! and leaves a citation of another pull request alone.
//! `require_pull_request` is the merged-subject mode, for checking a line
//! that is already on main.

use std::path::Path;
use std::sync::LazyLock;

use regex::Regex;

use crate::git::Git;

static SUBJECT_PATTERN: LazyLock<Regex> = LazyLock::new(|| {
    Regex::new(r"^(?P<type>[a-z]+)(?:\((?P<scope>[^()]+)\))?(?P<breaking>!)?: (?P<summary>.+?)(?: \(#(?P<pr>[0-9]+)\))?$").unwrap()
});

/// Section `None` means "this type produces no changelog entry". Absence
/// would be indistinguishable from an unknown type, which has to be
/// reported, not skipped.
pub const TYPE_SECTIONS: &[(&str, Option<&str>)] = &[
    ("feat", Some("Added")),
    ("fix", Some("Fixed")),
    ("perf", Some("Fixed")),
    ("refactor", Some("Changed")),
    ("docs", Some("Documentation")),
    ("ci", None),
    ("build", None),
    ("test", None),
    ("chore", None),
    ("style", None),
];

/// The order sections appear in a release. Breaking changes lead because they
/// are the only entries a reader has to act on before upgrading.
pub const SECTION_ORDER: &[&str] = &["Changed", "Added", "Fixed", "Documentation"];

/// The commit that added `scripts/lib/CommitPolicy.psm1`. The production
/// default; a caller with its own history (a test fixture, a shallow clone)
/// supplies `CommitPolicyOptions::epoch` instead.
pub const POLICY_EPOCH: &str = "9ad8d3d7126cc931ec3aec6cf9f9e1f2110acdd5";

const CHANGELOG_PATH: &str = "CHANGELOG.md";

pub struct ParsedCommit {
    pub subject: String,
    pub valid: bool,
    pub kind: Option<String>,
    pub scope: Option<String>,
    pub breaking: bool,
    pub summary: Option<String>,
    pub pull_request: Option<u32>,
    pub section: Option<&'static str>,
    pub problem: Option<String>,
}

fn section_for(kind: &str) -> Option<&'static str> {
    TYPE_SECTIONS
        .iter()
        .find(|(k, _)| *k == kind)
        .and_then(|(_, s)| *s)
}

fn known_types() -> String {
    TYPE_SECTIONS
        .iter()
        .map(|(k, _)| *k)
        .collect::<Vec<_>>()
        .join(", ")
}

/// Parses one commit subject into the fields the changelog reads.
///
/// Always returns a `ParsedCommit`. `valid` says whether the subject matched
/// the grammar, and `problem` says what a contributor has to change. `section`
/// is `None` both for a type that produces no entry and for a subject that
/// could not be parsed; `valid` tells the two apart.
pub fn parse(
    subject: &str,
    require_pull_request: bool,
    own_pull_request: Option<u32>,
) -> ParsedCommit {
    let mut result = ParsedCommit {
        subject: subject.to_string(),
        valid: false,
        kind: None,
        scope: None,
        breaking: false,
        summary: None,
        pull_request: None,
        section: None,
        problem: None,
    };

    let trimmed = subject.trim();
    if trimmed.is_empty() {
        result.problem = Some("the subject is empty".into());
        return result;
    }

    let Some(captures) = SUBJECT_PATTERN.captures(trimmed) else {
        result.problem =
            Some("does not read as 'type(scope): summary' -- see CONTRIBUTING.md".into());
        return result;
    };

    let kind = captures["type"].to_string();
    if !TYPE_SECTIONS.iter().any(|(k, _)| *k == kind) {
        result.kind = Some(kind.clone());
        result.problem = Some(format!("'{kind}' is not a known type ({})", known_types()));
        return result;
    }

    result.kind = Some(kind.clone());
    result.scope = captures.name("scope").map(|m| m.as_str().to_string());
    result.breaking = captures.name("breaking").is_some();
    let mut summary = captures["summary"].trim().to_string();
    result.pull_request = captures.name("pr").and_then(|m| m.as_str().parse().ok());

    // The squash append is unconditional, so a pull request title that
    // already ended in its own number arrives with that number twice. Only
    // the repetition is redundant: a different number in the same position
    // cites another pull request and has to survive.
    if let Some(pr) = result.pull_request {
        let repeated = format!(" (#{pr})");
        if summary.ends_with(&repeated) {
            summary.truncate(summary.len() - repeated.len());
            summary = summary.trim_end().to_string();
        }
    }
    if summary.is_empty() {
        result.problem = Some("the summary is empty".into());
        return result;
    }
    result.summary = Some(summary);

    if let (Some(own), Some(pr)) = (own_pull_request, result.pull_request)
        && own == pr
    {
        result.problem = Some(format!(
            "the title ends in its own pull request number (#{own}); the squash merge appends it, so leave it off"
        ));
        return result;
    }
    if require_pull_request && result.pull_request.is_none() {
        result.problem = Some(
            "the subject does not end in its pull request number, in parentheses after the summary"
                .into(),
        );
        return result;
    }

    // A breaking change is a Changed entry whatever its type says: a breaking
    // fix filed under Fixed would be read as a bugfix a reader can take blind.
    result.section = if result.breaking {
        Some("Changed")
    } else {
        section_for(&kind)
    };
    result.valid = true;
    result
}

/// The subject a squash merge lands on main: the title with its pull request
/// number appended exactly once. Rebuilt from the parsed fields rather than
/// by concatenating onto the raw title, so a title that already carried its
/// own number produces the same line as one that did not.
pub fn format_merge_subject(commit: &ParsedCommit, pull_request: u32) -> String {
    let scope = commit
        .scope
        .as_deref()
        .map(|s| format!("({s})"))
        .unwrap_or_default();
    let breaking = if commit.breaking { "!" } else { "" };
    format!(
        "{}{scope}{breaking}: {} (#{pull_request})",
        commit.kind.as_deref().unwrap_or_default(),
        commit.summary.as_deref().unwrap_or_default()
    )
}

/// One changelog line for a parsed subject. The summary is bolded and the
/// pull request linked; the description is not copied in.
pub fn format_changelog_entry(commit: &ParsedCommit, repository_url: &str) -> String {
    let mut summary = commit.summary.clone().unwrap_or_default();
    if !summary.ends_with(['.', '!', '?']) {
        summary.push('.');
    }
    if commit.breaking {
        summary = format!("BREAKING: {summary}");
    }
    let mut line = format!("- **{summary}**");
    if let Some(pr) = commit.pull_request {
        line.push_str(&format!(" ([#{pr}]({repository_url}/pull/{pr}))"));
    }
    line
}

// ---------------------------------------------------------------------------
// The `check commit-policy` rules: commit-subject and changelog-untouched
// ---------------------------------------------------------------------------

/// The policy epoch, injectable so a test fixture can supply its own history
/// instead of `POLICY_EPOCH`'s real, pinned commit.
#[derive(Default)]
pub struct CommitPolicyOptions {
    pub epoch: Option<String>,
}

pub struct CheckRequest<'a> {
    /// Check this single subject instead of the branch's commits.
    pub subject: Option<&'a str>,
    /// The number of the pull request `subject` is the title of. Rejects a
    /// title that already ends in that number. Title mode only.
    pub pull_request_number: Option<u32>,
    /// Require `subject` to end in a pull request number. The merged-subject
    /// mode; not for a title. Ignored on the branch walk.
    pub require_pull_request: bool,
    /// Commit to diff and log against. Defaults to the merge base with
    /// origin/next, then origin/main, next, main.
    pub base: Option<&'a str>,
    /// Restrict the run to this named rule (`commit-subject` or
    /// `changelog-untouched`).
    pub only: Option<&'a str>,
    /// Declares this run the release cut: `changelog-untouched` becomes a
    /// note instead of a violation.
    pub changelog_cut: bool,
}

pub struct CheckReport {
    pub violations: Vec<String>,
    pub notes: Vec<String>,
}

impl CheckReport {
    pub fn ok(&self) -> bool {
        self.violations.is_empty()
    }
}

fn rule_enabled(only: Option<&str>, name: &str) -> bool {
    only.is_none_or(|o| o == name)
}

/// The first commit the policy applies to, or `None` when it is not
/// reachable from `HEAD` (the module is not committed yet, or a fixture/
/// shallow clone never contains it). Not an error: "nothing in scope" is the
/// honest answer, not a refusal.
fn resolve_epoch(git: &Git, options: &CommitPolicyOptions) -> Option<String> {
    let epoch = options.epoch.as_deref().unwrap_or(POLICY_EPOCH);
    let spec = format!("{epoch}^{{commit}}");
    (git.run(&["rev-parse", "--verify", "--quiet", &spec]).0 == 0).then(|| epoch.to_string())
}

fn resolve_base(git: &Git, base: Option<&str>) -> Option<String> {
    if let Some(base) = base {
        return Some(base.to_string());
    }
    for candidate in ["origin/next", "origin/main", "next", "main"] {
        let (code, stdout) = git.run(&["merge-base", "HEAD", candidate]);
        let trimmed = stdout.trim();
        if code == 0 && !trimmed.is_empty() {
            return Some(trimmed.to_string());
        }
    }
    None
}

pub fn check(
    repo_root: &Path,
    request: &CheckRequest,
    options: &CommitPolicyOptions,
) -> anyhow::Result<CheckReport> {
    let git = Git::new(repo_root);
    anyhow::ensure!(
        git.head().is_some(),
        "commit-policy: '{}' is not a git repository",
        repo_root.display()
    );

    let mut violations = Vec::new();
    let mut notes = Vec::new();

    if rule_enabled(request.only, "commit-subject") {
        check_commit_subject(&git, request, options, &mut violations, &mut notes);
    }
    if rule_enabled(request.only, "changelog-untouched") {
        check_changelog_untouched(&git, request, options, &mut violations, &mut notes);
    }

    Ok(CheckReport { violations, notes })
}

fn check_commit_subject(
    git: &Git,
    request: &CheckRequest,
    options: &CommitPolicyOptions,
    violations: &mut Vec<String>,
    notes: &mut Vec<String>,
) {
    if let Some(subject) = request.subject {
        let parsed = parse(
            subject,
            request.require_pull_request,
            request.pull_request_number,
        );
        if !parsed.valid {
            violations.push(format!(
                "commit-subject: '{subject}' -- {}",
                parsed.problem.as_deref().unwrap_or("invalid")
            ));
        } else {
            notes.push(format!(
                "commit-subject: '{subject}' files under {}",
                parsed.section.unwrap_or("no changelog section")
            ));
        }
        return;
    }

    let Some(epoch) = resolve_epoch(git, options) else {
        notes.push(
            "commit-subject: the policy module is not committed yet; no commit is in scope"
                .to_string(),
        );
        return;
    };

    let Some(base_ref) = resolve_base(git, request.base) else {
        violations.push(
            "commit-subject: no development or Stable base could be resolved, so the branch range is unknown"
                .to_string(),
        );
        return;
    };

    // Two ranges intersected: what this branch adds, and what the policy
    // covers. %P is asked for so a merge commit can be recognised by having
    // more than one parent. A merge commit's subject is written by git, not
    // by an author, and the squash merge that lands a pull request can never
    // produce it, so it can never become the subject the changelog cut
    // parses. Counted and reported, not judged.
    let unit = '\u{1f}';
    let format_arg = format!("--format=%H{unit}%P{unit}%s");
    let range_arg = format!("{base_ref}..HEAD");
    let exclude_arg = format!("^{epoch}");
    let (_, stdout) = git.run(&["log", &format_arg, &range_arg, &exclude_arg]);

    let mut checked = 0usize;
    let mut merges = 0usize;
    for line in stdout.lines() {
        if line.is_empty() {
            continue;
        }
        let mut parts = line.splitn(3, unit);
        let hash = parts.next().unwrap_or_default();
        let parents = parts.next().unwrap_or_default();
        let subject_text = parts.next().unwrap_or_default();
        if parents.split(' ').filter(|p| !p.is_empty()).count() > 1 {
            merges += 1;
            continue;
        }
        checked += 1;
        let parsed = parse(subject_text, false, None);
        if !parsed.valid {
            let short = &hash[..hash.len().min(8)];
            violations.push(format!(
                "commit-subject: {short} '{subject_text}' -- {}",
                parsed.problem.as_deref().unwrap_or("invalid")
            ));
        }
    }

    let mut scope = format!(
        "commit-subject: {checked} commit(s) in scope since {}",
        &epoch[..epoch.len().min(8)]
    );
    if merges > 0 {
        scope.push_str(&format!("; {merges} merge commit(s) not judged"));
    }
    notes.push(scope);
}

fn check_changelog_untouched(
    git: &Git,
    request: &CheckRequest,
    options: &CommitPolicyOptions,
    violations: &mut Vec<String>,
    notes: &mut Vec<String>,
) {
    let epoch = resolve_epoch(git, options);
    if request.changelog_cut {
        notes.push(
            "changelog-untouched: skipped, EXOSNAP_CHANGELOG_CUT=1 declares this the release cut"
                .to_string(),
        );
        return;
    }
    if request.subject.is_some() {
        notes
            .push("changelog-untouched: not applicable when checking a single subject".to_string());
        return;
    }
    let Some(epoch) = epoch else {
        // Same epoch as commit-subject, for the same reason and one more: the
        // commit that introduces the policy is also the one that creates the
        // file the policy is about. Scoping the rule to what came after it is
        // what lets it be adopted without an escape hatch on its own first
        // commit.
        notes.push(
            "changelog-untouched: the policy module is not committed yet; no change is in scope"
                .to_string(),
        );
        return;
    };

    let range = format!("{epoch}..HEAD");
    let mut touched = false;
    let (_, committed) = git.run(&["diff", "--name-only", &range]);
    if committed.lines().any(|l| l.trim() == CHANGELOG_PATH) {
        touched = true;
    }
    let (_, unstaged) = git.run(&["diff", "--name-only"]);
    if unstaged.lines().any(|l| l.trim() == CHANGELOG_PATH) {
        touched = true;
    }
    let (_, staged) = git.run(&["diff", "--cached", "--name-only"]);
    if staged.lines().any(|l| l.trim() == CHANGELOG_PATH) {
        touched = true;
    }

    if touched {
        violations.push(format!(
            "changelog-untouched: this branch writes {CHANGELOG_PATH}. The release cut assembles it from commit subjects (scripts/new-changelog.ps1); set EXOSNAP_CHANGELOG_CUT=1 when that is what this is."
        ));
    } else {
        notes.push(format!(
            "changelog-untouched: {CHANGELOG_PATH} is unchanged"
        ));
    }
}

/// Renders a report the way `check-commit-policy.ps1` printed it: notes
/// first, then a blank line and a `FAIL` line per violation, then the count.
pub fn render(report: &CheckReport) -> String {
    let mut out = String::new();
    for note in &report.notes {
        out.push_str("  ");
        out.push_str(note);
        out.push('\n');
    }
    if report.violations.is_empty() {
        out.push_str("commit policy: OK\n");
    } else {
        out.push('\n');
        for violation in &report.violations {
            out.push_str("FAIL  ");
            out.push_str(violation);
            out.push('\n');
        }
        out.push('\n');
        out.push_str(&format!(
            "{} violation(s). CONTRIBUTING.md has the rules.\n",
            report.violations.len()
        ));
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    // -- subject grammar ----------------------------------------------------

    #[test]
    fn empty_subject_is_invalid_with_a_specific_problem() {
        let result = parse("", false, None);
        assert!(!result.valid);
        assert_eq!(result.problem.as_deref(), Some("the subject is empty"));
    }

    #[test]
    fn whitespace_only_subject_is_invalid() {
        let result = parse("   ", false, None);
        assert!(!result.valid);
        assert_eq!(result.problem.as_deref(), Some("the subject is empty"));
    }

    #[test]
    fn well_formed_subject_parses() {
        let result = parse("feat(tooling): add drift check", false, None);
        assert!(result.valid);
        assert_eq!(result.kind.as_deref(), Some("feat"));
        assert_eq!(result.scope.as_deref(), Some("tooling"));
        assert!(!result.breaking);
        assert_eq!(result.summary.as_deref(), Some("add drift check"));
        assert_eq!(result.section, Some("Added"));
    }

    #[test]
    fn a_plain_type_and_summary_parses() {
        let result = parse("fix: bound the capture drains", false, None);
        assert!(result.valid, "expected valid, got: {:?}", result.problem);
        assert_eq!(result.kind.as_deref(), Some("fix"));
        assert_eq!(result.scope, None);
        assert_eq!(result.section, Some("Fixed"));
    }

    #[test]
    fn scope_breaking_marker_and_pull_request_number_all_parse() {
        let result = parse(
            "refactor(engine)!: one device generation contract (#391)",
            false,
            None,
        );
        assert!(result.valid, "expected valid, got: {:?}", result.problem);
        assert_eq!(result.scope.as_deref(), Some("engine"));
        assert!(result.breaking);
        assert_eq!(result.pull_request, Some(391));
        assert_eq!(
            result.summary.as_deref(),
            Some("one device generation contract")
        );
    }

    #[test]
    fn breaking_marker_forces_changed_section_regardless_of_type() {
        let result = parse("fix!: change the wire format", false, None);
        assert!(result.valid);
        assert!(result.breaking);
        assert_eq!(result.section, Some("Changed"));
    }

    #[test]
    fn a_breaking_fix_files_under_changed_not_fixed() {
        let result = parse("fix(config)!: reset the stored preset schema", false, None);
        assert_eq!(result.section, Some("Changed"));
    }

    #[test]
    fn a_no_entry_type_parses_but_produces_no_section() {
        let result = parse("ci: pin the runner image", false, None);
        assert!(result.valid);
        assert_eq!(result.section, None);
    }

    #[test]
    fn unknown_type_is_invalid_and_names_the_known_types() {
        let result = parse("oops: nothing", false, None);
        assert!(!result.valid);
        assert_eq!(result.kind.as_deref(), Some("oops"));
        assert!(
            result
                .problem
                .as_deref()
                .unwrap()
                .contains("is not a known type")
        );
    }

    #[test]
    fn an_unknown_type_is_rejected_and_names_the_known_ones() {
        let result = parse("engine: selectable WGC capture backend", false, None);
        assert!(!result.valid);
        assert!(
            result
                .problem
                .as_deref()
                .unwrap()
                .contains("not a known type")
        );
        assert!(result.problem.as_deref().unwrap().contains("refactor"));
    }

    #[test]
    fn a_subject_with_no_type_at_all_is_rejected() {
        let result = parse("Enforce release promotion integrity", false, None);
        assert!(!result.valid);
    }

    #[test]
    fn a_subject_that_is_only_a_type_and_a_pull_request_number_is_rejected() {
        let result = parse("fix:   (#12)", false, None);
        assert!(!result.valid);
        assert!(
            result
                .problem
                .as_deref()
                .unwrap()
                .contains("summary is empty")
        );

        let trailing = parse("fix(engine): ", false, None);
        assert!(!trailing.valid);
    }

    #[test]
    fn subject_missing_its_own_pull_request_number_is_invalid_when_required() {
        let result = parse("fix: patch it", true, None);
        assert!(!result.valid);
        assert!(
            result
                .problem
                .as_deref()
                .unwrap()
                .contains("pull request number")
        );
    }

    #[test]
    fn the_pull_request_number_is_required_only_when_asked_for() {
        let without = parse("fix: bound the capture drains", false, None);
        assert!(without.valid);
        let required = parse("fix: bound the capture drains", true, None);
        assert!(!required.valid);
    }

    #[test]
    fn title_ending_in_its_own_pull_request_number_is_rejected() {
        let result = parse("fix: patch it (#42)", false, Some(42));
        assert!(!result.valid);
        assert!(result.problem.as_deref().unwrap().contains("leave it off"));
    }

    #[test]
    fn title_citing_a_different_pull_request_number_is_left_alone() {
        let result = parse("fix: patch it, follows (#41)", false, Some(42));
        assert!(result.valid);
        assert_eq!(result.pull_request, Some(41));
    }

    #[test]
    fn a_title_carrying_its_own_number_is_rejected_another_number_is_not() {
        let own = parse("fix: bound the drains (#390)", false, Some(390));
        assert!(!own.valid);
        assert!(
            own.problem
                .as_deref()
                .unwrap()
                .contains("its own pull request number")
        );

        let other = parse("fix: bound the drains (#370)", false, Some(390));
        assert!(other.valid, "{:?}", other.problem);

        let none = parse("fix: bound the drains", false, Some(390));
        assert!(none.valid, "{:?}", none.problem);
    }

    #[test]
    fn repeated_own_number_is_stripped_from_the_summary_before_reuse() {
        let result = parse("fix: patch it (#42)", false, None);
        assert!(result.valid);
        assert_eq!(result.summary.as_deref(), Some("patch it"));
        assert_eq!(result.pull_request, Some(42));
    }

    #[test]
    fn a_title_that_already_carried_its_number_does_not_print_it_twice() {
        let parsed = parse(
            "fix: eighteen defects from a source audit (#385) (#385)",
            false,
            None,
        );
        assert!(parsed.valid);
        assert_eq!(parsed.pull_request, Some(385));
        assert_eq!(
            parsed.summary.as_deref(),
            Some("eighteen defects from a source audit")
        );
        let line = format_changelog_entry(&parsed, "https://example.invalid/r");
        assert!(!line.contains("(#385) ([#385]"));
    }

    #[test]
    fn a_summary_citing_a_different_pull_request_keeps_that_reference() {
        let parsed = parse("fix: finish what (#370) started (#391)", false, None);
        assert_eq!(parsed.pull_request, Some(391));
        assert_eq!(
            parsed.summary.as_deref(),
            Some("finish what (#370) started")
        );
    }

    #[test]
    fn merge_subject_appends_the_number_exactly_once_from_parsed_fields() {
        let commit = parse("feat(tooling)!: add drift check", false, None);
        assert_eq!(
            format_merge_subject(&commit, 42),
            "feat(tooling)!: add drift check (#42)"
        );
    }

    #[test]
    fn the_merge_subject_appends_the_number_exactly_once() {
        let plain = parse("fix(engine): bound the drains", false, None);
        assert_eq!(
            format_merge_subject(&plain, 390),
            "fix(engine): bound the drains (#390)"
        );

        let carried = parse("fix(engine): bound the drains (#390)", false, None);
        assert_eq!(
            format_merge_subject(&carried, 390),
            "fix(engine): bound the drains (#390)"
        );

        let breaking = parse(
            "refactor(engine)!: one device generation contract",
            false,
            None,
        );
        assert_eq!(
            format_merge_subject(&breaking, 391),
            "refactor(engine)!: one device generation contract (#391)"
        );

        let cited = parse("fix: finish what (#370) started", false, None);
        assert_eq!(
            format_merge_subject(&cited, 391),
            "fix: finish what (#370) started (#391)"
        );
    }

    #[test]
    fn changelog_entry_bolds_summary_and_links_the_pull_request() {
        let commit = parse("feat: add drift check (#42)", false, None);
        assert_eq!(
            format_changelog_entry(&commit, "https://github.com/Exoridus/exosnap"),
            "- **add drift check.** ([#42](https://github.com/Exoridus/exosnap/pull/42))"
        );
    }

    #[test]
    fn changelog_entry_prefixes_breaking_changes() {
        let commit = parse("fix!: change the wire format (#7)", false, None);
        assert_eq!(
            format_changelog_entry(&commit, "https://github.com/Exoridus/exosnap"),
            "- **BREAKING: change the wire format.** ([#7](https://github.com/Exoridus/exosnap/pull/7))"
        );
    }

    #[test]
    fn an_entry_links_its_pull_request_and_marks_breaking_changes() {
        let parsed = parse("feat(ui)!: one settings surface (#392)", false, None);
        let line = format_changelog_entry(&parsed, "https://example.invalid/r");
        assert!(line.starts_with("- **BREAKING: "));
        assert!(line.contains("([#392](https://example.invalid/r/pull/392))"));
    }

    #[test]
    fn no_entry_types_have_no_section() {
        for kind in ["ci", "build", "test", "chore", "style"] {
            let result = parse(&format!("{kind}: something"), false, None);
            assert!(result.valid);
            assert_eq!(result.section, None);
        }
    }

    // -- the check: fixture repos ------------------------------------------

    use crate::test_support::{fixture_repo_committed, write_files};
    use std::path::Path;
    use std::process::Command;
    use std::sync::atomic::{AtomicU64, Ordering};

    fn run_git(dir: &Path, args: &[&str]) -> (i32, String) {
        let output = Command::new("git")
            .args(args)
            .current_dir(dir)
            .output()
            .expect("run git for fixture repo");
        (
            output.status.code().unwrap_or(-1),
            String::from_utf8_lossy(&output.stdout).trim().to_string(),
        )
    }

    static COUNTER: AtomicU64 = AtomicU64::new(0);

    fn add_commit(dir: &Path, subject: &str) {
        let n = COUNTER.fetch_add(1, Ordering::SeqCst);
        write_files(dir, &[(&format!("f-{n}.txt"), "x\n")]);
        run_git(dir, &["add", "-A"]);
        run_git(dir, &["commit", "-q", "-m", subject]);
    }

    /// A repository with an initial commit on a branch named `main`, and
    /// (when `with_policy`) a commit that adds a stand-in
    /// `scripts/lib/CommitPolicy.psm1`, whose SHA becomes the fixture's
    /// policy epoch. `work` is checked out from `main` afterwards.
    struct Fixture {
        dir: tempfile::TempDir,
        epoch: Option<String>,
    }

    impl Fixture {
        fn options(&self) -> CommitPolicyOptions {
            CommitPolicyOptions {
                epoch: self.epoch.clone(),
            }
        }

        fn path(&self) -> &Path {
            self.dir.path()
        }
    }

    fn new_fixture(with_policy: bool) -> Fixture {
        let dir = fixture_repo_committed(&[
            ("README.md", "Fixture.\n"),
            ("CHANGELOG.md", "# Changelog\n\n## [Unreleased]\n"),
        ]);
        run_git(dir.path(), &["branch", "-M", "main"]);

        let epoch = if with_policy {
            write_files(dir.path(), &[("scripts/lib/CommitPolicy.psm1", "policy\n")]);
            run_git(dir.path(), &["add", "-A"]);
            run_git(
                dir.path(),
                &[
                    "commit",
                    "-q",
                    "-m",
                    "build(policy): adopt the commit subject grammar",
                ],
            );
            let (_, sha) = run_git(dir.path(), &["rev-parse", "HEAD"]);
            Some(sha)
        } else {
            None
        };

        run_git(dir.path(), &["checkout", "-q", "-b", "work"]);
        Fixture { dir, epoch }
    }

    fn base_request<'a>(subject: Option<&'a str>, only: Option<&'a str>) -> CheckRequest<'a> {
        CheckRequest {
            subject,
            pull_request_number: None,
            require_pull_request: false,
            base: None,
            only,
            changelog_cut: false,
        }
    }

    #[test]
    fn a_branch_of_well_formed_subjects_is_accepted() {
        let fx = new_fixture(true);
        add_commit(fx.path(), "fix(engine): bound the capture drains");
        add_commit(fx.path(), "test(engine): cover the drain timeout");
        let report = check(fx.path(), &base_request(None, None), &fx.options()).unwrap();
        assert!(report.ok(), "{:?}", report.violations);
        assert!(
            report
                .notes
                .iter()
                .any(|n| n.contains("2 commit(s) in scope"))
        );
    }

    #[test]
    fn a_malformed_subject_on_the_branch_is_rejected() {
        let fx = new_fixture(true);
        add_commit(fx.path(), "made the drains better");
        let report = check(fx.path(), &base_request(None, None), &fx.options()).unwrap();
        assert!(!report.ok());
        assert!(
            report
                .violations
                .iter()
                .any(|v| v.contains("commit-subject"))
        );
    }

    #[test]
    fn a_merge_commit_on_the_branch_is_counted_not_judged() {
        let fx = new_fixture(true);
        add_commit(fx.path(), "fix(engine): bound the capture drains");
        run_git(fx.path(), &["checkout", "-q", "main"]);
        add_commit(fx.path(), "fix(app): unrelated work on main");
        run_git(fx.path(), &["checkout", "-q", "work"]);
        run_git(
            fx.path(),
            &[
                "merge",
                "main",
                "--no-ff",
                "-m",
                "Merge branch 'main' into work",
                "--quiet",
            ],
        );
        let report = check(fx.path(), &base_request(None, None), &fx.options()).unwrap();
        assert!(report.ok(), "{:?}", report.violations);
        assert!(
            report
                .notes
                .iter()
                .any(|n| n.contains("1 merge commit(s) not judged"))
        );
    }

    #[test]
    fn a_malformed_subject_is_still_rejected_when_a_merge_commit_is_present() {
        let fx = new_fixture(true);
        add_commit(fx.path(), "made the drains better");
        run_git(fx.path(), &["checkout", "-q", "main"]);
        add_commit(fx.path(), "fix(app): unrelated work on main");
        run_git(fx.path(), &["checkout", "-q", "work"]);
        run_git(
            fx.path(),
            &[
                "merge",
                "main",
                "--no-ff",
                "-m",
                "Merge branch 'main' into work",
                "--quiet",
            ],
        );
        let report = check(fx.path(), &base_request(None, None), &fx.options()).unwrap();
        assert!(!report.ok());
    }

    #[test]
    fn history_before_the_policy_commit_is_grandfathered() {
        let dir = fixture_repo_committed(&[
            ("README.md", "Fixture.\n"),
            ("CHANGELOG.md", "# Changelog\n\n## [Unreleased]\n"),
        ]);
        run_git(dir.path(), &["branch", "-M", "main"]);
        let (_, initial) = run_git(dir.path(), &["rev-parse", "HEAD"]);
        add_commit(dir.path(), "Fix gate defects");
        add_commit(dir.path(), "made the drains better");
        write_files(dir.path(), &[("scripts/lib/CommitPolicy.psm1", "policy\n")]);
        run_git(dir.path(), &["add", "-A"]);
        run_git(
            dir.path(),
            &[
                "commit",
                "-q",
                "-m",
                "build(policy): adopt the commit subject grammar",
            ],
        );
        let (_, epoch) = run_git(dir.path(), &["rev-parse", "HEAD"]);
        run_git(dir.path(), &["checkout", "-q", "-b", "work"]);
        add_commit(dir.path(), "fix(engine): bound the capture drains");

        let options = CommitPolicyOptions { epoch: Some(epoch) };
        let mut request = base_request(None, None);
        request.base = Some(&initial);
        let report = check(dir.path(), &request, &options).unwrap();
        assert!(report.ok(), "{:?}", report.violations);
        assert!(
            report
                .notes
                .iter()
                .any(|n| n.contains("1 commit(s) in scope"))
        );
    }

    #[test]
    fn a_repository_without_the_policy_commit_has_nothing_in_scope() {
        let fx = new_fixture(false);
        add_commit(fx.path(), "made the drains better");
        let report = check(fx.path(), &base_request(None, None), &fx.options()).unwrap();
        assert!(report.ok(), "{:?}", report.violations);
        assert!(report.notes.iter().any(|n| n.contains("not committed yet")));
    }

    #[test]
    fn a_pull_request_title_is_accepted_without_a_number_and_rejected_with_its_own() {
        let fx = new_fixture(true);

        let mut accepted = base_request(Some("fix(engine): bound the drains"), None);
        accepted.pull_request_number = Some(390);
        let report = check(fx.path(), &accepted, &fx.options()).unwrap();
        assert!(report.ok(), "{:?}", report.violations);

        let mut rejected = base_request(Some("fix(engine): bound the drains (#390)"), None);
        rejected.pull_request_number = Some(390);
        let report = check(fx.path(), &rejected, &fx.options()).unwrap();
        assert!(!report.ok());
        assert!(
            report
                .violations
                .iter()
                .any(|v| v.contains("its own pull request number"))
        );

        let mut cited = base_request(Some("fix(engine): finish what (#370) started"), None);
        cited.pull_request_number = Some(390);
        let report = check(fx.path(), &cited, &fx.options()).unwrap();
        assert!(report.ok(), "{:?}", report.violations);
    }

    #[test]
    fn a_merged_subject_still_has_to_carry_exactly_one_number() {
        let fx = new_fixture(true);

        let mut accepted = base_request(Some("fix(engine): bound the drains (#390)"), None);
        accepted.require_pull_request = true;
        let report = check(fx.path(), &accepted, &fx.options()).unwrap();
        assert!(report.ok(), "{:?}", report.violations);

        let mut rejected = base_request(Some("fix(engine): bound the drains"), None);
        rejected.require_pull_request = true;
        let report = check(fx.path(), &rejected, &fx.options()).unwrap();
        assert!(!report.ok());
    }

    #[test]
    fn a_branch_that_writes_the_changelog_is_rejected() {
        let fx = new_fixture(true);
        write_files(
            fx.path(),
            &[(
                "CHANGELOG.md",
                "# Changelog\n\n## [Unreleased]\n\n- hand written\n",
            )],
        );
        run_git(fx.path(), &["add", "-A"]);
        run_git(
            fx.path(),
            &["commit", "-q", "-m", "docs: add a changelog line"],
        );
        let report = check(fx.path(), &base_request(None, None), &fx.options()).unwrap();
        assert!(!report.ok());
        assert!(
            report
                .violations
                .iter()
                .any(|v| v.contains("changelog-untouched"))
        );
    }

    #[test]
    fn a_declared_release_cut_may_write_the_changelog() {
        let fx = new_fixture(true);
        write_files(
            fx.path(),
            &[("CHANGELOG.md", "# Changelog\n\n## [Unreleased]\n\n- cut\n")],
        );
        run_git(fx.path(), &["add", "-A"]);
        run_git(
            fx.path(),
            &[
                "commit",
                "-q",
                "-m",
                "chore(release): assemble the changelog",
            ],
        );
        let mut request = base_request(None, None);
        request.changelog_cut = true;
        let report = check(fx.path(), &request, &fx.options()).unwrap();
        assert!(report.ok(), "{:?}", report.violations);
    }

    #[test]
    fn the_commit_that_introduces_the_policy_may_create_the_changelog() {
        let fx = new_fixture(false);
        write_files(
            fx.path(),
            &[("CHANGELOG.md", "# Changelog\n\n## [Unreleased]\n")],
        );
        run_git(fx.path(), &["add", "-A"]);
        run_git(
            fx.path(),
            &["commit", "-q", "-m", "build(policy): open a changelog"],
        );
        let report = check(fx.path(), &base_request(None, None), &fx.options()).unwrap();
        assert!(report.ok(), "{:?}", report.violations);
    }

    #[test]
    fn an_uncommitted_changelog_edit_is_rejected_too() {
        let fx = new_fixture(true);
        write_files(
            fx.path(),
            &[(
                "CHANGELOG.md",
                "# Changelog\n\n## [Unreleased]\n\n- uncommitted\n",
            )],
        );
        let report = check(fx.path(), &base_request(None, None), &fx.options()).unwrap();
        assert!(!report.ok());
    }

    #[test]
    fn only_restricts_the_run_to_the_named_rule() {
        let fx = new_fixture(true);
        write_files(
            fx.path(),
            &[(
                "CHANGELOG.md",
                "# Changelog\n\n## [Unreleased]\n\n- hand written\n",
            )],
        );
        run_git(fx.path(), &["add", "-A"]);
        run_git(fx.path(), &["commit", "-q", "-m", "made the drains better"]);
        let report = check(
            fx.path(),
            &base_request(None, Some("commit-subject")),
            &fx.options(),
        )
        .unwrap();
        assert!(!report.ok());
        assert!(
            report
                .violations
                .iter()
                .all(|v| v.contains("commit-subject"))
        );
    }

    #[test]
    fn a_directory_that_is_not_a_git_repository_is_a_hard_error() {
        let dir = tempfile::tempdir().unwrap();
        let result = check(
            dir.path(),
            &base_request(None, None),
            &CommitPolicyOptions::default(),
        );
        assert!(result.is_err());
    }
}
