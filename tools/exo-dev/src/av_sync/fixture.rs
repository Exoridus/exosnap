//! Regenerates the golden clapper clip: synchronized flash and beep markers
//! spread evenly across a short synthetic clip, at ~0 drift on purpose (no
//! hardware emission path), so it is a stable regression anchor for the
//! av-sync analyzer.

use std::path::{Path, PathBuf};
use std::process::Stdio;

use anyhow::{Context as _, bail};

use crate::process;

const FLASH_SECONDS: f64 = 0.1;

/// Marker onsets, evenly spread with the last one ending inside the clip.
pub fn marker_times(duration: f64, markers: usize) -> anyhow::Result<Vec<f64>> {
    let last = duration - FLASH_SECONDS;
    if markers < 2 || last <= 0.0 {
        bail!("need at least two markers inside the clip");
    }
    let step = last / (markers - 1) as f64;
    Ok((0..markers)
        .map(|index| (index as f64 * step * 1000.0).round() / 1000.0)
        .collect())
}

/// The drawbox and volume enable expressions for a marker schedule.
///
/// Both are built from the same list, which is what keeps the flash and the
/// beep on one timeline: a fixture whose two enable expressions were
/// written separately would bake in exactly the skew the analyzer exists to
/// measure.
pub fn build_filters(times: &[f64]) -> (String, String) {
    let windows: Vec<String> = times
        .iter()
        .map(|start| format!("between(t,{start},{})", start + FLASH_SECONDS))
        .collect();
    let enable = windows.join("+");
    (
        format!("drawbox=t=fill:c=white:enable='{enable}'"),
        // Silence everywhere the markers are not.
        format!("volume=enable='not({enable})':volume=0"),
    )
}

pub struct FixtureSpec {
    pub duration: f64,
    pub markers: usize,
    pub out: PathBuf,
}

/// Bakes the fixture with system ffmpeg: full-frame white FLASH + 1 kHz BEEP
/// markers, silence and black in between.
pub fn generate(spec: &FixtureSpec) -> anyhow::Result<Vec<f64>> {
    let times = marker_times(spec.duration, spec.markers)?;
    if let Some(parent) = spec.out.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let (video_filter, audio_filter) = build_filters(&times);
    let out_str = spec.out.to_string_lossy().into_owned();
    let duration = spec.duration;
    let args: Vec<String> = vec![
        "-y".into(),
        "-hide_banner".into(),
        "-f".into(),
        "lavfi".into(),
        "-i".into(),
        format!("color=c=black:s=320x180:r=60:d={duration}"),
        "-f".into(),
        "lavfi".into(),
        "-i".into(),
        format!("sine=frequency=1000:sample_rate=48000:duration={duration}"),
        "-vf".into(),
        video_filter,
        "-af".into(),
        audio_filter,
        "-c:v".into(),
        "libx264".into(),
        "-pix_fmt".into(),
        "yuv420p".into(),
        "-b:v".into(),
        "150k".into(),
        "-c:a".into(),
        "aac".into(),
        "-b:a".into(),
        "64k".into(),
        "-shortest".into(),
        out_str,
    ];
    let mut command = process::command("ffmpeg");
    command
        .args(&args)
        .stdin(Stdio::null())
        .stdout(Stdio::null());
    let status = command.status().context("could not run ffmpeg")?;
    if !status.success() {
        bail!("ffmpeg failed with {status}");
    }
    Ok(times)
}

pub fn default_out(repo_root: &Path) -> PathBuf {
    repo_root
        .join("tests")
        .join("fixtures")
        .join("av-sync")
        .join("clapper-golden.mp4")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn marker_times_are_evenly_spread_across_the_clip() {
        let times = marker_times(4.0, 5).unwrap();
        assert_eq!(times.len(), 5);
        assert!((times[0] - 0.0).abs() < 1e-9);
        assert!((times[4] - 3.9).abs() < 1e-9);
        let step = times[1] - times[0];
        for window in times.windows(2) {
            assert!((window[1] - window[0] - step).abs() < 1e-9);
        }
    }

    #[test]
    fn marker_times_rejects_fewer_than_two_markers() {
        assert!(marker_times(4.0, 1).is_err());
    }

    #[test]
    fn marker_times_rejects_a_clip_too_short_for_one_flash() {
        assert!(marker_times(0.05, 2).is_err());
    }

    #[test]
    fn build_filters_shares_one_timeline_between_flash_and_beep() {
        let times = vec![0.0, 2.0];
        let (video, audio) = build_filters(&times);
        assert!(video.contains("between(t,0,0.1)"));
        assert!(video.contains("between(t,2,2.1)"));
        assert!(audio.contains("not(between(t,0,0.1)+between(t,2,2.1))"));
    }
}
