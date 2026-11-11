//! Full A/V decode and bounded, per-stream recording completeness evidence.

use anyhow::{Context as _, Result, ensure};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::BTreeMap;
use std::fs::File;
use std::io::{BufRead, BufReader};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::Duration;

use crate::tools;

const INSPECTION_TIMEOUT: Duration = Duration::from_secs(600);
const MAX_VIOLATIONS: usize = 32;

#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Expectation {
    pub video_streams: usize,
    pub audio_streams: usize,
    pub expected_seconds: f64,
    pub tolerance_ms: f64,
    pub max_gap_ms: f64,
    #[serde(default)]
    pub min_video_frames: u64,
}

impl Expectation {
    pub fn validate(&self) -> Result<()> {
        ensure!(
            self.video_streams > 0 || self.audio_streams > 0,
            "no expected A/V streams"
        );
        ensure!(
            self.expected_seconds.is_finite() && self.expected_seconds > 0.0,
            "expected_seconds must be finite and positive"
        );
        ensure!(
            self.tolerance_ms.is_finite()
                && self.tolerance_ms >= 0.0
                && self.tolerance_ms < self.expected_seconds * 1000.0,
            "tolerance_ms must be finite, nonnegative and smaller than the expected duration"
        );
        ensure!(
            self.max_gap_ms.is_finite() && self.max_gap_ms >= 0.0,
            "max_gap_ms must be finite and nonnegative"
        );
        Ok(())
    }
}

#[derive(Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DecodeReport {
    pub success: bool,
    pub errors: usize,
    pub first_error: Option<String>,
}

pub fn decode_all(file: &Path) -> Result<DecodeReport> {
    let output = tools::run(
        Command::new(tools::require("ffmpeg")?)
            .args([
                "-v",
                "error",
                "-nostdin",
                "-xerror",
                "-err_detect",
                "explode",
                "-i",
            ])
            .arg(file)
            .args(["-map", "0:v?", "-map", "0:a?", "-f", "null", "-"]),
        INSPECTION_TIMEOUT,
    )?;
    let mut errors = output.stderr.lines().filter(|line| !line.trim().is_empty());
    let first_error = errors.next().map(str::to_owned);
    let count = usize::from(first_error.is_some()) + errors.count();
    Ok(DecodeReport {
        success: output.success() && count == 0,
        errors: count,
        first_error,
    })
}

#[derive(Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct StreamEvidence {
    pub index: u64,
    pub kind: String,
    pub timestamp_resolution_seconds: f64,
    pub sample_rate: Option<u64>,
    pub packets: u64,
    pub frames: u64,
    pub samples: u64,
    pub initial_missing_dts: u64,
    pub first_pts_seconds: Option<f64>,
    pub end_pts_seconds: Option<f64>,
    pub maximum_gap_ms: f64,
    pub maximum_overlap_ms: f64,
}

#[derive(Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Report {
    pub expected: Expectation,
    pub decode: DecodeReport,
    pub timeline_checked: bool,
    pub completeness_checked: bool,
    pub streams: Vec<StreamEvidence>,
    pub violations: Vec<String>,
}

impl Report {
    pub fn passed(&self) -> bool {
        self.decode.success
            && self.timeline_checked
            && self.completeness_checked
            && self.violations.is_empty()
    }
}

struct StreamState {
    evidence: StreamEvidence,
    reorder_delay: u64,
    last_dts: Option<f64>,
    last_frame_pts: Option<f64>,
    last_frame_end: Option<f64>,
}

fn violation(issues: &mut Vec<String>, message: String) {
    if issues.len() < MAX_VIOLATIONS && !issues.contains(&message) {
        issues.push(message);
    }
}

fn number(fields: &BTreeMap<&str, &str>, name: &str) -> Result<f64> {
    let text = fields
        .get(name)
        .with_context(|| format!("missing {name}"))?;
    let value: f64 = text
        .parse()
        .with_context(|| format!("invalid {name}: {text}"))?;
    ensure!(value.is_finite(), "nonfinite {name}: {text}");
    Ok(value)
}

fn states(metadata: &Value) -> Result<BTreeMap<u64, StreamState>> {
    let mut streams = BTreeMap::new();
    for stream in metadata["streams"]
        .as_array()
        .context("no stream metadata")?
    {
        let kind = stream["codec_type"].as_str().unwrap_or("");
        if kind != "video" && kind != "audio" {
            continue;
        }
        let index = stream["index"].as_u64().context("stream has no index")?;
        let resolution = stream["time_base"]
            .as_str()
            .and_then(super::rate)
            .filter(|value| value.is_finite() && *value > 0.0)
            .context("invalid stream time base")?;
        let sample_rate = if kind == "audio" {
            Some(
                stream["sample_rate"]
                    .as_str()
                    .and_then(|text| text.parse::<u64>().ok())
                    .filter(|rate| *rate > 0)
                    .context("invalid audio sample rate")?,
            )
        } else {
            None
        };
        ensure!(
            !streams.contains_key(&index),
            "duplicate stream index {index}"
        );
        streams.insert(
            index,
            StreamState {
                evidence: StreamEvidence {
                    index,
                    kind: kind.into(),
                    timestamp_resolution_seconds: resolution,
                    sample_rate,
                    packets: 0,
                    frames: 0,
                    samples: 0,
                    initial_missing_dts: 0,
                    first_pts_seconds: None,
                    end_pts_seconds: None,
                    maximum_gap_ms: 0.0,
                    maximum_overlap_ms: 0.0,
                },
                reorder_delay: stream["has_b_frames"].as_u64().unwrap_or(0),
                last_dts: None,
                last_frame_pts: None,
                last_frame_end: None,
            },
        );
    }
    Ok(streams)
}

fn accept_row(state: &mut StreamState, section: &str, fields: &BTreeMap<&str, &str>) -> Result<()> {
    if section == "packet" {
        state.evidence.packets += 1;
        number(fields, "pts_time")?;
        if let Some(duration) = fields.get("duration_time")
            && *duration != "N/A"
        {
            ensure!(
                number(fields, "duration_time")? >= 0.0,
                "negative packet duration"
            );
        }
        // Matroska carries PTS, so FFmpeg cannot infer the first B-frame DTS
        // values. Permit only the declared leading reorder delay; never sort
        // the remaining packet timestamps or require B-frame packet PTS order.
        if fields.get("dts_time").is_none_or(|value| *value == "N/A")
            && state.last_dts.is_none()
            && state.evidence.packets <= state.reorder_delay
        {
            state.evidence.initial_missing_dts += 1;
            return Ok(());
        }
        let dts = number(fields, "dts_time")?;
        ensure!(
            state.last_dts.is_none_or(|previous| dts > previous),
            "packet DTS did not increase"
        );
        state.last_dts = Some(dts);
        return Ok(());
    }

    state.evidence.frames += 1;
    let pts = number(fields, "pts_time")?;
    ensure!(
        state.last_frame_pts.is_none_or(|previous| pts > previous),
        "decoded frame PTS did not increase"
    );
    let duration = if let Some(rate) = state.evidence.sample_rate {
        let samples = fields
            .get("nb_samples")
            .and_then(|text| text.parse::<u64>().ok())
            .filter(|count| *count > 0)
            .context("invalid decoded audio sample count")?;
        state.evidence.samples = state
            .evidence
            .samples
            .checked_add(samples)
            .context("audio sample count overflow")?;
        samples as f64 / rate as f64
    } else {
        let field = if fields.contains_key("duration_time") {
            "duration_time"
        } else {
            "pkt_duration_time"
        };
        let duration = number(fields, field)?;
        ensure!(duration > 0.0, "nonpositive decoded frame duration");
        duration
    };
    let end = pts + duration;
    ensure!(end.is_finite(), "nonfinite decoded frame end");
    if let Some(previous_end) = state.last_frame_end {
        let difference_ms = (pts - previous_end) * 1000.0;
        state.evidence.maximum_gap_ms = state.evidence.maximum_gap_ms.max(difference_ms);
        state.evidence.maximum_overlap_ms = state.evidence.maximum_overlap_ms.max(-difference_ms);
    }
    state.evidence.first_pts_seconds.get_or_insert(pts);
    state.evidence.end_pts_seconds = Some(end);
    state.last_frame_pts = Some(pts);
    state.last_frame_end = Some(end);
    Ok(())
}

fn scan(
    reader: impl BufRead,
    streams: &mut BTreeMap<u64, StreamState>,
    non_media: &[u64],
) -> Result<Vec<String>> {
    let mut issues = Vec::new();
    for line in reader.lines() {
        let line = line.context("read packet/frame timeline")?;
        if line.trim().is_empty() {
            continue;
        }
        let mut parts = line.split('|');
        let section = parts.next().unwrap();
        ensure!(
            section == "packet" || section == "frame",
            "unexpected timeline section {section}"
        );
        let mut fields = BTreeMap::new();
        for part in parts.filter(|part| !part.is_empty()) {
            let (key, value) = part.split_once('=').context("malformed timeline field")?;
            ensure!(
                fields.insert(key, value).is_none(),
                "duplicate timeline field {key}"
            );
        }
        let index = fields
            .get("stream_index")
            .and_then(|text| text.parse::<u64>().ok())
            .context("invalid timeline stream index")?;
        // Packet output also contains subtitles and attachments. Their decode
        // contracts are separate; all declared audio/video streams stay visible.
        if let Some(state) = streams.get_mut(&index) {
            if let Err(error) = accept_row(state, section, &fields) {
                violation(&mut issues, format!("stream {index}: {error:#}"));
            }
        } else {
            ensure!(
                non_media.contains(&index),
                "undeclared timeline stream {index}"
            );
        }
    }
    Ok(issues)
}

fn judge(streams: &[StreamEvidence], expected: &Expectation, issues: &mut Vec<String>) {
    for (kind, count) in [
        ("video", expected.video_streams),
        ("audio", expected.audio_streams),
    ] {
        let actual = streams.iter().filter(|stream| stream.kind == kind).count();
        if actual != count {
            violation(
                issues,
                format!("found {actual} {kind} streams, expected {count}"),
            );
        }
    }
    let first = streams
        .iter()
        .filter_map(|stream| stream.first_pts_seconds)
        .reduce(f64::min);
    let tolerance = expected.tolerance_ms / 1000.0;
    for stream in streams {
        let index = stream.index;
        let Some((start, end, origin)) = stream
            .first_pts_seconds
            .zip(stream.end_pts_seconds)
            .zip(first)
            .map(|((start, end), origin)| (start, end, origin))
        else {
            violation(issues, format!("stream {index}: no decoded timeline"));
            continue;
        };
        if stream.packets == 0 || stream.frames == 0 {
            violation(
                issues,
                format!("stream {index}: no packets or decoded frames"),
            );
        }
        if start - origin > tolerance {
            violation(
                issues,
                format!(
                    "stream {index}: starts {:.6}s after the recording",
                    start - origin
                ),
            );
        }
        if ((end - start) - expected.expected_seconds).abs() > tolerance {
            violation(
                issues,
                format!(
                    "stream {index}: decoded duration {:.6}s differs from expected {:.6}s",
                    end - start,
                    expected.expected_seconds
                ),
            );
        }
        if ((end - origin) - expected.expected_seconds).abs() > tolerance {
            violation(
                issues,
                format!(
                    "stream {index}: ends at {:.6}s instead of the expected {:.6}s recording duration",
                    end - origin,
                    expected.expected_seconds
                ),
            );
        }
        // Container timestamp rounding is limited by this stream's time base.
        let rounding_ms = stream.timestamp_resolution_seconds * 1000.0 + 0.001;
        if stream.maximum_gap_ms > expected.max_gap_ms.max(rounding_ms) {
            violation(
                issues,
                format!(
                    "stream {index}: decoded gap {:.6}ms exceeds {:.6}ms",
                    stream.maximum_gap_ms,
                    expected.max_gap_ms.max(rounding_ms)
                ),
            );
        }
        if stream.maximum_overlap_ms > rounding_ms {
            violation(
                issues,
                format!(
                    "stream {index}: decoded overlap {:.6}ms exceeds timestamp rounding",
                    stream.maximum_overlap_ms
                ),
            );
        }
        if let Some(rate) = stream.sample_rate {
            let decoded_seconds = stream.samples as f64 / rate as f64;
            if (decoded_seconds - expected.expected_seconds).abs() > tolerance {
                violation(
                    issues,
                    format!(
                        "stream {index}: sample duration {decoded_seconds:.6}s differs from expected {:.6}s",
                        expected.expected_seconds
                    ),
                );
            }
        } else if stream.frames < expected.min_video_frames {
            violation(
                issues,
                format!(
                    "stream {index}: {} decoded frames, expected at least {}",
                    stream.frames, expected.min_video_frames
                ),
            );
        }
    }
}

struct TimelineFile(PathBuf);

impl TimelineFile {
    fn create(path: PathBuf) -> Result<Self> {
        File::create_new(&path)?;
        Ok(Self(path))
    }
}

impl Drop for TimelineFile {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.0);
    }
}

pub fn inspect(file: &Path, scratch: &Path, expected: Expectation) -> Result<Report> {
    expected.validate()?;
    let decode = decode_all(file)?;
    let mut report = Report {
        expected,
        decode,
        timeline_checked: false,
        completeness_checked: false,
        streams: Vec::new(),
        violations: Vec::new(),
    };
    if !report.decode.success {
        report.violations.push(format!(
            "full A/V decode failed: {}",
            report
                .decode
                .first_error
                .as_deref()
                .unwrap_or("decoder exit status")
        ));
        return Ok(report);
    }
    let metadata = super::probe(file)?;
    let mut streams = states(&metadata)?;
    let non_media: Vec<u64> = metadata["streams"]
        .as_array()
        .unwrap()
        .iter()
        .filter_map(|stream| stream["index"].as_u64())
        .filter(|index| !streams.contains_key(index))
        .collect();
    std::fs::create_dir_all(scratch)?;
    let timeline = TimelineFile::create(scratch.join(format!(
        "{}.txt",
        crate::control::new_run_id("media-timeline")
    )))?;
    let output = tools::run(
        Command::new(tools::require("ffprobe")?)
            .args(["-v", "error", "-show_packets", "-show_frames", "-show_entries"])
            .arg("packet=stream_index,pts_time,dts_time,duration_time:frame=stream_index,pts_time,duration_time,pkt_duration_time,nb_samples:frame_side_data=:packet_side_data=")
            .args(["-of", "compact=p=1:nk=0", "-o"])
            .arg(&timeline.0).arg(file),
        INSPECTION_TIMEOUT,
    )?;
    ensure!(
        output.success(),
        "ffprobe timeline failed: {}",
        output.stderr.trim()
    );
    if !output.stderr.trim().is_empty() {
        violation(
            &mut report.violations,
            format!(
                "ffprobe timeline reported errors: {}",
                output.stderr.lines().next().unwrap_or("")
            ),
        );
    }
    // A multi-hour recording can produce millions of rows. Only this temporary
    // stream and fixed per-track accumulators grow; no frame/packet Vec is kept.
    report.violations.extend(scan(
        BufReader::new(File::open(&timeline.0)?),
        &mut streams,
        &non_media,
    )?);
    report.streams = streams
        .into_values()
        .map(|stream| stream.evidence)
        .collect();
    report.timeline_checked = true;
    judge(&report.streams, &report.expected, &mut report.violations);
    report.completeness_checked = true;
    Ok(report)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn failed_spool_creation_preserves_an_existing_file() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("timeline.txt");
        std::fs::write(&path, b"existing evidence").unwrap();
        assert!(TimelineFile::create(path.clone()).is_err());
        assert_eq!(std::fs::read(&path).unwrap(), b"existing evidence");
    }

    fn video_metadata() -> Value {
        json!({"streams": [{"index": 0, "codec_type": "video", "time_base": "1/1000", "has_b_frames": 2}]})
    }

    fn inspect_rows(metadata: Value, rows: &str) -> (Vec<StreamEvidence>, Vec<String>) {
        let mut streams = states(&metadata).unwrap();
        let issues = scan(std::io::Cursor::new(rows), &mut streams, &[]).unwrap();
        (
            streams
                .into_values()
                .map(|stream| stream.evidence)
                .collect(),
            issues,
        )
    }

    #[test]
    fn malformed_packet_times_are_not_discarded() {
        for invalid in ["N/A", "NaN", "inf", "-inf", "broken"] {
            let (_, issues) = inspect_rows(
                video_metadata(),
                &format!("packet|stream_index=0|pts_time={invalid}|dts_time=0.0\n"),
            );
            assert!(!issues.is_empty(), "accepted {invalid}");
        }
    }

    #[test]
    fn packet_dts_regressions_are_visible_in_original_order() {
        let (_, issues) = inspect_rows(
            video_metadata(),
            concat!(
                "packet|stream_index=0|pts_time=0.0|dts_time=0.1\n",
                "packet|stream_index=0|pts_time=0.1|dts_time=0.05\n",
            ),
        );
        assert!(
            issues.iter().any(|issue| issue.contains("DTS")),
            "{issues:?}"
        );
    }

    #[test]
    fn b_frame_pts_reordering_allows_only_leading_unknown_dts() {
        let (streams, issues) = inspect_rows(
            video_metadata(),
            concat!(
                "packet|stream_index=0|pts_time=0.0|dts_time=N/A\n",
                "packet|stream_index=0|pts_time=0.3|dts_time=N/A\n",
                "packet|stream_index=0|pts_time=0.1|dts_time=0.0\n",
                "packet|stream_index=0|pts_time=0.2|dts_time=0.1\n",
            ),
        );
        assert!(issues.is_empty(), "{issues:?}");
        assert_eq!(streams[0].initial_missing_dts, 2);
        for rows in [
            "packet|stream_index=0|pts_time=0.0|dts_time=0.0\npacket|stream_index=0|pts_time=0.1|dts_time=N/A\n",
            "packet|stream_index=0|pts_time=0.0|dts_time=N/A\npacket|stream_index=0|pts_time=0.3|dts_time=N/A\npacket|stream_index=0|pts_time=0.1|dts_time=N/A\n",
        ] {
            assert!(!inspect_rows(video_metadata(), rows).1.is_empty());
        }
    }

    #[test]
    fn decoded_frame_order_and_missing_pts_fail() {
        for tail in ["0.0", "-0.1", "N/A", "NaN"] {
            let (_, issues) = inspect_rows(
                video_metadata(),
                &format!(
                    "frame|stream_index=0|pts_time=0.0|duration_time=0.1\nframe|stream_index=0|pts_time={tail}|duration_time=0.1\n"
                ),
            );
            assert!(!issues.is_empty(), "accepted {tail}");
        }
    }

    #[test]
    fn sample_count_detects_holes_even_when_packet_span_is_long_enough() {
        let (streams, mut issues) = inspect_rows(
            json!({"streams": [{
                "index": 1, "codec_type": "audio", "time_base": "1/1000", "sample_rate": "48000"
            }]}),
            concat!(
                "packet|stream_index=1|pts_time=0.0|dts_time=0.0\n",
                "frame|stream_index=1|pts_time=0.0|nb_samples=4800\n",
                "packet|stream_index=1|pts_time=0.2|dts_time=0.2\n",
                "frame|stream_index=1|pts_time=0.2|nb_samples=4800\n",
            ),
        );
        judge(
            &streams,
            &Expectation {
                video_streams: 0,
                audio_streams: 1,
                expected_seconds: 0.3,
                tolerance_ms: 1.0,
                max_gap_ms: 150.0,
                min_video_frames: 0,
            },
            &mut issues,
        );
        assert!(
            issues.iter().any(|issue| issue.contains("sample duration")),
            "{issues:?}"
        );
    }

    #[test]
    fn per_track_tolerances_do_not_double_the_recording_end_budget() {
        let metadata = json!({"streams": [
            {"index": 0, "codec_type": "video", "time_base": "1/1000"},
            {"index": 1, "codec_type": "audio", "time_base": "1/1000", "sample_rate": "48000"},
        ]});
        let (streams, mut issues) = inspect_rows(
            metadata,
            concat!(
                "packet|stream_index=0|pts_time=0.0|dts_time=0.0\n",
                "frame|stream_index=0|pts_time=0.0|duration_time=1.0\n",
                "packet|stream_index=1|pts_time=0.024|dts_time=0.024\n",
                "frame|stream_index=1|pts_time=0.024|nb_samples=49152\n",
            ),
        );
        judge(
            &streams,
            &Expectation {
                video_streams: 1,
                audio_streams: 1,
                expected_seconds: 1.0,
                tolerance_ms: 25.0,
                max_gap_ms: 2.0,
                min_video_frames: 1,
            },
            &mut issues,
        );
        assert!(
            !issues.is_empty(),
            "a track ending 48ms late passed a 25ms recording budget"
        );
    }

    #[test]
    fn undeclared_timeline_stream_is_rejected() {
        let mut streams = states(&video_metadata()).unwrap();
        let row = "packet|stream_index=9|pts_time=0.0|dts_time=0.0\n";
        assert!(scan(std::io::Cursor::new(row), &mut streams, &[]).is_err());
        assert!(
            scan(std::io::Cursor::new(row), &mut streams, &[9])
                .unwrap()
                .is_empty()
        );
    }

    #[test]
    fn real_b_frame_media_keeps_decode_and_presentation_order_separate() {
        if !super::super::tests::ffmpeg_or_skip() {
            return;
        }
        let dir = tempfile::tempdir().unwrap();
        let file = dir.path().join("b-frames.mkv");
        let output = tools::run(
            Command::new(tools::require("ffmpeg").unwrap())
                .args([
                    "-v",
                    "error",
                    "-nostdin",
                    "-y",
                    "-f",
                    "lavfi",
                    "-i",
                    "testsrc2=size=64x48:rate=10:duration=1",
                ])
                .args(["-c:v", "libx264", "-bf", "2", "-g", "10"])
                .arg(&file),
            Duration::from_secs(30),
        )
        .unwrap();
        assert!(output.success(), "{}", output.stderr);
        let report = inspect(
            &file,
            dir.path(),
            Expectation {
                video_streams: 1,
                audio_streams: 0,
                expected_seconds: 1.0,
                tolerance_ms: 1.0,
                max_gap_ms: 1.0,
                min_video_frames: 10,
            },
        )
        .unwrap();
        assert!(report.passed(), "{:?}", report.violations);
        assert_eq!(report.streams[0].frames, 10);
        assert_eq!(report.streams[0].initial_missing_dts, 2);
    }
}
