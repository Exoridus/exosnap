//! Media evidence read by independent tools. ffprobe and ffmpeg judge the
//! container and the decoded pixels; nothing here trusts the product's own
//! report of what it wrote.

use anyhow::{Context, Result, bail, ensure};
use serde_json::Value;
use std::path::Path;
use std::process::{Command, Stdio};
use std::time::Duration;

use crate::pattern::{self, Decoded, Luma};
use crate::tools;

pub fn probe(file: &Path) -> Result<Value> {
    let out = tools::run(
        Command::new(tools::require("ffprobe")?)
            .args([
                "-v",
                "error",
                "-show_format",
                "-show_streams",
                "-of",
                "json",
            ])
            .arg(file),
        Duration::from_secs(120),
    )?;
    ensure!(
        out.status.success(),
        "ffprobe failed on {}: {}",
        file.display(),
        out.stderr.trim()
    );
    Ok(serde_json::from_str(&out.stdout)?)
}

pub fn streams<'a>(probe: &'a Value, kind: &str) -> Vec<&'a Value> {
    probe["streams"]
        .as_array()
        .map(|s| s.iter().filter(|s| s["codec_type"] == kind).collect())
        .unwrap_or_default()
}

pub fn f64_field(v: &Value, key: &str) -> Option<f64> {
    v.get(key).and_then(|x| {
        x.as_str()
            .and_then(|s| s.parse().ok())
            .or_else(|| x.as_f64())
    })
}

/// `"60/1"` or `"30000/1001"` as frames per second.
pub fn rate(text: &str) -> Option<f64> {
    let (n, d) = text.split_once('/')?;
    let (n, d): (f64, f64) = (n.parse().ok()?, d.parse().ok()?);
    (d != 0.0).then_some(n / d)
}

#[derive(Debug, Clone, Copy)]
pub struct Packet {
    pub pts: f64,
    pub key: bool,
}

/// Video packets of the first video stream in presentation order.
pub fn video_packets(file: &Path) -> Result<Vec<Packet>> {
    let out = tools::run(
        Command::new(tools::require("ffprobe")?)
            .args([
                "-v",
                "error",
                "-select_streams",
                "v:0",
                "-show_entries",
                "packet=pts_time,flags",
                "-of",
                "csv=p=0",
            ])
            .arg(file),
        Duration::from_secs(300),
    )?;
    ensure!(
        out.status.success(),
        "ffprobe packets failed: {}",
        out.stderr.trim()
    );
    let mut packets: Vec<Packet> = out
        .stdout
        .lines()
        .filter_map(|l| {
            let mut parts = l.split(',');
            let pts = parts.next()?.trim().parse().ok()?;
            let flags = parts.next().unwrap_or("");
            Some(Packet {
                pts,
                key: flags.contains('K'),
            })
        })
        .collect();
    packets.sort_by(|a, b| a.pts.total_cmp(&b.pts));
    Ok(packets)
}

/// Decoded frame and demuxed packet counts of the first video stream.
pub fn frame_packet_counts(file: &Path) -> Result<(u64, u64)> {
    let out = tools::run(
        Command::new(tools::require("ffprobe")?)
            .args([
                "-v",
                "error",
                "-select_streams",
                "v:0",
                "-count_frames",
                "-count_packets",
                "-show_entries",
                "stream=nb_read_frames,nb_read_packets",
                "-of",
                "json",
            ])
            .arg(file),
        Duration::from_secs(600),
    )?;
    ensure!(
        out.status.success(),
        "ffprobe count failed: {}",
        out.stderr.trim()
    );
    let v: Value = serde_json::from_str(&out.stdout)?;
    let s = &v["streams"][0];
    let n = |k: &str| {
        s[k].as_str()
            .and_then(|x| x.parse().ok())
            .context(format!("ffprobe reported no {k}"))
    };
    Ok((n("nb_read_frames")?, n("nb_read_packets")?))
}

/// One decoded frame as a luminance plane scaled to `width`.
pub struct LumaFrame {
    pub pts: f64,
    pub width: u32,
    pub height: u32,
    pub data: Vec<u8>,
}

impl LumaFrame {
    pub fn luma(&self) -> Luma<'_> {
        Luma {
            width: self.width,
            height: self.height,
            data: &self.data,
        }
    }
}

/// Decodes every frame of the first video stream, scaled to `width` wide,
/// as 8-bit luma. `-fps_mode passthrough` keeps the decoder from inventing or
/// dropping frames, so the frame list is the stream's own.
pub fn luma_frames(file: &Path, width: u32) -> Result<Vec<LumaFrame>> {
    let info = probe(file)?;
    let video = streams(&info, "video")
        .into_iter()
        .next()
        .context("no video stream")?;
    let (sw, sh) = (
        video["width"].as_u64().context("width")? as u32,
        video["height"].as_u64().context("height")? as u32,
    );
    let height = ((sh as u64 * width as u64 / sw as u64) as u32) & !1;
    let pts = frame_pts(file)?;
    let mut child = Command::new(tools::require("ffmpeg")?)
        .args(["-v", "error", "-i"])
        .arg(file)
        .args(["-map", "0:v:0", "-fps_mode", "passthrough", "-vf"])
        .arg(format!("scale={width}:{height}:flags=area,format=gray"))
        .args(["-f", "rawvideo", "-"])
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .context("start ffmpeg")?;
    let mut raw = Vec::new();
    std::io::Read::read_to_end(child.stdout.as_mut().unwrap(), &mut raw)?;
    let status = tools::wait(&mut child, Duration::from_secs(600))?;
    ensure!(
        status.success(),
        "ffmpeg decode failed on {}",
        file.display()
    );
    let size = (width * height) as usize;
    ensure!(
        raw.len() % size == 0,
        "decoded byte count is not a whole number of frames"
    );
    let count = raw.len() / size;
    ensure!(
        count == pts.len(),
        "ffmpeg decoded {count} frames but ffprobe listed {} frame timestamps",
        pts.len()
    );
    Ok(raw
        .chunks_exact(size)
        .zip(pts)
        .map(|(d, pts)| LumaFrame {
            pts,
            width,
            height,
            data: d.to_vec(),
        })
        .collect())
}

fn frame_pts(file: &Path) -> Result<Vec<f64>> {
    let out = tools::run(
        Command::new(tools::require("ffprobe")?)
            .args([
                "-v",
                "error",
                "-select_streams",
                "v:0",
                "-show_entries",
                "frame=pts_time,best_effort_timestamp_time",
                "-of",
                "csv=p=0",
            ])
            .arg(file),
        Duration::from_secs(600),
    )?;
    ensure!(
        out.status.success(),
        "ffprobe frames failed: {}",
        out.stderr.trim()
    );
    Ok(out
        .stdout
        .lines()
        .filter_map(|l| l.split(',').find_map(|p| p.trim().parse::<f64>().ok()))
        .collect())
}

/// Decodes one audio stream to mono f32 at `rate` Hz.
pub fn audio_samples(file: &Path, stream: usize, rate: u32) -> Result<Vec<f32>> {
    let mut child = Command::new(tools::require("ffmpeg")?)
        .args(["-v", "error", "-i"])
        .arg(file)
        .args([
            "-map",
            &format!("0:a:{stream}"),
            "-ac",
            "1",
            "-ar",
            &rate.to_string(),
            "-f",
            "f32le",
            "-",
        ])
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()?;
    let mut raw = Vec::new();
    std::io::Read::read_to_end(child.stdout.as_mut().unwrap(), &mut raw)?;
    let status = tools::wait(&mut child, Duration::from_secs(600))?;
    ensure!(status.success(), "ffmpeg audio decode failed");
    Ok(raw
        .chunks_exact(4)
        .map(|b| f32::from_le_bytes([b[0], b[1], b[2], b[3]]))
        .collect())
}

/// What the frame-id barcode says about a recording's video timeline.
#[derive(Debug, Clone, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct IdTimeline {
    pub frames: usize,
    pub decoded: usize,
    pub corrupt: usize,
    pub absent: usize,
    pub unique: usize,
    pub first_id: Option<u32>,
    pub last_id: Option<u32>,
    /// Places where the id went backwards.
    pub reorders: usize,
    /// Longest run of output time over which the id did not change, seconds.
    pub longest_hold_s: f64,
    /// Largest jump between consecutive distinct ids.
    pub largest_id_step: u32,
    /// (pts, id) of every decoded frame, for timeline fitting.
    #[serde(skip)]
    pub samples: Vec<(f64, u32)>,
}

pub fn id_timeline(frames: &[LumaFrame]) -> IdTimeline {
    let mut t = IdTimeline {
        frames: frames.len(),
        decoded: 0,
        corrupt: 0,
        absent: 0,
        unique: 0,
        first_id: None,
        last_id: None,
        reorders: 0,
        longest_hold_s: 0.0,
        largest_id_step: 0,
        samples: Vec::new(),
    };
    let mut last: Option<(u32, f64)> = None;
    for f in frames {
        match pattern::decode_id(&f.luma()) {
            Decoded::Id(id) => {
                t.decoded += 1;
                t.samples.push((f.pts, id));
                t.first_id.get_or_insert(id);
                t.last_id = Some(id);
                match last {
                    Some((prev, since)) if id == prev => {
                        t.longest_hold_s = t.longest_hold_s.max(f.pts - since);
                    }
                    Some((prev, _)) => {
                        if id < prev {
                            t.reorders += 1;
                        } else {
                            t.largest_id_step = t.largest_id_step.max(id - prev);
                        }
                        t.unique += 1;
                        last = Some((id, f.pts));
                    }
                    None => {
                        t.unique += 1;
                        last = Some((id, f.pts));
                    }
                }
            }
            Decoded::Corrupt => t.corrupt += 1,
            Decoded::Absent => t.absent += 1,
        }
    }
    t
}

/// Least-squares slope and intercept of y over x.
pub fn fit(points: &[(f64, f64)]) -> Option<(f64, f64)> {
    let n = points.len() as f64;
    if points.len() < 2 {
        return None;
    }
    let (sx, sy) = points
        .iter()
        .fold((0.0, 0.0), |(a, b), (x, y)| (a + x, b + y));
    let (mx, my) = (sx / n, sy / n);
    let (mut sxx, mut sxy) = (0.0, 0.0);
    for (x, y) in points {
        sxx += (x - mx) * (x - mx);
        sxy += (x - mx) * (y - my);
    }
    if sxx == 0.0 {
        return None;
    }
    let slope = sxy / sxx;
    Some((slope, my - slope * mx))
}

/// Onsets of a tone at `freq` Hz: times where its Goertzel power rises above
/// `threshold` of full scale after at least `gap_s` of quiet.
pub fn tone_onsets(samples: &[f32], rate: u32, freq: f64, gap_s: f64) -> Vec<f64> {
    let window = (rate as usize / 200).max(32); // 5 ms
    let k = 2.0 * (2.0 * std::f64::consts::PI * freq / rate as f64).cos();
    let mut onsets = Vec::new();
    let mut quiet_since = 0.0f64;
    let mut active = false;
    for (i, chunk) in samples.chunks(window).enumerate() {
        let (mut s1, mut s2) = (0.0f64, 0.0f64);
        for &x in chunk {
            let s0 = x as f64 + k * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        let power = (s1 * s1 + s2 * s2 - k * s1 * s2).max(0.0).sqrt() * 2.0 / chunk.len() as f64;
        let t = (i * window) as f64 / rate as f64;
        if power > 0.05 {
            if !active && t - quiet_since >= gap_s {
                onsets.push(t);
            }
            active = true;
        } else {
            if active {
                quiet_since = t;
            }
            active = false;
        }
    }
    onsets
}

pub fn peak(samples: &[f32]) -> f32 {
    samples.iter().fold(0.0f32, |m, x| m.max(x.abs()))
}

/// Encodes BGRA frames into a video file with ffmpeg, for oracle fixtures.
pub fn encode_fixture(
    frames: impl Iterator<Item = Vec<u8>>,
    width: u32,
    height: u32,
    fps: u32,
    out: &Path,
    codec: &str,
) -> Result<()> {
    let mut child = Command::new(tools::require("ffmpeg")?)
        .args([
            "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "bgra", "-s",
        ])
        .arg(format!("{width}x{height}"))
        .args([
            "-r",
            &fps.to_string(),
            "-i",
            "-",
            "-c:v",
            codec,
            "-pix_fmt",
            "yuv420p",
        ])
        .arg(out)
        .stdin(Stdio::piped())
        .stdout(Stdio::null())
        .stderr(Stdio::piped())
        .spawn()?;
    {
        let mut stdin = child.stdin.take().unwrap();
        for f in frames {
            std::io::Write::write_all(&mut stdin, &f)?;
        }
    }
    let status = tools::wait(&mut child, Duration::from_secs(300))?;
    if !status.success() {
        bail!("ffmpeg fixture encode failed");
    }
    Ok(())
}

#[cfg(test)]
pub mod tests {
    use super::*;
    use crate::pattern::{FrameState, Layout, render};

    /// Oracle tests need real ffmpeg. They skip where it is absent unless
    /// EXO_VERIFY_REQUIRE_FFMPEG is set, which CI sets so a missing tool
    /// fails loudly instead of leaving the oracles untested.
    pub fn ffmpeg_or_skip() -> bool {
        let present = tools::resolve("ffmpeg").is_some() && tools::resolve("ffprobe").is_some();
        if !present && std::env::var_os("EXO_VERIFY_REQUIRE_FFMPEG").is_some() {
            panic!("EXO_VERIFY_REQUIRE_FFMPEG is set but ffmpeg/ffprobe are not available");
        }
        present
    }

    pub fn fixture(dir: &Path, ids: &[u32], fps: u32) -> std::path::PathBuf {
        let layout = Layout::new(640, 360);
        let out = dir.join("fixture.mkv");
        encode_fixture(
            ids.iter().map(|&id| {
                render(
                    &layout,
                    &FrameState {
                        frame_id: id,
                        ..Default::default()
                    },
                )
            }),
            640,
            360,
            fps,
            &out,
            "libx264",
        )
        .unwrap();
        out
    }

    #[test]
    fn a_clean_sequence_reads_back_exactly() {
        if !ffmpeg_or_skip() {
            return;
        }
        let dir = tempfile::tempdir().unwrap();
        let ids: Vec<u32> = (100..160).collect();
        let file = fixture(dir.path(), &ids, 30);
        let frames = luma_frames(&file, 320).unwrap();
        let t = id_timeline(&frames);
        assert_eq!(
            (t.decoded, t.corrupt, t.absent, t.unique, t.reorders),
            (60, 0, 0, 60, 0),
            "{t:?}"
        );
        assert_eq!(
            (t.first_id, t.last_id, t.largest_id_step),
            (Some(100), Some(159), 1)
        );
        let (frames_n, packets_n) = frame_packet_counts(&file).unwrap();
        assert_eq!((frames_n, packets_n), (60, 60));
    }

    #[test]
    fn dropped_held_and_reordered_frames_are_measured() {
        if !ffmpeg_or_skip() {
            return;
        }
        let dir = tempfile::tempdir().unwrap();
        let mut ids: Vec<u32> = (0..30).collect();
        ids.extend(std::iter::repeat_n(30, 20)); // a 20-frame hold
        ids.extend(45..60); // 14 ids never captured
        ids.push(50); // a reorder
        let file = fixture(dir.path(), &ids, 30);
        let t = id_timeline(&luma_frames(&file, 320).unwrap());
        assert_eq!(t.reorders, 1);
        assert_eq!(t.largest_id_step, 15);
        assert!(
            (t.longest_hold_s - 19.0 / 30.0).abs() < 0.01,
            "{}",
            t.longest_hold_s
        );
    }

    #[test]
    fn tone_onsets_find_beeps() {
        let rate = 48_000;
        let mut s = vec![0f32; rate as usize * 3];
        for start in [0.5f64, 1.5, 2.5] {
            let i0 = (start * rate as f64) as usize;
            for i in 0..(rate as usize / 20) {
                s[i0 + i] =
                    0.5 * (2.0 * std::f32::consts::PI * 1000.0 * i as f32 / rate as f32).sin();
            }
        }
        let onsets = tone_onsets(&s, rate, 1000.0, 0.3);
        assert_eq!(onsets.len(), 3, "{onsets:?}");
        for (o, e) in onsets.iter().zip([0.5, 1.5, 2.5]) {
            assert!((o - e).abs() < 0.006, "{o} vs {e}");
        }
    }

    #[test]
    fn fit_recovers_a_line() {
        let pts: Vec<(f64, f64)> = (0..100)
            .map(|i| (i as f64, 1.001 * i as f64 + 0.25))
            .collect();
        let (slope, intercept) = fit(&pts).unwrap();
        assert!((slope - 1.001).abs() < 1e-9 && (intercept - 0.25).abs() < 1e-9);
    }
}
