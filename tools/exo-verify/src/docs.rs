//! Structural documentation checks. Structure only: prose quality is review work.
//!
//! - removed archives (`docs/decisions/`, `docs/superpowers/`) must not return;
//! - no tracked file refers to a numbered decision record or the removed archive;
//! - current Markdown never links the private working directory;
//! - relative Markdown links, images and heading fragments resolve;
//! - every document the documentation index links exists;
//! - retired release-verification paths do not silently return.

use anyhow::{Context, Result};
use regex::Regex;
use std::collections::{BTreeSet, HashMap};
use std::path::{Component, Path, PathBuf};
use std::process::Command;
use std::sync::LazyLock;

/// Documents whose job is to record history rather than describe the current tree.
const HISTORICAL: &[&str] = &["CHANGELOG.md"];

const ARCHIVES: &[&str] = &["docs/decisions/", "docs/superpowers/"];

/// Paths of the retired release-verification stack. A file reappearing here
/// means a second verification engine is growing back.
const RETIRED: &[&str] = &[
    "scripts/release-verify.ps1",
    "scripts/lib/ReleaseScenarios.ps1",
    "scripts/lib/ReleaseQualification.ps1",
    "scripts/lib/release-policy.json",
    "scripts/check-release-qualification.ps1",
    "scripts/check-release-promotion.ps1",
    "scripts/find-qualified-rc.ps1",
    "scripts/check-documentation.py",
    "tools/release-verify/",
];

static DECISION: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"(?i)\bADRs?[\s:/#*_\-\x{2010}-\x{2014}]*\d{4}\b").unwrap());
static ARCHIVE_REFERENCE: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"(?i)docs[/\\]decisions[/\\]").unwrap());
static PRIVATE: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"(?i)\.workspace[/\\]").unwrap());
static FENCE: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"^\s{0,3}(`{3,}|~{3,})").unwrap());
static EXPLICIT_ANCHOR: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r#"(?i)\b(?:id|name)\s*=\s*["']([^"']+)["']"#).unwrap());
static HTML_LINK: LazyLock<Regex> = LazyLock::new(|| {
    Regex::new(r#"(?i)<(?:a|img)\b[^>]*\b(?:href|src)\s*=\s*["']([^"']+)["']"#).unwrap()
});
static ATX_HEADING: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"^ {0,3}#{1,6}\s+(.+?)\s*#*\s*$").unwrap());
static SETEXT_UNDERLINE: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"^(?:={3,}|-{3,})\s*$").unwrap());
static INLINE_LINK_START: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"!?\[[^\]\n]*\]\(").unwrap());
static REFERENCE_DEF: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"(?m)^ {0,3}\[[^\]\n]+\]:\s*(<[^>]+>|\S+)").unwrap());
static TAG: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"<[^>]+>").unwrap());
static MD_LINK_TEXT: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"!?\[([^\]]+)\]\([^)]*\)").unwrap());

/// Blanks fenced blocks and inline code spans, keeping line structure.
pub fn prose(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    let mut fence: Option<String> = None;
    for line in text.split_inclusive('\n') {
        let newline = if line.ends_with('\n') { "\n" } else { "" };
        let marker = FENCE.captures(line).map(|c| c[1].to_string());
        match (&fence, marker) {
            (None, Some(m)) => {
                fence = Some(m);
                out.push_str(newline);
            }
            (Some(open), m) => {
                if let Some(m) = m
                    && m.as_bytes()[0] == open.as_bytes()[0]
                    && m.len() >= open.len()
                {
                    fence = None;
                }
                out.push_str(newline);
            }
            (None, None) => out.push_str(&blank_code_spans(line)),
        }
    }
    out
}

fn blank_code_spans(line: &str) -> String {
    let bytes = line.as_bytes();
    let mut out = String::with_capacity(line.len());
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'`' {
            let run = bytes[i..].iter().take_while(|&&b| b == b'`').count();
            let search_from = i + run;
            let mut j = search_from;
            let mut close = None;
            while j < bytes.len() {
                if bytes[j] == b'`' {
                    let r = bytes[j..].iter().take_while(|&&b| b == b'`').count();
                    if r == run {
                        close = Some(j);
                        break;
                    }
                    j += r;
                } else {
                    j += 1;
                }
            }
            if let Some(end) = close {
                let span_end = end + run;
                out.extend(std::iter::repeat_n(' ', line[i..span_end].chars().count()));
                i = span_end;
                continue;
            }
            out.push_str(&line[i..i + run]);
            i += run;
            continue;
        }
        let ch = line[i..].chars().next().unwrap();
        out.push(ch);
        i += ch.len_utf8();
    }
    out
}

/// GitHub-style heading anchors plus explicit `id`/`name` anchors.
pub fn anchors(text: &str) -> BTreeSet<String> {
    let mut result: BTreeSet<String> = EXPLICIT_ANCHOR
        .captures_iter(text)
        .map(|c| c[1].to_string())
        .collect();
    let mut used: HashMap<String, usize> = HashMap::new();
    let mut fence: Option<String> = None;
    let mut previous: Option<String> = None;
    let mut titles = Vec::new();
    for line in text.lines() {
        if let Some(c) = FENCE.captures(line) {
            let m = c[1].to_string();
            match &fence {
                None => fence = Some(m),
                Some(open) if m.as_bytes()[0] == open.as_bytes()[0] && m.len() >= open.len() => {
                    fence = None
                }
                _ => {}
            }
            previous = None;
            continue;
        }
        if fence.is_some() {
            continue;
        }
        if let Some(c) = ATX_HEADING.captures(line) {
            titles.push(c[1].to_string());
            previous = None;
            continue;
        }
        if SETEXT_UNDERLINE.is_match(line)
            && let Some(p) = previous.take()
            && !p.trim().is_empty()
        {
            titles.push(p);
            continue;
        }
        previous = Some(line.to_string());
    }
    for title in titles {
        let title = TAG.replace_all(&title, "").replace('`', "");
        let title = MD_LINK_TEXT.replace_all(&title, "$1").to_string();
        let slug: String = title
            .to_lowercase()
            .chars()
            .filter(|c| c.is_alphanumeric() || *c == '_' || *c == '-' || *c == ' ')
            .map(|c| if c == ' ' { '-' } else { c })
            .collect();
        let n = used.entry(slug.clone()).or_insert(0);
        result.insert(if *n == 0 {
            slug.clone()
        } else {
            format!("{slug}-{n}")
        });
        *n += 1;
    }
    result
}

fn line_of(text: &str, offset: usize) -> usize {
    text[..offset].matches('\n').count() + 1
}

/// Link destinations with their line numbers: inline, reference definitions and HTML.
pub fn links(text: &str) -> Vec<(String, usize)> {
    let clean = prose(text);
    let bytes = clean.as_bytes();
    let mut out = Vec::new();
    for m in INLINE_LINK_START.find_iter(&clean) {
        let mut pos = m.end();
        while pos < bytes.len() && (bytes[pos] as char).is_whitespace() {
            pos += 1;
        }
        if pos < bytes.len() && bytes[pos] == b'<' {
            if let Some(end) = clean[pos + 1..].find('>') {
                out.push((
                    clean[pos + 1..pos + 1 + end].to_string(),
                    line_of(&clean, m.start()),
                ));
            }
            continue;
        }
        let mut end = pos;
        let mut depth = 0usize;
        while end < bytes.len() {
            let c = bytes[end];
            if c == b'\\' && end + 1 < bytes.len() {
                end += 2;
                continue;
            }
            if c == b'(' {
                depth += 1;
            } else if c == b')' {
                if depth == 0 {
                    break;
                }
                depth -= 1;
            } else if (c as char).is_whitespace() && depth == 0 {
                break;
            }
            end += 1;
        }
        out.push((
            clean[pos..end.min(bytes.len())].to_string(),
            line_of(&clean, m.start()),
        ));
    }
    for c in REFERENCE_DEF.captures_iter(&clean) {
        let whole = c.get(0).unwrap();
        out.push((
            c[1].trim_matches(|ch| ch == '<' || ch == '>').to_string(),
            line_of(&clean, whole.start()),
        ));
    }
    for c in HTML_LINK.captures_iter(&clean) {
        out.push((c[1].to_string(), line_of(&clean, c.get(0).unwrap().start())));
    }
    out
}

fn percent_decode(s: &str) -> String {
    let bytes = s.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'%'
            && i + 2 < bytes.len()
            && s.is_char_boundary(i + 3)
            && let Ok(v) = u8::from_str_radix(&s[i + 1..i + 3], 16)
        {
            out.push(v);
            i += 3;
            continue;
        }
        out.push(bytes[i]);
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

fn normalize(path: &Path) -> PathBuf {
    let mut out = PathBuf::new();
    for c in path.components() {
        match c {
            Component::ParentDir => {
                out.pop();
            }
            Component::CurDir => {}
            other => out.push(other.as_os_str()),
        }
    }
    out
}

fn has_scheme(destination: &str) -> bool {
    let Some(colon) = destination.find(':') else {
        return false;
    };
    let scheme = &destination[..colon];
    !scheme.is_empty()
        && scheme.chars().next().unwrap().is_ascii_alphabetic()
        && scheme
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || matches!(c, '+' | '-' | '.'))
}

/// Checks the given repository-relative paths. Paths that do not exist in the
/// working tree are deletions and are skipped.
pub fn check(root: &Path, paths: &[String]) -> Vec<String> {
    let mut failures = Vec::new();
    let mut anchor_cache: HashMap<PathBuf, BTreeSet<String>> = HashMap::new();
    let unique: BTreeSet<&String> = paths.iter().collect();
    for name in unique {
        let name = name.replace('\\', "/");
        if HISTORICAL.contains(&name.as_str()) {
            continue;
        }
        let path = root.join(&name);
        if !path.is_file() {
            continue;
        }
        if ARCHIVES.iter().any(|a| name.starts_with(a)) {
            failures.push(format!("{name}: forbidden archive"));
        }
        if RETIRED.iter().any(|r| {
            if r.ends_with('/') {
                name.starts_with(r)
            } else {
                name == *r
            }
        }) {
            failures.push(format!("{name}: retired release-verification path; exo-verify is the only verification engine"));
        }
        let Ok(data) = std::fs::read(&path) else {
            continue;
        };
        if data.contains(&0) {
            continue;
        }
        let Ok(text) = String::from_utf8(data) else {
            continue;
        };
        let text = text.strip_prefix('\u{feff}').unwrap_or(&text).to_string();
        let is_self = name == "tools/exo-verify/src/docs.rs";
        if !is_self {
            for (pattern, label) in [
                (&*DECISION, "numbered decision reference"),
                (&*ARCHIVE_REFERENCE, "removed archive reference"),
            ] {
                for m in pattern.find_iter(&text) {
                    failures.push(format!(
                        "{name}:{}: {label}: {}",
                        line_of(&text, m.start()),
                        m.as_str()
                    ));
                }
            }
        }
        if !name.to_ascii_lowercase().ends_with(".md") {
            continue;
        }
        for m in PRIVATE.find_iter(&text) {
            failures.push(format!(
                "{name}:{}: private working path",
                line_of(&text, m.start())
            ));
        }
        for (destination, line) in links(&text) {
            if destination.is_empty() || destination.contains("${") {
                continue;
            }
            let destination = destination
                .replace("\\(", "(")
                .replace("\\)", ")")
                .replace("\\ ", " ");
            if has_scheme(&destination) || destination.starts_with("//") {
                continue;
            }
            let (path_part, fragment) = match destination.split_once('#') {
                Some((p, f)) => (p, Some(f)),
                None => (destination.as_str(), None),
            };
            let path_part = path_part.split('?').next().unwrap_or_default();
            let target = if path_part.is_empty() {
                path.clone()
            } else if let Some(stripped) = path_part.strip_prefix('/') {
                root.join(percent_decode(stripped))
            } else {
                path.parent().unwrap().join(percent_decode(path_part))
            };
            let target = normalize(&target);
            if !target.starts_with(normalize(root)) {
                failures.push(format!(
                    "{name}:{line}: link outside repository: {destination}"
                ));
            } else if !target.exists() {
                failures.push(format!("{name}:{line}: missing target: {destination}"));
            } else if let Some(fragment) = fragment
                && target
                    .extension()
                    .is_some_and(|e| e.eq_ignore_ascii_case("md"))
            {
                let set = anchor_cache.entry(target.clone()).or_insert_with(|| {
                    let raw = std::fs::read_to_string(&target).unwrap_or_default();
                    anchors(raw.strip_prefix('\u{feff}').unwrap_or(&raw))
                });
                if !set.contains(&percent_decode(fragment)) {
                    failures.push(format!("{name}:{line}: missing fragment: {destination}"));
                }
            }
        }
    }
    failures
}

/// Tracked and new, non-ignored paths of the working tree.
pub fn repository_paths(root: &Path) -> Result<Vec<String>> {
    let output = Command::new("git")
        .arg("-C")
        .arg(root)
        .args([
            "ls-files",
            "-z",
            "--cached",
            "--others",
            "--exclude-standard",
        ])
        .output()
        .context("run git ls-files")?;
    anyhow::ensure!(output.status.success(), "git ls-files failed");
    Ok(String::from_utf8(output.stdout)?
        .split('\0')
        .filter(|p| !p.is_empty())
        .map(String::from)
        .collect())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    fn repo(files: &[(&str, &str)]) -> (tempfile::TempDir, Vec<String>) {
        let dir = tempfile::tempdir().unwrap();
        for (name, content) in files {
            let path = dir.path().join(name);
            fs::create_dir_all(path.parent().unwrap()).unwrap();
            fs::write(path, content).unwrap();
        }
        (dir, files.iter().map(|(n, _)| n.to_string()).collect())
    }

    #[test]
    fn a_clean_tree_passes() {
        let (dir, paths) = repo(&[
            (
                "docs/README.md",
                "# Docs\n\n[Arch](architecture/overview.md#processes-and-layers)\n",
            ),
            (
                "docs/architecture/overview.md",
                "# Overview\n\n## Processes and layers\n",
            ),
        ]);
        assert_eq!(check(dir.path(), &paths), Vec::<String>::new());
    }

    #[test]
    fn decision_references_are_reported_in_any_file() {
        let (dir, paths) = repo(&[
            ("src/a.cpp", "// see ADR 0041\n/*\n * ADR-0012\n */\n"),
            ("x.md", "Per ADR 0033.\n"),
        ]);
        let failures = check(dir.path(), &paths);
        assert_eq!(failures.len(), 3, "{failures:?}");
    }

    #[test]
    fn archives_and_retired_paths_cannot_return() {
        let (dir, paths) = repo(&[
            ("docs/decisions/0001-x.md", "# x\n"),
            ("scripts/release-verify.ps1", "#\n"),
            ("tools/release-verify/X.cs", "//\n"),
        ]);
        let failures = check(dir.path(), &paths);
        assert!(failures.iter().any(|f| f.contains("forbidden archive")));
        assert_eq!(failures.iter().filter(|f| f.contains("retired")).count(), 2);
    }

    #[test]
    fn broken_links_and_fragments_are_reported() {
        let (dir, paths) = repo(&[
            (
                "docs/README.md",
                "[a](missing.md) [b](other.md#nope) [c](other.md#real-heading) [d](https://x/y)\n",
            ),
            ("docs/other.md", "# Real heading\n"),
        ]);
        let failures = check(dir.path(), &paths);
        assert_eq!(failures.len(), 2, "{failures:?}");
        assert!(failures[0].contains("missing target"));
        assert!(failures[1].contains("missing fragment"));
    }

    #[test]
    fn private_paths_in_markdown_are_reported_but_code_is_ignored() {
        let (dir, paths) = repo(&[(
            "README.md",
            "See .workspace/plan.md\n\n```\n[x](nowhere.md)\n```\n`[y](nowhere.md)`\n",
        )]);
        let failures = check(dir.path(), &paths);
        assert_eq!(failures.len(), 1, "{failures:?}");
        assert!(failures[0].contains("private working path"));
    }

    #[test]
    fn duplicate_headings_get_numbered_anchors() {
        let set = anchors("# A\n## Same\n## Same\nSetext\n---\n");
        assert!(set.contains("same") && set.contains("same-1") && set.contains("setext"));
    }

    #[test]
    fn links_outside_the_repository_are_reported() {
        let (dir, paths) = repo(&[("docs/a.md", "[x](../../outside.md)\n")]);
        assert!(check(dir.path(), &paths)[0].contains("outside repository"));
    }

    #[test]
    fn historical_changelog_is_exempt() {
        let (dir, paths) = repo(&[("CHANGELOG.md", "ADR 0001 [x](gone.md)\n")]);
        assert!(check(dir.path(), &paths).is_empty());
    }
}
