//! What each package channel is serving or being prepared to serve, against
//! what the publication policy states. Advisory: it reports, it never
//! publishes.
//!
//! Four channels carry ExoSnap to users: the GitHub Release every other one
//! downloads from, Chocolatey, WinGet and Scoop. The policy states one of
//! PENDING, PREPARED, SUBMITTED, IN_REVIEW or PUBLISHED per channel, so a
//! channel that is behind the release is distinguishable from a submission
//! that was forgotten.
//!
//! `packaging/publication-policy.json` states that per channel, and this
//! module is the other half. It runs in two parts:
//!
//!   - The policy itself, offline and deterministic: every packaging
//!     surface in the tree is covered, every version parses, every state is
//!     one this checker implements, and no channel claims to serve a version
//!     the tree has not reached. A defect here fails the run, because it is
//!     a defect in the statement itself and needs no network to see.
//!
//!   - The feeds, over the network: what each one actually serves.
//!     Everything here is advisory. A feed that moved without the policy
//!     moving is worth knowing about the day it happens, but it is never a
//!     reason to fail a build: the feeds are outside this repository, and
//!     the answer to drift is a decision, never an automatic submission.
//!
//! Nothing in here submits, pushes or publishes anything: submission is the
//! protected distribution workflow's job, after a readiness report freezes.

use std::path::Path;

use anyhow::Context as _;
use serde_json::Value;

use crate::packaging;
use crate::process;

/// The packaging surfaces this repository actually carries, each with the
/// file or directory a channel of that name must have in the tree. A
/// channel added to `packaging/` without a matching line in the policy is
/// the case this list exists to catch.
const PACKAGED_CHANNELS: &[(&str, &str)] = &[
    ("chocolatey", "packaging/chocolatey/exosnap.nuspec"),
    ("scoop", "packaging/scoop/exosnap.json"),
    ("winget", "packaging/winget/manifests"),
];

/// What one channel's public feed reported, or why it could not be read.
pub struct FeedComparison {
    pub name: String,
    pub expected: String,
    pub observed: Option<String>,
    pub detail: String,
    pub agrees: bool,
}

#[derive(Default)]
pub struct FeedDriftReport {
    pub repository_version: String,
    /// Statements the policy itself must not make: undeclared channels, an
    /// unknown state, a version no channel could serve yet. Any of these
    /// fails.
    pub problems: Vec<String>,
    pub channel_names: Vec<String>,
    /// Empty unless the live half ran.
    pub feeds: Vec<FeedComparison>,
    pub advisories: Vec<String>,
}

impl FeedDriftReport {
    pub fn ok(&self) -> bool {
        self.problems.is_empty()
    }
}

/// A version for ordering, or `None` when the string is not `x.y.z`.
fn parse_version(value: &str) -> Option<(u64, u64, u64)> {
    let mut parts = value.split('.');
    let major = parts.next()?.parse().ok()?;
    let minor = parts.next()?.parse().ok()?;
    let patch = parts.next()?.parse().ok()?;
    if parts.next().is_some() {
        return None;
    }
    Some((major, minor, patch))
}

/// Reads the offline half: whether `policy_path` is internally consistent
/// with the packaging tree under `repo_root`. When `live` is set, also polls
/// each channel's public feed and records advisories; a feed that cannot be
/// read, or that disagrees with the policy, is never a `problem` and never
/// fails the run.
pub fn check(
    repo_root: &Path,
    policy_path: &Path,
    live: bool,
    timeout_seconds: u64,
) -> anyhow::Result<FeedDriftReport> {
    check_with_fetcher(repo_root, policy_path, live, timeout_seconds, &CurlFetcher)
}

/// `check`, with the live feed reader injected so a test can supply a
/// scripted double instead of reaching the network.
pub fn check_with_fetcher(
    repo_root: &Path,
    policy_path: &Path,
    live: bool,
    timeout_seconds: u64,
    fetcher: &dyn FeedFetcher,
) -> anyhow::Result<FeedDriftReport> {
    anyhow::ensure!(
        policy_path.is_file(),
        "No publication policy at '{}'.",
        policy_path.display()
    );
    let policy: Value = serde_json::from_str(&std::fs::read_to_string(policy_path)?)
        .with_context(|| format!("{} is not valid JSON", policy_path.display()))?;
    let channels = policy
        .get("channels")
        .and_then(Value::as_object)
        .with_context(|| format!("{} has no 'channels' object", policy_path.display()))?;

    let repository_version = packaging::cmake_project_version(repo_root)?;
    let repository_comparable = parse_version(&repository_version).with_context(|| {
        format!("CMakeLists.txt declares version '{repository_version}', which is not x.y.z")
    })?;

    let mut channel_names: Vec<String> = channels.keys().cloned().collect();
    channel_names.sort();

    const STATES: &[&str] = &["PENDING", "PREPARED", "SUBMITTED", "IN_REVIEW", "PUBLISHED"];

    let mut problems = Vec::new();

    for (required, marker) in PACKAGED_CHANNELS {
        if !channels.contains_key(*required) {
            problems.push(format!(
                "packaging/{required} is in the tree and the policy says nothing about it"
            ));
            continue;
        }
        if !repo_root.join(marker).exists() {
            problems.push(format!(
                "the policy covers '{required}', and '{marker}' is not in the tree"
            ));
        }
    }

    for name in &channel_names {
        let channel = &channels[name];
        let state = channel
            .get("state")
            .and_then(Value::as_str)
            .unwrap_or_default();
        if !STATES.contains(&state) {
            problems.push(format!(
                "{name} declares state '{state}'; it must be one of {}",
                STATES.join(", ")
            ));
            continue;
        }
        let version = channel
            .get("version")
            .and_then(Value::as_str)
            .unwrap_or_default();
        let Some(comparable) = parse_version(version) else {
            problems.push(format!(
                "{name} declares version '{version}', which is not x.y.z"
            ));
            continue;
        };
        if comparable > repository_comparable {
            problems.push(format!(
                "{name} claims {state} at {version}, ahead of the version the tree declares ({repository_version})"
            ));
        }
    }

    let mut feeds = Vec::new();
    let mut advisories = Vec::new();
    if live {
        for name in &channel_names {
            let channel = &channels[name];
            let expected = channel
                .get("version")
                .and_then(Value::as_str)
                .unwrap_or_default()
                .to_string();
            let feed_url = channel
                .get("feed")
                .and_then(Value::as_str)
                .unwrap_or_default();
            let (observed, detail) = read_feed_version(fetcher, name, feed_url, timeout_seconds);
            let agrees = observed.as_deref() == Some(expected.as_str());
            match &observed {
                None => advisories.push(format!("{name} could not be read: {detail}")),
                Some(version) if !agrees => advisories.push(format!(
                    "{name} serves {version} and the policy says {expected}; either the feed moved or the policy did not"
                )),
                Some(_) => {}
            }
            feeds.push(FeedComparison {
                name: name.clone(),
                expected,
                observed,
                detail,
                agrees,
            });
        }
    }

    Ok(FeedDriftReport {
        repository_version,
        problems,
        channel_names,
        feeds,
        advisories,
    })
}

/// Renders a report the way the offline and (when run) live halves printed
/// it: the policy verdict first, then per-feed lines and advisories.
pub fn render(report: &FeedDriftReport) -> String {
    let mut out = String::new();
    out.push_str(&format!(
        "Publication policy: the tree declares {}.\n\n",
        report.repository_version
    ));
    for problem in &report.problems {
        out.push_str(&format!("  policy: {problem}\n"));
    }
    if report.problems.is_empty() {
        out.push_str(&format!(
            "  policy: OK ({} channel(s) declared)\n",
            report.channel_names.len()
        ));
    }
    if !report.feeds.is_empty() {
        out.push('\n');
        for feed in &report.feeds {
            let observed = feed.observed.as_deref().unwrap_or("?");
            let verdict = if feed.observed.is_none() {
                ""
            } else if feed.agrees {
                "as declared"
            } else {
                "DRIFT"
            };
            out.push_str(&format!(
                "  {:<12} expected {:<8} observed {:<8} {verdict}\n",
                feed.name, feed.expected, observed
            ));
        }
    }
    if !report.advisories.is_empty() {
        out.push('\n');
        for advisory in &report.advisories {
            out.push_str(&format!("  advisory: {advisory}\n"));
        }
    }
    out.push('\n');
    if report.ok() {
        out.push_str("Publication policy OK.");
        if !report.advisories.is_empty() {
            out.push_str(&format!(
                " {} advisory finding(s) above; publishing is a decision, never automatic.",
                report.advisories.len()
            ));
        }
        out.push('\n');
    } else {
        out.push_str(&format!(
            "Publication policy FAILED: {} problem(s) in the publication policy.\n",
            report.problems.len()
        ));
    }
    out
}

/// Reads a public package feed, over the network. Injected behind
/// [`FeedFetcher`] so the offline half of `check` never has to touch it and
/// a test can supply a scripted double instead of reaching a real service.
pub trait FeedFetcher {
    /// GETs `url` and returns its body.
    fn get(&self, url: &str, timeout_seconds: u64) -> anyhow::Result<String>;
    /// GETs `url` without following a redirect, returning the `Location`
    /// header when the response is one. Chocolatey's package endpoint
    /// redirects to the newest `.nupkg`, and following it would download
    /// several megabytes nobody here needs.
    fn redirect_location(&self, url: &str, timeout_seconds: u64) -> anyhow::Result<Option<String>>;
}

pub struct CurlFetcher;

impl FeedFetcher for CurlFetcher {
    fn get(&self, url: &str, timeout_seconds: u64) -> anyhow::Result<String> {
        let mut command = process::command("curl");
        command.args(["-s", "-S", "--max-time", &timeout_seconds.to_string(), url]);
        let output = command.output().context("could not run curl")?;
        anyhow::ensure!(output.status.success(), "curl {url} failed");
        Ok(String::from_utf8_lossy(&output.stdout).into_owned())
    }

    fn redirect_location(&self, url: &str, timeout_seconds: u64) -> anyhow::Result<Option<String>> {
        let mut command = process::command("curl");
        command.args([
            "-s",
            "-S",
            "-D",
            "-",
            "-o",
            "NUL",
            "--max-time",
            &timeout_seconds.to_string(),
            url,
        ]);
        let output = command.output().context("could not run curl")?;
        anyhow::ensure!(output.status.success(), "curl {url} failed");
        let headers = String::from_utf8_lossy(&output.stdout);
        Ok(headers.lines().find_map(|line| {
            let (name, value) = line.split_once(':')?;
            name.trim()
                .eq_ignore_ascii_case("location")
                .then(|| value.trim().to_string())
        }))
    }
}

/// One channel's observed feed version, and a detail string describing
/// where it came from or why it could not be read. Each channel has its own
/// reader because each feed answers a different question: Chocolatey
/// redirects its package endpoint to the newest `.nupkg`, the WinGet
/// repository keeps one directory per published version, Scoop's bucket
/// entry is a manifest, and GitHub answers with the release itself.
fn read_feed_version(
    fetcher: &dyn FeedFetcher,
    channel: &str,
    feed: &str,
    timeout_seconds: u64,
) -> (Option<String>, String) {
    let result = match channel {
        "chocolatey" => read_chocolatey(fetcher, feed, timeout_seconds),
        "winget" => read_winget(fetcher, feed, timeout_seconds),
        "scoop" => read_scoop(fetcher, feed, timeout_seconds),
        "github" => read_github(fetcher, feed, timeout_seconds),
        other => Err(anyhow::anyhow!("no reader for channel '{other}'")),
    };
    match result {
        Ok(pair) => pair,
        Err(error) => (None, format!("unreachable: {error}")),
    }
}

fn read_chocolatey(
    fetcher: &dyn FeedFetcher,
    feed: &str,
    timeout_seconds: u64,
) -> anyhow::Result<(Option<String>, String)> {
    static PATTERN: std::sync::LazyLock<regex::Regex> = std::sync::LazyLock::new(|| {
        regex::Regex::new(r"/exosnap\.([0-9]+\.[0-9]+\.[0-9]+)\.nupkg").unwrap()
    });
    let location = fetcher.redirect_location(feed, timeout_seconds)?;
    match location
        .as_deref()
        .and_then(|location| PATTERN.captures(location))
    {
        Some(captures) => Ok((Some(captures[1].to_string()), location.unwrap())),
        None => Ok((
            None,
            "the package endpoint did not answer with a .nupkg redirect".to_string(),
        )),
    }
}

fn read_winget(
    fetcher: &dyn FeedFetcher,
    feed: &str,
    timeout_seconds: u64,
) -> anyhow::Result<(Option<String>, String)> {
    let body = fetcher.get(feed, timeout_seconds)?;
    let entries: Vec<Value> =
        serde_json::from_str(&body).context("winget feed is not valid JSON")?;
    let mut versions: Vec<(u64, u64, u64)> = entries
        .iter()
        .filter(|entry| entry.get("type").and_then(Value::as_str) == Some("dir"))
        .filter_map(|entry| entry.get("name").and_then(Value::as_str))
        .filter_map(parse_version)
        .collect();
    versions.sort();
    match versions.last() {
        Some((major, minor, patch)) => Ok((
            Some(format!("{major}.{minor}.{patch}")),
            format!("{} version(s) published", versions.len()),
        )),
        None => Ok((
            None,
            "the package directory holds no version directory".to_string(),
        )),
    }
}

fn read_scoop(
    fetcher: &dyn FeedFetcher,
    feed: &str,
    timeout_seconds: u64,
) -> anyhow::Result<(Option<String>, String)> {
    let body = fetcher.get(feed, timeout_seconds)?;
    let manifest: Value =
        serde_json::from_str(&body).context("scoop manifest is not valid JSON")?;
    let version = manifest
        .get("version")
        .and_then(Value::as_str)
        .unwrap_or_default();
    let url = manifest
        .get("architecture")
        .and_then(|a| a.get("64bit"))
        .and_then(|a| a.get("url"))
        .and_then(Value::as_str)
        .unwrap_or_default();
    Ok((Some(version.to_string()), url.to_string()))
}

fn read_github(
    fetcher: &dyn FeedFetcher,
    feed: &str,
    timeout_seconds: u64,
) -> anyhow::Result<(Option<String>, String)> {
    let body = fetcher.get(feed, timeout_seconds)?;
    let release: Value = serde_json::from_str(&body).context("github release is not valid JSON")?;
    let tag = release
        .get("tag_name")
        .and_then(Value::as_str)
        .unwrap_or_default();
    let published = release
        .get("published_at")
        .and_then(Value::as_str)
        .unwrap_or_default();
    Ok((
        Some(tag.strip_prefix('v').unwrap_or(tag).to_string()),
        format!("published {published}"),
    ))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    struct UnreachableFetcher;
    impl FeedFetcher for UnreachableFetcher {
        fn get(&self, _url: &str, _timeout_seconds: u64) -> anyhow::Result<String> {
            anyhow::bail!("network access is not available to this test")
        }
        fn redirect_location(
            &self,
            _url: &str,
            _timeout_seconds: u64,
        ) -> anyhow::Result<Option<String>> {
            anyhow::bail!("network access is not available to this test")
        }
    }

    fn fixture(version: &str, channels: &Value) -> tempfile::TempDir {
        let dir = tempfile::tempdir().unwrap();
        std::fs::create_dir_all(dir.path().join("packaging/chocolatey")).unwrap();
        std::fs::create_dir_all(dir.path().join("packaging/winget/manifests")).unwrap();
        std::fs::create_dir_all(dir.path().join("packaging/scoop")).unwrap();
        std::fs::write(
            dir.path().join("packaging/chocolatey/exosnap.nuspec"),
            "<package />",
        )
        .unwrap();
        std::fs::write(dir.path().join("packaging/scoop/exosnap.json"), "{}").unwrap();
        std::fs::write(
            dir.path().join("CMakeLists.txt"),
            format!("project(exosnap VERSION {version} LANGUAGES C CXX)\n"),
        )
        .unwrap();
        let policy =
            serde_json::json!({ "policyVersion": 1, "product": "ExoSnap", "channels": channels });
        std::fs::write(
            dir.path().join("policy.json"),
            serde_json::to_string_pretty(&policy).unwrap(),
        )
        .unwrap();
        dir
    }

    fn passing_channels(version: &str) -> Value {
        serde_json::json!({
            "github": { "state": "PUBLISHED", "version": version, "feed": "https://example.invalid/github" },
            "chocolatey": { "state": "SUBMITTED", "version": version, "feed": "https://example.invalid/choco" },
            "winget": { "state": "IN_REVIEW", "version": version, "feed": "https://example.invalid/winget" },
            "scoop": { "state": "PUBLISHED", "version": version, "feed": "https://example.invalid/scoop" },
        })
    }

    fn check_offline(dir: &tempfile::TempDir) -> anyhow::Result<FeedDriftReport> {
        let policy_path: PathBuf = dir.path().join("policy.json");
        check_with_fetcher(dir.path(), &policy_path, false, 1, &UnreachableFetcher)
    }

    #[test]
    fn the_tracked_policy_in_this_repository_is_consistent_with_its_packaging_tree() {
        let repo_root = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../..")
            .canonicalize()
            .unwrap();
        let policy_path = repo_root.join("packaging/publication-policy.json");
        let report =
            check_with_fetcher(&repo_root, &policy_path, false, 1, &UnreachableFetcher).unwrap();
        assert!(report.ok(), "{:?}", report.problems);
    }

    #[test]
    fn a_fixture_built_to_pass_does_pass() {
        let dir = fixture("0.10.0", &passing_channels("0.10.0"));
        let report = check_offline(&dir).unwrap();
        assert!(report.ok(), "{:?}", report.problems);
    }

    #[test]
    fn a_packaging_surface_the_policy_says_nothing_about_fails() {
        let mut channels = passing_channels("0.10.0");
        channels.as_object_mut().unwrap().remove("chocolatey");
        let dir = fixture("0.10.0", &channels);
        let report = check_offline(&dir).unwrap();
        assert!(!report.ok());
        assert!(
            report
                .problems
                .iter()
                .any(|p| p.contains("packaging/chocolatey is in the tree"))
        );
    }

    #[test]
    fn an_unknown_state_fails() {
        let mut channels = passing_channels("0.10.0");
        channels["chocolatey"]["state"] = serde_json::json!("maybe");
        let dir = fixture("0.10.0", &channels);
        let report = check_offline(&dir).unwrap();
        assert!(!report.ok());
        assert!(
            report
                .problems
                .iter()
                .any(|p| p.contains("declares state 'maybe'"))
        );
    }

    #[test]
    fn a_channel_ahead_of_the_tree_version_fails() {
        let mut channels = passing_channels("0.10.0");
        channels["github"]["version"] = serde_json::json!("9.9.9");
        let dir = fixture("0.10.0", &channels);
        let report = check_offline(&dir).unwrap();
        assert!(!report.ok());
        assert!(
            report
                .problems
                .iter()
                .any(|p| p.contains("ahead of the version the tree declares"))
        );
    }

    #[test]
    fn a_published_channel_behind_the_tree_is_reported_not_failed() {
        let mut channels = passing_channels("0.10.0");
        channels["scoop"]["state"] = serde_json::json!("PENDING");
        channels["scoop"]["version"] = serde_json::json!("0.9.0");
        let dir = fixture("0.10.0", &channels);
        let report = check_offline(&dir).unwrap();
        assert!(report.ok(), "{:?}", report.problems);
    }

    #[test]
    fn a_version_that_is_not_x_y_z_fails() {
        let mut channels = passing_channels("0.10.0");
        channels["chocolatey"]["version"] = serde_json::json!("latest");
        let dir = fixture("0.10.0", &channels);
        let report = check_offline(&dir).unwrap();
        assert!(!report.ok());
        assert!(report.problems.iter().any(|p| p.contains("'latest'")));
    }
}
