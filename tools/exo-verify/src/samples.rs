//! Deterministic sample regressions.
//!
//! A sample pairs a known input (a committed media file, or one generated
//! deterministically by ffmpeg) with oracles that judge observable properties
//! of it: it decodes cleanly, its timestamps advance, its keyframes are close
//! enough, its stream layout matches a reviewed reference, its audio and
//! video markers line up. Samples live under `tests/samples`, apart from the
//! code that judges them, and each manifest states the contract it protects.
//!
//! References are only ever rewritten by an explicit `--update-reference`,
//! which prints what changed.

use anyhow::{Context as _, Result, bail};
use clap::Subcommand;
use serde::Deserialize;
use serde_json::{Value, json};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode};
use std::time::{Duration, Instant};

use crate::capability::{self, Capability};
use crate::media;
use crate::model::{
    Identity, LaneResult, RESULT_SCHEMA_VERSION, ScenarioResult, Verdict, current_attempt,
    now_rfc3339, runner_version,
};
use crate::scenario::{ScenarioClass, Step, Stop};
use crate::tools;
use crate::{infra_ensure, product_ensure};

#[derive(Subcommand)]
pub enum SamplesCommand {
    /// List every sample manifest.
    List {
        #[arg(long, default_value = "tests/samples")]
        root: PathBuf,
    },
    /// Run samples. Patterns match ids exactly or by a trailing `*`.
    Run {
        patterns: Vec<String>,
        #[arg(long, default_value = "tests/samples")]
        root: PathBuf,
        /// Rewrite metadata references from the current inputs. A maintainer
        /// action; never pass it in CI.
        #[arg(long)]
        update_reference: bool,
        /// Write the results as a lane result document.
        #[arg(long)]
        out: Option<PathBuf>,
    },
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Sample {
    pub id: String,
    #[serde(default = "one")]
    pub revision: u32,
    pub class: String,
    pub contract: String,
    pub input: Input,
    #[serde(rename = "oracle")]
    pub oracles: OneOrMany<Oracle>,
    #[serde(default)]
    pub requirements: Requirements,
}

fn one() -> u32 {
    1
}

#[derive(Debug, Deserialize)]
#[serde(untagged)]
pub enum OneOrMany<T> {
    One(T),
    Many(Vec<T>),
}

impl<T> OneOrMany<T> {
    fn as_slice(&self) -> &[T] {
        match self {
            OneOrMany::One(t) => std::slice::from_ref(t),
            OneOrMany::Many(v) => v,
        }
    }
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Input {
    /// A committed file, relative to the samples root.
    pub path: Option<String>,
    /// ffmpeg arguments that deterministically generate the input; the
    /// output path is appended.
    pub generate: Option<Vec<String>>,
    /// Container extension of a generated input.
    #[serde(default = "mkv")]
    pub extension: String,
}

fn mkv() -> String {
    "mkv".into()
}

#[derive(Debug, Default, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Requirements {
    #[serde(default)]
    pub capabilities: Vec<String>,
}

#[derive(Debug, Deserialize)]
#[serde(tag = "kind", rename_all = "kebab-case", deny_unknown_fields)]
pub enum Oracle {
    /// The whole file decodes without a single decoder error.
    DecodeIntegrity {
        #[serde(default)]
        min_frames: u64,
    },
    /// Video presentation timestamps strictly increase and never leave a gap
    /// longer than `max_gap_ms`.
    Timestamps { max_gap_ms: f64 },
    /// A decoder joining anywhere waits at most `max_interval_s` for a
    /// keyframe.
    Keyframes { max_interval_s: f64 },
    /// Stream layout fields match `references/<id>.json`.
    Metadata { fields: Vec<String> },
    /// Full-frame flash and tone markers stay aligned.
    AvSync {
        min_markers: usize,
        max_offset_ms: f64,
        max_spread_ms: f64,
        #[serde(default = "khz")]
        tone_hz: f64,
    },
}

fn khz() -> f64 {
    1000.0
}

impl Oracle {
    fn name(&self) -> &'static str {
        match self {
            Oracle::DecodeIntegrity { .. } => "decode-integrity",
            Oracle::Timestamps { .. } => "timestamps",
            Oracle::Keyframes { .. } => "keyframes",
            Oracle::Metadata { .. } => "metadata",
            Oracle::AvSync { .. } => "av-sync",
        }
    }
}

pub fn load_all(root: &Path) -> Result<Vec<Sample>> {
    let dir = root.join("manifests");
    let mut files: Vec<PathBuf> = std::fs::read_dir(&dir)
        .with_context(|| format!("read {}", dir.display()))?
        .filter_map(|e| e.ok().map(|e| e.path()))
        .filter(|p| p.extension().is_some_and(|e| e == "toml"))
        .collect();
    files.sort();
    let mut samples: Vec<Sample> = Vec::new();
    for file in files {
        let text = std::fs::read_to_string(&file)?;
        let sample: Sample =
            toml::from_str(&text).with_context(|| format!("parse {}", file.display()))?;
        validate(&sample).with_context(|| format!("invalid sample {}", file.display()))?;
        if samples.iter().any(|s| s.id == sample.id) {
            bail!("duplicate sample id {}", sample.id);
        }
        samples.push(sample);
    }
    Ok(samples)
}

fn validate(sample: &Sample) -> Result<()> {
    if !sample
        .id
        .chars()
        .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '.' || c == '-')
        || !sample.id.contains('.')
    {
        bail!("id '{}' is not lowercase dotted", sample.id);
    }
    match ScenarioClass::parse(&sample.class) {
        Some(ScenarioClass::Regression | ScenarioClass::Contract) => {}
        _ => bail!("class '{}' is not regression or contract", sample.class),
    }
    if sample.contract.trim().is_empty() {
        bail!("a sample must state the contract it protects");
    }
    if sample.input.path.is_some() == sample.input.generate.is_some() {
        bail!("input needs exactly one of path or generate");
    }
    if sample.oracles.as_slice().is_empty() {
        bail!("a sample needs at least one oracle");
    }
    for c in &sample.requirements.capabilities {
        Capability::parse(c).with_context(|| format!("unknown capability '{c}'"))?;
    }
    Ok(())
}

fn matches(id: &str, patterns: &[String]) -> bool {
    patterns.is_empty()
        || patterns.iter().any(|p| match p.strip_suffix('*') {
            Some(prefix) => id.starts_with(prefix),
            None => id == p,
        })
}

struct Run<'a> {
    root: &'a Path,
    scratch: PathBuf,
    update_reference: bool,
    evidence: BTreeMap<String, Value>,
}

impl Run<'_> {
    fn put(&mut self, key: &str, value: impl Into<Value>) {
        self.evidence.insert(key.into(), value.into());
    }
}

fn input_file(run: &Run, sample: &Sample) -> Step<PathBuf> {
    if let Some(path) = &sample.input.path {
        let file = run.root.join(path);
        infra_ensure!(file.is_file(), "sample input {} is missing", file.display());
        return Ok(file);
    }
    let args = sample.input.generate.as_ref().unwrap();
    std::fs::create_dir_all(&run.scratch)?;
    let out = run
        .scratch
        .join(format!("{}.{}", sample.id, sample.input.extension));
    let result = tools::run(
        Command::new(tools::require("ffmpeg")?)
            .args(["-v", "error", "-y", "-nostdin"])
            .args(args)
            .arg(&out),
        Duration::from_secs(300),
    )?;
    infra_ensure!(
        result.success(),
        "generating the input failed: {}",
        result.stderr.trim()
    );
    Ok(out)
}

fn decode_integrity(run: &mut Run, file: &Path, min_frames: u64) -> Step {
    let result = tools::run(
        Command::new(tools::require("ffmpeg")?)
            .args(["-v", "error", "-nostdin", "-i"])
            .arg(file)
            .args(["-f", "null", "-"]),
        Duration::from_secs(600),
    )?;
    let errors: Vec<&str> = result
        .stderr
        .lines()
        .filter(|l| !l.trim().is_empty())
        .collect();
    run.put("decodeErrors", errors.len());
    product_ensure!(
        result.success() && errors.is_empty(),
        "decoding reported {} error(s), first: {}",
        errors.len(),
        errors.first().copied().unwrap_or("exit status")
    );
    let (frames, _) = media::frame_packet_counts(file)?;
    run.put("frames", frames);
    product_ensure!(
        frames >= min_frames,
        "{frames} frames decoded, at least {min_frames} expected"
    );
    Ok(())
}

fn timestamps(run: &mut Run, file: &Path, max_gap_ms: f64) -> Step {
    let out = tools::run(
        Command::new(tools::require("ffprobe")?)
            .args([
                "-v",
                "error",
                "-select_streams",
                "v:0",
                "-show_entries",
                "frame=pts_time",
                "-of",
                "csv=p=0",
            ])
            .arg(file),
        Duration::from_secs(300),
    )?;
    infra_ensure!(
        out.success(),
        "ffprobe frames failed: {}",
        out.stderr.trim()
    );
    let pts: Vec<f64> = out
        .stdout
        .lines()
        .filter_map(|l| l.trim().trim_end_matches(',').parse().ok())
        .collect();
    infra_ensure!(pts.len() >= 2, "fewer than two timestamped frames");
    let backwards = pts.windows(2).filter(|w| w[1] <= w[0]).count();
    let largest_gap = pts
        .windows(2)
        .map(|w| (w[1] - w[0]) * 1000.0)
        .fold(0.0f64, f64::max);
    run.put("frames", pts.len());
    run.put("nonIncreasing", backwards);
    run.put("largestGapMs", largest_gap);
    product_ensure!(
        backwards == 0,
        "{backwards} frame timestamps did not increase"
    );
    product_ensure!(
        largest_gap <= max_gap_ms,
        "a {largest_gap:.1} ms gap exceeds the {max_gap_ms} ms bound"
    );
    Ok(())
}

fn keyframes(run: &mut Run, file: &Path, max_interval_s: f64) -> Step {
    let packets = media::video_packets(file)?;
    let keys: Vec<f64> = packets.iter().filter(|p| p.key).map(|p| p.pts).collect();
    infra_ensure!(!packets.is_empty(), "no video packets");
    product_ensure!(!keys.is_empty(), "the stream has no keyframe");
    let end = packets.last().unwrap().pts;
    let mut bounds = keys.clone();
    bounds.push(end);
    let longest = bounds
        .windows(2)
        .map(|w| w[1] - w[0])
        .fold(0.0f64, f64::max);
    run.put("keyframes", keys.len());
    run.put("longestKeyframeIntervalS", longest);
    product_ensure!(
        longest <= max_interval_s,
        "a keyframe interval of {longest:.2} s exceeds {max_interval_s} s"
    );
    Ok(())
}

/// The reviewed layout of a file: the named fields of every stream, and of
/// the container under `format.*`.
fn layout(file: &Path, fields: &[String]) -> Result<Value> {
    let probe = media::probe(file)?;
    let mut streams = Vec::new();
    for stream in probe["streams"].as_array().cloned().unwrap_or_default() {
        let mut entry = serde_json::Map::new();
        for f in fields.iter().filter(|f| !f.starts_with("format.")) {
            entry.insert(f.clone(), stream.get(f).cloned().unwrap_or(Value::Null));
        }
        streams.push(Value::Object(entry));
    }
    let mut format = serde_json::Map::new();
    for f in fields.iter().filter_map(|f| f.strip_prefix("format.")) {
        format.insert(
            f.to_string(),
            probe["format"].get(f).cloned().unwrap_or(Value::Null),
        );
    }
    Ok(json!({"format": format, "streams": streams}))
}

fn metadata(run: &mut Run, sample: &Sample, file: &Path, fields: &[String]) -> Step {
    let actual = layout(file, fields)?;
    let reference = run
        .root
        .join("references")
        .join(format!("{}.json", sample.id));
    if run.update_reference {
        let previous = std::fs::read_to_string(&reference).ok();
        let text = format!("{}\n", serde_json::to_string_pretty(&actual)?);
        if previous.as_deref() != Some(text.as_str()) {
            std::fs::create_dir_all(reference.parent().unwrap())?;
            std::fs::write(&reference, &text)?;
            println!("updated reference {}:", reference.display());
            println!(
                "  was: {}",
                previous.as_deref().map(str::trim).unwrap_or("(none)")
            );
            println!("  now: {}", serde_json::to_string(&actual)?);
        }
        return Ok(());
    }
    let expected: Value =
        serde_json::from_str(&std::fs::read_to_string(&reference).map_err(|_| {
            Stop::infra(format!(
                "no reference {}; create it with --update-reference and review it",
                reference.display()
            ))
        })?)?;
    run.put("layout", actual.clone());
    product_ensure!(
        actual == expected,
        "stream layout differs from the reference: expected {expected}, got {actual}"
    );
    Ok(())
}

fn av_sync(
    run: &mut Run,
    file: &Path,
    min_markers: usize,
    max_offset_ms: f64,
    max_spread_ms: f64,
    tone_hz: f64,
) -> Step {
    let frames = media::luma_frames(file, 64)?;
    let lit: Vec<(f64, bool)> = frames
        .iter()
        .map(|f| {
            let mean = f.data.iter().map(|&v| v as f64).sum::<f64>() / f.data.len().max(1) as f64;
            (f.pts, mean > 128.0)
        })
        .collect();
    let flashes = crate::scenarios::common::flash_onsets(&lit);
    let audio = media::audio_samples(file, 0, 48_000)?;
    let tones = media::tone_onsets(&audio, 48_000, tone_hz, 0.25);
    run.put("flashes", json!(flashes));
    run.put("tones", json!(tones));
    infra_ensure!(
        flashes.len() >= min_markers,
        "{} flash markers found, the oracle needs {min_markers}",
        flashes.len()
    );
    let offsets: Vec<f64> = flashes
        .iter()
        .filter_map(|v| {
            tones
                .iter()
                .map(|a| (a - v) * 1000.0)
                .min_by(|a, b| a.abs().total_cmp(&b.abs()))
        })
        .filter(|o| o.abs() < 1000.0)
        .collect();
    product_ensure!(
        offsets.len() >= min_markers,
        "only {} of {} flashes have a matching tone",
        offsets.len(),
        flashes.len()
    );
    let mut sorted = offsets.clone();
    sorted.sort_by(f64::total_cmp);
    let median = sorted[sorted.len() / 2];
    let spread = sorted.last().unwrap() - sorted.first().unwrap();
    run.put("offsetMedianMs", median);
    run.put("offsetSpreadMs", spread);
    product_ensure!(
        median.abs() <= max_offset_ms,
        "audio is {median:.1} ms away from video"
    );
    product_ensure!(
        spread <= max_spread_ms,
        "the A/V offset moved by {spread:.1} ms across markers"
    );
    Ok(())
}

fn run_sample(run: &mut Run, sample: &Sample, caps: &capability::CapabilitySet) -> ScenarioResult {
    let started = Instant::now();
    let required: Vec<Capability> = sample
        .requirements
        .capabilities
        .iter()
        .filter_map(|c| Capability::parse(c))
        .collect();
    let missing = caps.missing(&required);
    let outcome: Step = if missing.is_empty() {
        (|| {
            let file = input_file(run, sample)?;
            for oracle in sample.oracles.as_slice() {
                let step = match oracle {
                    Oracle::DecodeIntegrity { min_frames } => {
                        decode_integrity(run, &file, *min_frames)
                    }
                    Oracle::Timestamps { max_gap_ms } => timestamps(run, &file, *max_gap_ms),
                    Oracle::Keyframes { max_interval_s } => keyframes(run, &file, *max_interval_s),
                    Oracle::Metadata { fields } => metadata(run, sample, &file, fields),
                    Oracle::AvSync {
                        min_markers,
                        max_offset_ms,
                        max_spread_ms,
                        tone_hz,
                    } => av_sync(
                        run,
                        &file,
                        *min_markers,
                        *max_offset_ms,
                        *max_spread_ms,
                        *tone_hz,
                    ),
                };
                step.map_err(|stop| match stop {
                    Stop::Fail(m) => Stop::Fail(format!("{}: {m}", oracle.name())),
                    Stop::Infra(e) => Stop::Infra(e.context(oracle.name())),
                    other => other,
                })?;
            }
            Ok(())
        })()
    } else {
        Err(Stop::unavailable(format!(
            "missing capability: {}",
            missing
                .iter()
                .map(|c| c.name())
                .collect::<Vec<_>>()
                .join(", ")
        )))
    };
    let (verdict, detail) = match outcome {
        Ok(()) => (Verdict::Pass, sample.contract.trim().to_string()),
        Err(Stop::Fail(m)) => (Verdict::Fail, m),
        Err(Stop::Unavailable(m)) => (Verdict::Unavailable, m),
        Err(Stop::Infra(e)) => (Verdict::InfraError, format!("{e:#}")),
    };
    ScenarioResult {
        id: sample.id.clone(),
        scenario_revision: sample.revision,
        verdict,
        detail,
        duration_ms: started.elapsed().as_millis() as u64,
        missing_capabilities: missing.iter().map(|c| c.name().to_string()).collect(),
        evidence: std::mem::take(&mut run.evidence),
        artifacts: vec![],
    }
}

pub fn run(command: SamplesCommand) -> Result<ExitCode> {
    match command {
        SamplesCommand::List { root } => {
            for s in load_all(&root)? {
                let kinds: Vec<&str> = s.oracles.as_slice().iter().map(Oracle::name).collect();
                println!(
                    "{:<44} r{} {:<10} [{}]",
                    s.id,
                    s.revision,
                    s.class,
                    kinds.join(",")
                );
            }
            Ok(ExitCode::SUCCESS)
        }
        SamplesCommand::Run {
            patterns,
            root,
            update_reference,
            out,
        } => {
            let samples = load_all(&root)?;
            let selected: Vec<&Sample> = samples
                .iter()
                .filter(|s| matches(&s.id, &patterns))
                .collect();
            if selected.is_empty() {
                bail!("no sample matches {patterns:?}");
            }
            let caps = capability::probe(&[]);
            let scratch = std::env::temp_dir().join(crate::control::new_run_id("exo-samples"));
            let mut run = Run {
                root: &root,
                scratch: scratch.clone(),
                update_reference,
                evidence: BTreeMap::new(),
            };
            let started_at = now_rfc3339();
            let mut results = Vec::new();
            for sample in selected {
                let result = run_sample(&mut run, sample, &caps);
                println!(
                    "{:<44} {:<12} {:>6.1}s  {}",
                    result.id,
                    result.verdict,
                    result.duration_ms as f64 / 1000.0,
                    result.detail
                );
                results.push(result);
            }
            let _ = std::fs::remove_dir_all(&scratch);
            let blocking = results
                .iter()
                .any(|r| matches!(r.verdict, Verdict::Fail | Verdict::InfraError));
            if let Some(path) = out {
                let document = LaneResult {
                    schema_version: RESULT_SCHEMA_VERSION,
                    lane: "samples".into(),
                    profile: "samples".into(),
                    runner_version: runner_version(),
                    started_at,
                    finished_at: now_rfc3339(),
                    attempt: current_attempt(),
                    identity: Identity::default(),
                    environment: caps.facts.clone(),
                    tools: BTreeMap::new(),
                    capabilities: caps.names(),
                    slot: Some(crate::disposable::default_slot(
                        crate::disposable::BackendKind::Local,
                        &caps,
                    )),
                    backend: Some("local".into()),
                    scenarios: results,
                };
                if let Some(parent) = path.parent() {
                    std::fs::create_dir_all(parent)?;
                }
                std::fs::write(&path, serde_json::to_vec_pretty(&document)?)?;
            }
            Ok(if blocking {
                ExitCode::from(1)
            } else {
                ExitCode::SUCCESS
            })
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn repo_samples() -> PathBuf {
        Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests/samples")
    }

    #[test]
    fn every_committed_manifest_is_valid() {
        let samples = load_all(&repo_samples()).unwrap();
        assert!(!samples.is_empty());
        for s in &samples {
            if let Some(path) = &s.input.path {
                assert!(
                    repo_samples().join(path).is_file(),
                    "{} input missing",
                    s.id
                );
            }
        }
    }

    #[test]
    fn a_manifest_without_a_contract_is_rejected() {
        let sample: Sample = toml::from_str(
            r#"
id = "media.x"
class = "regression"
contract = " "
[input]
path = "a.mkv"
[oracle]
kind = "keyframes"
max_interval_s = 2.0
"#,
        )
        .unwrap();
        assert!(validate(&sample).is_err());
    }

    #[test]
    fn oracles_may_be_one_table_or_an_array() {
        let sample: Sample = toml::from_str(
            r#"
id = "media.x"
class = "regression"
contract = "c"
[input]
generate = ["-f", "lavfi", "-i", "testsrc2=d=1"]
[[oracle]]
kind = "keyframes"
max_interval_s = 2.0
[[oracle]]
kind = "decode-integrity"
"#,
        )
        .unwrap();
        validate(&sample).unwrap();
        assert_eq!(sample.oracles.as_slice().len(), 2);
    }

    #[test]
    fn patterns_select_by_id_or_prefix() {
        assert!(matches("media.av-sync.a", &[]));
        assert!(matches("media.av-sync.a", &["media.*".into()]));
        assert!(!matches("timing.x", &["media.*".into()]));
        assert!(matches("timing.x", &["timing.x".into()]));
    }
}
