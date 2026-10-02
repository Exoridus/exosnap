//! What a screenshot run leaves behind next to its PNGs: `manifest.json`, which
//! binds every image to the binary and source that produced it, and
//! `index.html`, a contact sheet for looking at them.
//!
//! A run directory accumulates: a later run into it replaces the shots it
//! retakes and keeps the rest, so both files describe the whole directory, not
//! only the last run.

use std::path::Path;

use serde::{Deserialize, Serialize};
use serde_json::{Value, json};

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Status {
    Ok,
    /// Saved, but the app logged that part of the request did not apply.
    Warn,
    Failed,
    #[serde(rename = "timeout")]
    TimedOut,
}

impl Status {
    pub fn id(self) -> &'static str {
        match self {
            Status::Ok => "ok",
            Status::Warn => "warn",
            Status::Failed => "failed",
            Status::TimedOut => "timeout",
        }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ShotResult {
    pub name: String,
    pub status: Status,
    pub file: String,
    pub overlays: Vec<String>,
    pub exit_code: Option<i32>,
    pub duration_ms: u64,
    pub warnings: Vec<String>,
    pub log: String,
    /// The binary this shot photographed. Shots in one directory can come
    /// from different builds of the same source state.
    #[serde(default)]
    pub binary_sha256: String,
    pub args: Vec<String>,
    pub env: Vec<(String, String)>,
}

/// The binary a run photographed.
#[derive(Clone, Debug)]
pub struct Binary {
    pub path: String,
    pub tree: Option<String>,
    pub built_at_utc: String,
    pub bytes: u64,
    pub sha256: String,
}

pub struct RunInfo<'a> {
    pub created_at_utc: &'a str,
    pub binary: &'a Binary,
    pub head: Option<String>,
    pub dirty: bool,
    /// The working tree digest from the same source-identity rule the test
    /// receipts use, so a screenshot set and a test receipt can be tied to one
    /// source state. None when the tree could not be hashed.
    pub source_fingerprint: Option<String>,
    pub version: Option<String>,
    pub qt_bin: Option<String>,
    /// Composed overlay sheets, relative to the run directory.
    pub sheets: Vec<String>,
}

pub fn manifest(info: &RunInfo<'_>, results: &[ShotResult]) -> Value {
    let count = |status: Status| results.iter().filter(|r| r.status == status).count();
    json!({
        "tool": "exo-dev screenshot",
        "updatedAtUtc": info.created_at_utc,
        "binary": {
            "path": info.binary.path,
            "tree": info.binary.tree,
            "builtAtUtc": info.binary.built_at_utc,
            "bytes": info.binary.bytes,
            "sha256": info.binary.sha256,
        },
        "source": {
            "head": info.head,
            "dirty": info.dirty,
            "fingerprint": info.source_fingerprint,
            "version": info.version,
        },
        "qtBin": info.qt_bin,
        "sheets": info.sheets,
        "summary": {
            "total": results.len(),
            "ok": count(Status::Ok),
            "warn": count(Status::Warn),
            "failed": count(Status::Failed),
            "timedOut": count(Status::TimedOut),
        },
        "shots": results,
    })
}

/// This run's results over whatever an earlier run left in `dir`: a retaken
/// shot replaces its entry in place, anything else keeps its position, and a
/// new shot is appended. An entry whose image is gone is dropped.
fn merge(dir: &Path, previous: &Value, results: &[ShotResult]) -> Vec<ShotResult> {
    let mut merged: Vec<ShotResult> = previous
        .get("shots")
        .and_then(|shots| serde_json::from_value(shots.clone()).ok())
        .unwrap_or_default();
    merged.retain(|shot| dir.join(&shot.file).exists());
    for result in results {
        match merged.iter_mut().find(|shot| shot.name == result.name) {
            Some(existing) => *existing = result.clone(),
            None => merged.push(result.clone()),
        }
    }
    merged
}

fn merge_sheets(dir: &Path, previous: &Value, sheets: &[String]) -> Vec<String> {
    let mut merged: Vec<String> = previous
        .get("sheets")
        .and_then(|sheets| serde_json::from_value(sheets.clone()).ok())
        .unwrap_or_default();
    for sheet in sheets {
        if !merged.contains(sheet) {
            merged.push(sheet.clone());
        }
    }
    merged.retain(|sheet| dir.join(sheet).exists());
    merged
}

/// Writes both files for the whole directory and returns how many shots it
/// now holds.
pub fn write(dir: &Path, info: &RunInfo<'_>, results: &[ShotResult]) -> anyhow::Result<usize> {
    let previous: Value = std::fs::read_to_string(dir.join("manifest.json"))
        .ok()
        .and_then(|text| serde_json::from_str(&text).ok())
        .unwrap_or(Value::Null);
    let shots = merge(dir, &previous, results);
    let info = RunInfo {
        sheets: merge_sheets(dir, &previous, &info.sheets),
        head: info.head.clone(),
        source_fingerprint: info.source_fingerprint.clone(),
        version: info.version.clone(),
        qt_bin: info.qt_bin.clone(),
        ..*info
    };
    std::fs::write(
        dir.join("manifest.json"),
        serde_json::to_string_pretty(&manifest(&info, &shots))? + "\n",
    )?;
    std::fs::write(dir.join("index.html"), html(&info, &shots))?;
    Ok(shots.len())
}

fn escape(text: &str) -> String {
    text.replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('"', "&quot;")
}

fn sheets_html(sheets: &[String]) -> String {
    if sheets.is_empty() {
        return String::new();
    }
    let figures: String = sheets
        .iter()
        .map(|sheet| {
            format!(
                "<figure class=\"sheet\" data-name=\"{0}\"><a href=\"{0}\"><img loading=\"lazy\" \
                 src=\"{0}\" alt=\"{0}\"></a><figcaption><b>{0}</b></figcaption></figure>\n",
                escape(sheet)
            )
        })
        .collect();
    format!("<h2>Overlay sheets</h2>\n<main>\n{figures}</main>\n<h2>Shots</h2>\n")
}

pub fn html(info: &RunInfo<'_>, results: &[ShotResult]) -> String {
    let mut cards = String::new();
    for result in results {
        let mut images = String::new();
        if result.status == Status::Ok || result.status == Status::Warn {
            images.push_str(&format!(
                "<a href=\"{0}\"><img loading=\"lazy\" src=\"{0}\" alt=\"{1}\"></a>",
                escape(&result.file),
                escape(&result.name)
            ));
        }
        for overlay in &result.overlays {
            images.push_str(&format!(
                "<a class=\"overlay\" href=\"{0}\"><img loading=\"lazy\" src=\"{0}\" alt=\"{0}\"></a>",
                escape(overlay)
            ));
        }
        let warnings: String = result
            .warnings
            .iter()
            .map(|w| format!("<li>{}</li>", escape(w)))
            .collect();
        cards.push_str(&format!(
            "<figure class=\"{status}\" data-name=\"{name}\">{images}<figcaption>\
             <span class=\"badge\">{status}</span> <b>{name}</b> \
             <a class=\"log\" href=\"{log}\">log</a> <span class=\"ms\">{ms} ms</span>\
             <ul>{warnings}</ul></figcaption></figure>\n",
            status = result.status.id(),
            name = escape(&result.name),
            log = escape(&result.log),
            ms = result.duration_ms,
        ));
    }
    let failed = results
        .iter()
        .filter(|r| matches!(r.status, Status::Failed | Status::TimedOut))
        .count();
    let warned = results.iter().filter(|r| r.status == Status::Warn).count();
    format!(
        r#"<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ExoSnap screenshots</title>
<style>
:root {{ --bg: #f4f5f6; --card: #ffffff; --text: #16181a; --muted: #5d646b; --line: #d9dde1;
  --ok: #1f7a4d; --warn: #9a6200; --fail: #b3261e; }}
@media (prefers-color-scheme: dark) {{
  :root {{ --bg: #111315; --card: #1b1e21; --text: #e8eaec; --muted: #9aa1a8; --line: #2c3035;
    --ok: #5fcf95; --warn: #f0b44c; --fail: #ff8a80; }}
}}
body {{ margin: 0; padding: 16px; background: var(--bg); color: var(--text);
  font: 14px/1.4 system-ui, sans-serif; }}
header {{ margin-bottom: 16px; }}
header p {{ margin: 4px 0; color: var(--muted); }}
code {{ font-family: ui-monospace, Consolas, monospace; font-size: 12px; }}
input {{ margin-top: 8px; width: min(420px, 100%); padding: 6px 8px; background: var(--card);
  color: var(--text); border: 1px solid var(--line); border-radius: 6px; }}
main {{ display: grid; grid-template-columns: repeat(auto-fill, minmax(360px, 1fr)); gap: 16px; }}
figure {{ margin: 0; background: var(--card); border: 1px solid var(--line); border-radius: 8px;
  padding: 8px; }}
figure img {{ display: block; width: 100%; height: auto; border-radius: 4px; }}
figure .overlay {{ display: inline-block; width: 30%; margin: 6px 6px 0 0; }}
figcaption {{ margin-top: 6px; word-break: break-all; }}
figcaption ul {{ margin: 4px 0 0; padding-left: 18px; color: var(--warn); }}
.badge {{ font-size: 11px; text-transform: uppercase; font-weight: 600; }}
.ok .badge {{ color: var(--ok); }} .warn .badge {{ color: var(--warn); }}
.failed .badge, .timeout .badge {{ color: var(--fail); }}
.log, .ms {{ color: var(--muted); font-size: 12px; }}
</style>
</head>
<body>
<header>
<h1>ExoSnap screenshots</h1>
<p>{total} shots, {warned} with warnings, {failed} failed. Last updated {created} UTC.</p>
<p>Binary <code>{path}</code>, built {built} UTC, sha256 <code>{sha}</code>.</p>
<p>Source <code>{head}</code>{dirty}.</p>
<input id="filter" type="search" placeholder="Filter by name">
</header>
{sheets}<main>
{cards}</main>
<script>
document.getElementById('filter').addEventListener('input', (event) => {{
  const term = event.target.value.toLowerCase();
  for (const figure of document.querySelectorAll('figure')) {{
    figure.hidden = !figure.dataset.name.includes(term);
  }}
}});
</script>
</body>
</html>
"#,
        sheets = sheets_html(&info.sheets),
        total = results.len(),
        created = escape(info.created_at_utc),
        path = escape(&info.binary.path),
        built = escape(&info.binary.built_at_utc),
        sha = escape(&info.binary.sha256[..16.min(info.binary.sha256.len())]),
        head = escape(info.head.as_deref().unwrap_or("unknown")),
        dirty = if info.dirty {
            " with uncommitted changes"
        } else {
            ""
        },
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    fn binary() -> Binary {
        Binary {
            path: "C:/b/exosnap.exe".to_string(),
            tree: Some("build/windows-x64-debug".to_string()),
            built_at_utc: "2026-09-30T10:00:00.0000000Z".to_string(),
            bytes: 1,
            sha256: "ab".repeat(32),
        }
    }

    fn result(status: Status) -> ShotResult {
        ShotResult {
            name: "record-paused".to_string(),
            file: "record-paused.png".to_string(),
            overlays: vec![],
            args: vec!["--visual-test".to_string()],
            env: vec![],
            exit_code: Some(0),
            duration_ms: 1200,
            status,
            warnings: vec!["<b>".to_string()],
            log: "logs/record-paused.log".to_string(),
            binary_sha256: String::new(),
        }
    }

    #[test]
    fn the_manifest_counts_each_status() {
        let binary = binary();
        let info = RunInfo {
            created_at_utc: "now",
            binary: &binary,
            head: None,
            dirty: true,
            source_fingerprint: None,
            version: None,
            qt_bin: None,
            sheets: vec![],
        };
        let manifest = manifest(&info, &[result(Status::Ok), result(Status::TimedOut)]);
        assert_eq!(manifest["summary"]["ok"], 1);
        assert_eq!(manifest["summary"]["timedOut"], 1);
        assert_eq!(manifest["source"]["dirty"], true);
    }

    #[test]
    fn the_source_fingerprint_is_serialized_exactly() {
        let binary = binary();
        let info = RunInfo {
            created_at_utc: "now",
            binary: &binary,
            head: Some("953338cd".to_string()),
            dirty: true,
            source_fingerprint: Some("2b6bd7f5f8581418".to_string()),
            version: None,
            qt_bin: None,
            sheets: vec![],
        };
        let manifest = manifest(&info, &[]);
        // The manifest must carry the input digest unchanged: a lowercased,
        // truncated or re-encoded fingerprint would not compare equal to the
        // test receipt's for the same source state.
        assert_eq!(manifest["source"]["fingerprint"], "2b6bd7f5f8581418");
        assert_eq!(manifest["source"]["head"], "953338cd");
    }

    #[test]
    fn an_unavailable_source_fingerprint_is_null_not_omitted() {
        let binary = binary();
        let info = RunInfo {
            created_at_utc: "now",
            binary: &binary,
            head: None,
            dirty: false,
            source_fingerprint: None,
            version: None,
            qt_bin: None,
            sheets: vec![],
        };
        let manifest = manifest(&info, &[]);
        assert!(manifest["source"].get("fingerprint").is_some());
        assert!(manifest["source"]["fingerprint"].is_null());
    }

    #[test]
    fn the_contact_sheet_escapes_app_output() {
        let binary = binary();
        let info = RunInfo {
            created_at_utc: "now",
            binary: &binary,
            head: None,
            dirty: false,
            source_fingerprint: None,
            version: None,
            qt_bin: None,
            sheets: vec![],
        };
        let page = html(&info, &[result(Status::Warn)]);
        assert!(page.contains("<li>&lt;b&gt;</li>"));
        assert!(page.contains("src=\"record-paused.png\""));
    }

    #[test]
    fn a_failed_shot_shows_no_image() {
        let binary = binary();
        let info = RunInfo {
            created_at_utc: "now",
            binary: &binary,
            head: None,
            dirty: false,
            source_fingerprint: None,
            version: None,
            qt_bin: None,
            sheets: vec![],
        };
        let page = html(&info, &[result(Status::Failed)]);
        assert!(!page.contains("<img"));
        assert!(!page.contains("Overlay sheets"));
    }
}

#[cfg(test)]
mod merge_tests {
    use super::*;

    fn shot(dir: &Path, name: &str, status: Status) -> ShotResult {
        let file = format!("{name}.png");
        std::fs::write(dir.join(&file), b"x").unwrap();
        ShotResult {
            name: name.to_string(),
            status,
            file,
            overlays: vec![],
            exit_code: Some(0),
            duration_ms: 1,
            warnings: vec![],
            log: String::new(),
            binary_sha256: String::new(),
            args: vec![],
            env: vec![],
        }
    }

    #[test]
    fn a_rerun_replaces_its_shots_and_keeps_the_others() {
        let dir = tempfile::tempdir().unwrap();
        let earlier = vec![
            shot(dir.path(), "a", Status::Ok),
            shot(dir.path(), "b", Status::Warn),
            shot(dir.path(), "gone", Status::Ok),
        ];
        std::fs::remove_file(dir.path().join("gone.png")).unwrap();
        let previous = json!({ "shots": earlier });
        let merged = merge(
            dir.path(),
            &previous,
            &[
                shot(dir.path(), "b", Status::Ok),
                shot(dir.path(), "c", Status::Ok),
            ],
        );
        let names: Vec<_> = merged.iter().map(|s| (s.name.as_str(), s.status)).collect();
        assert_eq!(
            names,
            [("a", Status::Ok), ("b", Status::Ok), ("c", Status::Ok)]
        );
    }
}
