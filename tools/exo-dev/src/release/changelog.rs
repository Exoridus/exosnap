//! Assembles the changelog section for everything merged since the last
//! release, and renders a release's notes from a template plus that section.
//!
//! The changelog is written here and nowhere else: a pull request that edits
//! CHANGELOG.md by hand conflicts with every other pull request that also
//! did, which is why `commit_policy`'s `changelog-untouched` rule fails one
//! that tries.
//!
//! What `assemble` reads is the first-parent history since the last
//! version tag: on a squash-merging repository that is exactly one commit
//! per merged pull request, with the pull request title as the subject and
//! its number appended. The Conventional Commits type files the entry, `!`
//! marks it breaking, and a type with no changelog section produces no
//! entry at all.
//!
//! A subject the parser cannot read is reported, never skipped. A changelog
//! assembled from a history it silently dropped half of is worse than no
//! changelog at all: a reader cannot tell an empty section from an unparsed
//! one. Anything merged before the policy took effect is counted separately
//! as grandfathered, for the same reason.
//!
//! The release notes template is the text; `render_notes` fills in the
//! identity and reads the changelog section out of CHANGELOG.md rather than
//! regenerating it, so the releases page and the file cannot disagree about
//! what shipped. An unresolved placeholder is an error: a release published
//! with a literal `${VERSION}` in it cannot be edited back into history
//! readers already saw.

use std::collections::HashSet;
use std::path::{Path, PathBuf};
use std::sync::LazyLock;

use anyhow::bail;
use regex::Regex;

use crate::commit_policy::{self, CommitPolicyOptions};
use crate::git::Git;

/// Default base URL for the release-notes compare/releases links and the
/// changelog's pull-request links.
pub const DEFAULT_REPOSITORY_URL: &str = "https://github.com/Exoridus/exosnap";

/// One assembled changelog section, before a heading is chosen for it.
pub struct ChangelogAssembly {
    /// The git range assembled, as printed to the operator (`a..b` or `b`).
    pub range: String,
    /// Entries per section, in `commit_policy::SECTION_ORDER`.
    pub sections: Vec<(&'static str, Vec<String>)>,
    pub entry_count: usize,
    /// Subjects of a type that intentionally produces no changelog entry.
    pub skipped: usize,
    /// Explicit merge commits on the first-parent line. They carry no entry:
    /// either their changes already shipped (a back-merge of a released
    /// branch) or they belong to commits the first-parent walk deliberately
    /// does not read.
    pub merges: usize,
    /// Subjects grandfathered because they predate the commit-policy epoch.
    pub older: usize,
    /// Subjects the parser could not read at all, as `hash  subject  -- problem`.
    pub unreadable: Vec<String>,
}

/// Reads the range and assembles it into changelog sections. `since` and
/// `until` override the range endpoints the same way `-Since`/`-Until` did;
/// when `since` is not given, the baseline is resolved here, from the last
/// released version tag, rather than trusted from a caller that might hand
/// back a release-candidate tag by mistake.
pub fn assemble(
    repo_root: &Path,
    since: Option<&str>,
    until: Option<&str>,
    options: &CommitPolicyOptions,
) -> anyhow::Result<ChangelogAssembly> {
    let git = Git::new(repo_root);
    anyhow::ensure!(
        git.head().is_some(),
        "changelog: '{}' is not a git repository",
        repo_root.display()
    );

    let since_ref = match since {
        Some(value) => Some(value.to_string()),
        None => resolve_since(&git),
    };
    let until_ref = match until {
        Some(value) => value.to_string(),
        None => resolve_until(&git),
    };
    let range = match &since_ref {
        Some(since_ref) => format!("{since_ref}..{until_ref}"),
        None => until_ref,
    };

    let epoch = commit_policy::resolve_epoch(&git, options);
    let grandfathered: HashSet<String> = match &epoch {
        Some(epoch) => {
            let (_, out) = git.run(&["rev-list", &format!("{epoch}^")]);
            out.lines().map(str::trim).map(str::to_string).collect()
        }
        None => HashSet::new(),
    };

    // --first-parent: on a squash-merging repository the first-parent line is
    // one commit per merged pull request. Without it a branch that was
    // merged rather than squashed would contribute every commit it ever had.
    let unit = '\u{1f}';
    let format_arg = format!("--format=%H{unit}%s");
    let (_, subjects) = git.run(&["log", "--first-parent", &format_arg, &range]);

    let mut sections: Vec<(&'static str, Vec<String>)> = commit_policy::SECTION_ORDER
        .iter()
        .map(|name| (*name, Vec::new()))
        .collect();
    let mut unreadable = Vec::new();
    let mut skipped = 0usize;
    let mut merges = 0usize;
    let mut older = 0usize;

    for line in subjects.lines() {
        if line.is_empty() {
            continue;
        }
        let mut parts = line.splitn(2, unit);
        let hash = parts.next().unwrap_or_default();
        let subject = parts.next().unwrap_or_default();

        let parsed = commit_policy::parse(subject, false, None);
        if !parsed.valid {
            if grandfathered.contains(hash) {
                older += 1;
                continue;
            }
            if is_merge_subject(subject) {
                merges += 1;
                continue;
            }
            let short = &hash[..hash.len().min(8)];
            unreadable.push(format!(
                "{short}  {subject}  -- {}",
                parsed.problem.as_deref().unwrap_or("invalid")
            ));
            continue;
        }
        let Some(section) = parsed.section else {
            skipped += 1;
            continue;
        };
        let entry = commit_policy::format_changelog_entry(&parsed, DEFAULT_REPOSITORY_URL);
        if let Some((_, entries)) = sections.iter_mut().find(|(name, _)| *name == section) {
            entries.push(entry);
        }
    }

    let entry_count = sections.iter().map(|(_, entries)| entries.len()).sum();

    Ok(ChangelogAssembly {
        range,
        sections,
        entry_count,
        skipped,
        merges,
        older,
        unreadable,
    })
}

/// True for the subjects git itself writes for an explicit merge. On a
/// squash-merging repository the only such commit is a deliberate merge such
/// as a back-merge of a released branch, and its content is either already in
/// an earlier release or lives on a second-parent line the changelog does not
/// walk, so it must not be reported as an unreadable subject.
fn is_merge_subject(subject: &str) -> bool {
    subject.starts_with("Merge pull request #")
        || subject.starts_with("Merge branch ")
        || subject.starts_with("Merge remote-tracking branch ")
}

fn resolve_since(git: &Git) -> Option<String> {
    // Released versions only. A release candidate is a prerelease of the
    // very version being assembled, and git's version sort ranks it ABOVE
    // the release it precedes, so an unfiltered list would hand back the
    // newest candidate and silently drop the release it is the changelog of.
    let (_, out) = git.run(&[
        "tag",
        "--list",
        "v*",
        "--merged",
        "HEAD",
        "--sort=-version:refname",
    ]);
    out.lines()
        .map(str::trim)
        .find(|tag| !tag.is_empty() && !tag.contains('-'))
        .map(str::to_string)
}

fn resolve_until(git: &Git) -> String {
    let (code, _) = git.run(&["rev-parse", "--verify", "--quiet", "origin/main"]);
    if code == 0 {
        "origin/main".to_string()
    } else {
        "HEAD".to_string()
    }
}

/// Renders the heading and body for one assembled section: `## [Unreleased]`
/// without `version`, or `## [x.y.z] - date` with it. A section with no
/// entries is omitted entirely.
pub fn render(
    assembly: &ChangelogAssembly,
    version: Option<&str>,
    date: Option<&str>,
) -> Vec<String> {
    let heading = match version {
        Some(version) => {
            let when = date.map(str::to_string).unwrap_or_else(today_utc);
            format!("## [{version}] - {when}")
        }
        None => "## [Unreleased]".to_string(),
    };

    let mut rendered = vec![heading];
    for (name, entries) in &assembly.sections {
        if entries.is_empty() {
            continue;
        }
        rendered.push(String::new());
        rendered.push(format!("### {name}"));
        rendered.push(String::new());
        rendered.extend(entries.iter().cloned());
    }
    rendered
}

/// The operator-facing summary: range, entry counts, and every unreadable
/// subject with the fix instruction. Printed before the rendered section
/// whether or not `-Apply` was given.
pub fn render_report(assembly: &ChangelogAssembly) -> String {
    let mut out = String::new();
    out.push_str(&format!("range     {}\n", assembly.range));
    let section_count = assembly
        .sections
        .iter()
        .filter(|(_, entries)| !entries.is_empty())
        .count();
    out.push_str(&format!(
        "entries   {} in {section_count} section(s)\n",
        assembly.entry_count
    ));
    out.push_str(&format!(
        "no entry  {} (ci/build/test/chore/style)\n",
        assembly.skipped
    ));
    if assembly.merges > 0 {
        out.push_str(&format!(
            "merges    {} explicit merge commit(s)\n",
            assembly.merges
        ));
    }
    if assembly.older > 0 {
        out.push_str(&format!(
            "older     {} merged before the policy took effect\n",
            assembly.older
        ));
    }
    if !assembly.unreadable.is_empty() {
        out.push('\n');
        out.push_str(&format!(
            "{} subject(s) the cut cannot file:\n",
            assembly.unreadable.len()
        ));
        for item in &assembly.unreadable {
            out.push_str(&format!("  {item}\n"));
        }
        out.push('\n');
        out.push_str(
            "Fix the subject on the merged commit, or file the entry by hand and say why in the pull request.\n",
        );
    }
    out
}

/// Writes `rendered` into CHANGELOG.md's `## [Unreleased]` section.
/// `open_new_unreleased` (true for a release cut) closes Unreleased with the
/// rendered release section and opens a fresh, empty Unreleased above it, so
/// the next merge has somewhere to go without anyone editing the file's
/// shape.
pub fn apply(
    repo_root: &Path,
    rendered: &[String],
    open_new_unreleased: bool,
) -> anyhow::Result<PathBuf> {
    let changelog_path = repo_root.join("CHANGELOG.md");
    anyhow::ensure!(
        changelog_path.is_file(),
        "{} does not exist.",
        changelog_path.display()
    );

    let existing = std::fs::read_to_string(&changelog_path)?;
    let lines: Vec<&str> = existing.lines().collect();
    let Some(start) = lines
        .iter()
        .position(|line| line.starts_with("## [Unreleased]"))
    else {
        bail!(
            "{} has no '## [Unreleased]' section to write into.",
            changelog_path.display()
        );
    };
    let end = lines[start + 1..]
        .iter()
        .position(|line| line.starts_with("## ["))
        .map(|offset| start + 1 + offset)
        .unwrap_or(lines.len());

    let mut body: Vec<String> = Vec::new();
    if open_new_unreleased {
        body.push("## [Unreleased]".to_string());
        body.push(String::new());
    }
    body.extend(rendered.iter().cloned());
    body.push(String::new());

    let mut updated: Vec<String> = Vec::new();
    updated.extend(lines[..start].iter().map(|line| line.to_string()));
    updated.extend(body);
    updated.extend(lines[end..].iter().map(|line| line.to_string()));

    let mut text = updated.join("\n");
    while text.ends_with('\n') {
        text.pop();
    }
    text.push('\n');
    // LF: .gitattributes pins this tree to eol=lf.
    std::fs::write(&changelog_path, text)?;
    Ok(changelog_path)
}

/// Today's date in UTC, `yyyy-mm-dd`. Computed from the Unix epoch directly
/// rather than through a calendar dependency, since this is the one place in
/// the crate that needs a calendar date at all.
fn today_utc() -> String {
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default();
    let days = (now.as_secs() / 86_400) as i64;
    let (year, month, day) = civil_from_days(days);
    format!("{year:04}-{month:02}-{day:02}")
}

/// Howard Hinnant's `civil_from_days`: days since the Unix epoch to a
/// proleptic Gregorian (year, month, day), valid across the full `i64` range
/// without leap-year special cases.
fn civil_from_days(days_since_epoch: i64) -> (i64, u32, u32) {
    let z = days_since_epoch + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let day_of_era = (z - era * 146_097) as u64;
    let year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36_524 - day_of_era / 146_096) / 365;
    let year = year_of_era as i64 + era * 400;
    let day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    let month_index = (5 * day_of_year + 2) / 153;
    let day = (day_of_year - (153 * month_index + 2) / 5 + 1) as u32;
    let month = if month_index < 10 {
        month_index + 3
    } else {
        month_index - 9
    } as u32;
    let year = if month <= 2 { year + 1 } else { year };
    (year, month, day)
}

// ---------------------------------------------------------------------------
// Release notes
// ---------------------------------------------------------------------------

static PLACEHOLDER_PATTERN: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"\$\{[A-Z_]+\}").unwrap());

/// The identity a release's notes are rendered for.
pub struct ReleaseNotesRequest<'a> {
    /// The full release identity, as in `0.9.1` or `0.9.1-rc3`. The
    /// changelog section is looked up under its `x.y.z` part.
    pub version: &'a str,
    /// The git tag. Defaults to `v` + `version`.
    pub tag: Option<&'a str>,
    /// The tag the compare link starts at. Defaults to the newest version
    /// tag below this one that is an ancestor of HEAD.
    pub previous_tag: Option<&'a str>,
    /// The commit the artifacts were built from. Defaults to HEAD.
    pub commit: Option<&'a str>,
    /// Render the release-candidate template and read Unreleased instead of
    /// this version's own section.
    pub candidate: bool,
    /// Base URL for the compare and releases links.
    pub repository_url: &'a str,
}

#[derive(Debug)]
pub struct RenderedNotes {
    pub text: String,
    pub previous_tag: Option<String>,
    /// The compare link degrades to a link at the repository, which is
    /// wrong but harmless: a first release genuinely has nothing to compare
    /// against. The caller decides whether and how to surface this.
    pub warn_no_previous_tag: bool,
}

/// Renders one release's notes from its template and the changelog.
pub fn render_notes(
    repo_root: &Path,
    request: &ReleaseNotesRequest,
) -> anyhow::Result<RenderedNotes> {
    let git = Git::new(repo_root);

    let tag = request
        .tag
        .map(str::to_string)
        .unwrap_or_else(|| format!("v{}", request.version));
    let commit = match request.commit {
        Some(commit) => commit.to_string(),
        None => git
            .head()
            .unwrap_or_else(|| "an unknown commit".to_string()),
    };

    let previous_tag = match request.previous_tag {
        Some(previous_tag) if !previous_tag.is_empty() => Some(previous_tag.to_string()),
        Some(_) => None,
        None => resolve_previous_tag(&git, &tag),
    };

    let product_version = request.version.split('-').next().unwrap_or(request.version);

    let section = get_changelog_section(repo_root, product_version, request.candidate)?
        .unwrap_or_else(|| "_No changelog entries were recorded for this release._".to_string());

    let template_name = if request.candidate {
        "release-notes-candidate.md"
    } else {
        "release-notes.md"
    };
    let template_path = repo_root.join(".github/templates").join(template_name);
    anyhow::ensure!(
        template_path.is_file(),
        "Release notes template {} is missing.",
        template_path.display()
    );
    let mut notes = std::fs::read_to_string(&template_path)?;

    let previous_tag_value = previous_tag.clone().unwrap_or_default();
    let values: [(&str, &str); 6] = [
        ("VERSION", request.version),
        ("TAG", &tag),
        ("PREVIOUS_TAG", &previous_tag_value),
        ("COMMIT", &commit),
        ("REPO_URL", request.repository_url),
        ("CHANGELOG_SECTION", &section),
    ];
    for (key, value) in values {
        notes = notes.replace(&format!("${{{key}}}"), value);
    }

    let unresolved = unresolved_placeholders(&notes);
    anyhow::ensure!(
        unresolved.is_empty(),
        "Release notes still contain unresolved placeholders: {}.",
        unresolved.join(", ")
    );

    Ok(RenderedNotes {
        text: notes,
        warn_no_previous_tag: previous_tag.is_none(),
        previous_tag,
    })
}

fn resolve_previous_tag(git: &Git, tag: &str) -> Option<String> {
    // The tag being released is excluded by name: it exists by the time this
    // runs, and a compare link from a tag to itself is empty.
    //
    // A final release compares against the previous FINAL release. Git's
    // version sort ranks a release candidate above the release it precedes,
    // so an unfiltered list would point a release's compare link at its own
    // candidate. A candidate's own notes keep the nearest tag, which is the
    // candidate before it, because that is the window a candidate is read
    // against.
    let compares_to_prerelease = tag.contains('-');
    let (_, out) = git.run(&[
        "tag",
        "--list",
        "v*",
        "--merged",
        "HEAD",
        "--sort=-version:refname",
    ]);
    out.lines()
        .map(str::trim)
        .find(|candidate| {
            !candidate.is_empty()
                && *candidate != tag
                && (compares_to_prerelease || !candidate.contains('-'))
        })
        .map(str::to_string)
}

/// The body of one version's changelog section, without its heading. A
/// candidate reads Unreleased, because its changes are by definition not cut
/// yet; a final release reads its own section, and falls back to Unreleased
/// for the window between the cut's assembly and its heading being stamped.
fn get_changelog_section(
    repo_root: &Path,
    product_version: &str,
    prefer_unreleased: bool,
) -> anyhow::Result<Option<String>> {
    let path = repo_root.join("CHANGELOG.md");
    if !path.is_file() {
        return Ok(None);
    }
    let text = std::fs::read_to_string(&path)?;
    let lines: Vec<&str> = text.lines().collect();

    let wanted: Vec<String> = if prefer_unreleased {
        vec!["## [Unreleased]".to_string()]
    } else {
        vec![
            format!("## [{product_version}]"),
            "## [Unreleased]".to_string(),
        ]
    };

    for heading in wanted {
        let Some(start) = lines
            .iter()
            .position(|line| line.starts_with(heading.as_str()))
        else {
            continue;
        };
        let end = lines[start + 1..]
            .iter()
            .position(|line| line.starts_with("## ["))
            .map(|offset| start + 1 + offset)
            .unwrap_or(lines.len());
        let body = lines[start + 1..end].join("\n");
        let trimmed = body.trim();
        if !trimmed.is_empty() {
            return Ok(Some(trimmed.to_string()));
        }
    }
    Ok(None)
}

fn unresolved_placeholders(text: &str) -> Vec<String> {
    let mut found: Vec<String> = PLACEHOLDER_PATTERN
        .find_iter(text)
        .map(|m| m.as_str().to_string())
        .collect();
    found.sort();
    found.dedup();
    found
}

#[cfg(test)]
mod tests {
    use super::*;
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

    /// A committed repository with `scripts/lib/CommitPolicy.psm1` staged in
    /// as the fixture's policy epoch, so `commit_policy::resolve_epoch` finds
    /// scope without touching the real, pinned `POLICY_EPOCH`.
    struct Fixture {
        dir: tempfile::TempDir,
        epoch: String,
    }

    impl Fixture {
        fn path(&self) -> &Path {
            self.dir.path()
        }

        fn options(&self) -> CommitPolicyOptions {
            CommitPolicyOptions {
                epoch: Some(self.epoch.clone()),
            }
        }
    }

    fn new_fixture() -> Fixture {
        let dir = fixture_repo_committed(&[
            ("README.md", "Fixture.\n"),
            ("CHANGELOG.md", "# Changelog\n\n## [Unreleased]\n"),
        ]);
        run_git(dir.path(), &["branch", "-M", "main"]);
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
        Fixture { dir, epoch }
    }

    fn assemble_all(fx: &Fixture) -> ChangelogAssembly {
        assemble(fx.path(), None, Some("HEAD"), &fx.options()).unwrap()
    }

    fn rendered_text(
        assembly: &ChangelogAssembly,
        version: Option<&str>,
        date: Option<&str>,
    ) -> String {
        render(assembly, version, date).join("\n")
    }

    #[test]
    fn entries_are_grouped_and_no_entry_types_produce_nothing() {
        let fx = new_fixture();
        add_commit(fx.path(), "feat(ui): a recording presets row (#10)");
        add_commit(fx.path(), "fix(engine): bound the capture drains (#11)");
        add_commit(fx.path(), "ci: pin the runner image (#12)");
        let assembly = assemble_all(&fx);
        let text = rendered_text(&assembly, None, None);

        assert!(text.contains("### Added"), "{text}");
        assert!(text.contains("### Fixed"), "{text}");
        assert!(!text.contains("pin the runner image"), "{text}");
        // Two: the ci commit above, and the fixture's own build(policy) commit.
        assert_eq!(assembly.skipped, 2);
        assert!(assembly.unreadable.is_empty());
    }

    #[test]
    fn an_unreadable_subject_is_reported_not_silently_dropped() {
        let fx = new_fixture();
        add_commit(fx.path(), "made the drains better");
        let assembly = assemble_all(&fx);
        assert_eq!(assembly.unreadable.len(), 1);
        assert!(assembly.unreadable[0].contains("made the drains better"));
    }

    #[test]
    fn an_explicit_merge_commit_is_counted_but_carries_no_entry() {
        let fx = new_fixture();
        add_commit(fx.path(), "feat(ui): a real entry (#10)");
        run_git(fx.path(), &["checkout", "-q", "-b", "back-merge"]);
        add_commit(
            fx.path(),
            "fix(engine): arrives through the merged branch (#11)",
        );
        run_git(fx.path(), &["checkout", "-q", "main"]);
        add_commit(fx.path(), "fix(ui): after the merge (#12)");
        run_git(
            fx.path(),
            &[
                "merge",
                "--no-ff",
                "-q",
                "-m",
                "Merge pull request #449 from Exoridus/back-merge",
                "back-merge",
            ],
        );

        let assembly = assemble_all(&fx);
        assert!(assembly.unreadable.is_empty(), "{:?}", assembly.unreadable);
        assert_eq!(assembly.merges, 1);
        let text = rendered_text(&assembly, None, None);
        assert!(!text.contains("back-merge"), "{text}");
        assert!(text.contains("after the merge"), "{text}");
    }

    #[test]
    fn pre_policy_subjects_are_counted_as_older_not_as_unreadable() {
        let dir = fixture_repo_committed(&[
            ("README.md", "Fixture.\n"),
            ("CHANGELOG.md", "# Changelog\n\n## [Unreleased]\n"),
        ]);
        run_git(dir.path(), &["branch", "-M", "main"]);
        add_commit(dir.path(), "Fix gate defects");
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
        add_commit(dir.path(), "fix(engine): bound the capture drains (#11)");

        let options = CommitPolicyOptions { epoch: Some(epoch) };
        let assembly = assemble(dir.path(), None, Some("HEAD"), &options).unwrap();
        // Two: the fixture's initial import and the pre-policy subject.
        assert_eq!(assembly.older, 2);
        assert!(assembly.unreadable.is_empty());
    }

    #[test]
    fn the_range_starts_at_the_last_version_tag() {
        let fx = new_fixture();
        add_commit(fx.path(), "feat(ui): before the tag (#1)");
        run_git(fx.path(), &["tag", "v0.9.0"]);
        add_commit(fx.path(), "feat(ui): after the tag (#2)");
        let assembly = assemble(fx.path(), None, Some("HEAD"), &fx.options()).unwrap();
        let text = rendered_text(&assembly, None, None);
        assert!(text.contains("after the tag"), "{text}");
        assert!(!text.contains("before the tag"), "{text}");
    }

    #[test]
    fn a_release_candidate_is_not_a_baseline_the_range_may_start_at() {
        // git's version sort ranks v0.9.1-rc5 ABOVE v0.9.0, so an unfiltered
        // tag list hands back the newest release candidate, and the release
        // it is a candidate FOR would then be described by whatever was
        // merged after it.
        let fx = new_fixture();
        add_commit(fx.path(), "feat(ui): in the release line (#1)");
        run_git(fx.path(), &["tag", "v0.9.0"]);
        add_commit(fx.path(), "fix(engine): part of the next release (#2)");
        run_git(fx.path(), &["tag", "v0.9.1-rc1"]);
        add_commit(fx.path(), "fix(ui): merged after the candidate (#3)");

        let assembly = assemble(fx.path(), None, Some("HEAD"), &fx.options()).unwrap();
        let text = rendered_text(&assembly, Some("0.9.1"), None);
        assert!(
            text.contains("part of the next release"),
            "the work the candidate carried was dropped from its own release: {text}"
        );
        assert!(text.contains("merged after the candidate"), "{text}");
        assert!(
            !text.contains("in the release line"),
            "a commit released in v0.9.0 was listed again: {text}"
        );
    }

    #[test]
    fn apply_writes_into_unreleased_and_version_opens_a_new_one() {
        let fx = new_fixture();
        add_commit(fx.path(), "fix(engine): bound the capture drains (#11)");
        let assembly = assemble(fx.path(), None, Some("HEAD"), &fx.options()).unwrap();
        let rendered = render(&assembly, None, None);
        apply(fx.path(), &rendered, false).unwrap();
        let text = std::fs::read_to_string(fx.path().join("CHANGELOG.md")).unwrap();
        assert!(text.contains("## [Unreleased]"));
        assert!(text.contains("bound the capture drains"), "{text}");

        let cut_assembly = assemble(fx.path(), None, Some("HEAD"), &fx.options()).unwrap();
        let cut_rendered = render(&cut_assembly, Some("0.9.1"), Some("2026-09-13"));
        apply(fx.path(), &cut_rendered, true).unwrap();
        let text = std::fs::read_to_string(fx.path().join("CHANGELOG.md")).unwrap();
        assert!(text.contains("## [0.9.1] - 2026-09-13"), "{text}");
        assert!(
            text.find("## [Unreleased]").unwrap() < text.find("## [0.9.1]").unwrap(),
            "the new Unreleased section is not above the release: {text}"
        );
    }

    fn install_template(dir: &Path, name: &str) {
        let source = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../.github/templates")
            .join(name);
        let dest_dir = dir.join(".github/templates");
        std::fs::create_dir_all(&dest_dir).unwrap();
        std::fs::copy(source, dest_dir.join(name)).unwrap();
    }

    #[test]
    fn notes_carry_the_changelog_section_and_resolve_every_placeholder() {
        let fx = new_fixture();
        install_template(fx.path(), "release-notes.md");
        write_files(
            fx.path(),
            &[(
                "CHANGELOG.md",
                "# Changelog\n\n## [0.9.1] - 2026-09-13\n\n### Fixed\n\n- **Bound the capture drains.** ([#11](https://example.invalid/r/pull/11))\n",
            )],
        );
        run_git(fx.path(), &["add", "-A"]);
        run_git(
            fx.path(),
            &[
                "commit",
                "-q",
                "-m",
                "chore(release): stage the notes fixture",
            ],
        );
        run_git(fx.path(), &["tag", "v0.9.0"]);

        let request = ReleaseNotesRequest {
            version: "0.9.1",
            tag: None,
            previous_tag: None,
            commit: None,
            candidate: false,
            repository_url: DEFAULT_REPOSITORY_URL,
        };
        let rendered = render_notes(fx.path(), &request).unwrap();
        assert!(
            rendered.text.contains("Bound the capture drains"),
            "{}",
            rendered.text
        );
        assert!(!rendered.text.contains("${"), "{}", rendered.text);
        assert!(
            rendered.text.contains("v0.9.0...v0.9.1"),
            "{}",
            rendered.text
        );
    }

    #[test]
    fn the_compare_link_of_a_release_skips_the_candidates_of_that_release() {
        // Git's version sort ranks v0.9.1-rc1 above v0.9.0, so the nearest
        // tag by that order is the release's own candidate, and a
        // full-changelog link to it would show what was merged after the
        // candidate instead of what the release contains.
        let fx = new_fixture();
        install_template(fx.path(), "release-notes.md");
        write_files(
            fx.path(),
            &[(
                "CHANGELOG.md",
                "# Changelog\n\n## [0.9.1] - 2026-09-13\n\n### Fixed\n\n- **Bound the capture drains.** ([#11](https://example.invalid/r/pull/11))\n",
            )],
        );
        run_git(fx.path(), &["add", "-A"]);
        run_git(
            fx.path(),
            &[
                "commit",
                "-q",
                "-m",
                "chore(release): stage the notes fixture",
            ],
        );
        run_git(fx.path(), &["tag", "v0.9.0"]);
        add_commit(fx.path(), "fix(engine): bound the capture drains (#11)");
        run_git(fx.path(), &["tag", "v0.9.1-rc1"]);

        let request = ReleaseNotesRequest {
            version: "0.9.1",
            tag: None,
            previous_tag: None,
            commit: None,
            candidate: false,
            repository_url: DEFAULT_REPOSITORY_URL,
        };
        let rendered = render_notes(fx.path(), &request).unwrap();
        assert!(
            rendered.text.contains("v0.9.0...v0.9.1"),
            "{}",
            rendered.text
        );
        assert!(!rendered.text.contains("rc1..."), "{}", rendered.text);
    }

    #[test]
    fn a_candidate_still_compares_against_the_candidate_before_it() {
        let fx = new_fixture();
        install_template(fx.path(), "release-notes-candidate.md");
        add_commit(fx.path(), "chore(release): stage the notes fixture");
        run_git(fx.path(), &["tag", "v0.9.0"]);
        add_commit(fx.path(), "fix(engine): bound the capture drains (#11)");
        run_git(fx.path(), &["tag", "v0.9.1-rc1"]);

        let request = ReleaseNotesRequest {
            version: "0.9.1-rc2",
            tag: None,
            previous_tag: None,
            commit: None,
            candidate: true,
            repository_url: DEFAULT_REPOSITORY_URL,
        };
        let rendered = render_notes(fx.path(), &request).unwrap();
        assert!(
            rendered.text.contains("v0.9.1-rc1...v0.9.1-rc2"),
            "a candidate lost the window it is read against: {}",
            rendered.text
        );
    }

    #[test]
    fn a_template_with_an_unknown_placeholder_fails_instead_of_publishing_it() {
        let fx = new_fixture();
        write_files(
            fx.path(),
            &[(
                ".github/templates/release-notes.md",
                "# ExoSnap ${VERSION}\n\n${UNKNOWN_FIELD}\n",
            )],
        );
        let request = ReleaseNotesRequest {
            version: "0.9.1",
            tag: None,
            previous_tag: None,
            commit: None,
            candidate: false,
            repository_url: DEFAULT_REPOSITORY_URL,
        };
        let error = render_notes(fx.path(), &request).unwrap_err();
        assert!(error.to_string().contains("UNKNOWN_FIELD"), "{error}");
    }

    #[test]
    fn a_candidate_reads_unreleased_and_says_so_when_there_is_nothing() {
        let fx = new_fixture();
        install_template(fx.path(), "release-notes-candidate.md");
        let request = ReleaseNotesRequest {
            version: "0.9.1-rc3",
            tag: None,
            previous_tag: None,
            commit: None,
            candidate: true,
            repository_url: DEFAULT_REPOSITORY_URL,
        };
        let rendered = render_notes(fx.path(), &request).unwrap();
        assert!(
            rendered.text.contains("No changelog entries"),
            "{}",
            rendered.text
        );
        assert!(
            rendered.text.contains("Do not announce"),
            "{}",
            rendered.text
        );
    }
}
