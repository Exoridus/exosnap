//! Pure argv construction for the external `probe_encode_file` and `ffmpeg`
//! calls this tool drives, kept separate from actually running them so the
//! exact arguments are unit-testable without the real binaries.

use anyhow::bail;

use super::matrix::{MatrixCell, RateControl};

/// Both inputs are forced to the same colour description before scoring.
/// The encodes carry a real colour description (the encoder writes BT.709
/// into the bitstream) while a Y4M reference carries none, and ffmpeg then
/// auto-inserts a colour conversion on one input only. Scoring that
/// conversion cost 14 dB of PSNR-Y and 4.3 VMAF on a 1440p60 AV1 encode
/// whose pixels were in fact untouched, large enough to invert a
/// comparison. VMAF, SSIM and PSNR are all defined on raw sample values, so
/// declaring both sides unspecified limited-range is the correct
/// normalization, not a workaround.
///
/// Both inputs are also re-stamped by frame index, which is what "compare
/// frame i against frame i" actually means. The metric filters pair frames
/// by presentation time, and a muxed candidate carries container
/// timestamps a raw Y4M reference does not: Matroska quantizes to its 1 ms
/// timecode scale, so 60 fps lands on 0/16/33/50 ms while the Y4M sits on
/// exact 1/60 s. The two sets never coincide, framesync then duplicates and
/// mispairs, and a bit-exact lossless copy scores PSNR-Y 21 dB with a third
/// of its frames at VMAF 0. Rebasing to PTS-STARTPTS on a shared timebase
/// is not enough, because the quantization is inside the sequence, not at
/// its start. Elementary streams happen not to need any of this, which is
/// exactly why it has to be unconditional.
pub const METRIC_INPUT_NORMALISATION: &str =
    "settb=1/1,setpts=N,setparams=range=1:color_primaries=2:color_trc=2:colorspace=2";

/// The argv for one `probe_encode_file` invocation for `cell`.
pub fn probe_encode_argv(
    probe: &str,
    y4m: &str,
    out: &str,
    vcodec: &str,
    cell: &MatrixCell,
) -> Vec<String> {
    let mut argv = vec![
        probe.to_string(),
        "--y4m".to_string(),
        y4m.to_string(),
        "--out".to_string(),
        out.to_string(),
        "--vcodec".to_string(),
        vcodec.to_string(),
        "--preset".to_string(),
        cell.preset.clone(),
        "--rc".to_string(),
        cell.rc.as_str().to_string(),
    ];
    match cell.rc {
        RateControl::Cq => {
            argv.push("--cq".to_string());
            argv.push(cell.value.to_string());
        }
        RateControl::Vbr => {
            argv.push("--bitrate".to_string());
            argv.push(cell.value.to_string());
        }
    }
    argv
}

/// The `-filter_complex` value for `measure_quality_argv`.
///
/// `vmaf_log_name` is passed as a bare filename, not a full path, because
/// the libvmaf filter's own option string splits on ':', which collides
/// with the colon after a Windows drive letter in an absolute path. No
/// amount of escaping avoids that collision, so the caller passes a
/// relative filename and runs ffmpeg with its working directory set to
/// that file's directory instead.
pub fn measure_quality_filter_arg(vmaf_log_name: &str) -> String {
    let norm = METRIC_INPUT_NORMALISATION;
    format!(
        "[0:v]{norm}[dist];[1:v]{norm}[ref];[dist][ref]libvmaf=log_path={vmaf_log_name}:log_fmt=json:feature=name=psnr|name=float_ssim"
    )
}

/// The argv for the ffmpeg call that scores `distorted_path` against
/// `reference_path`.
///
/// Input order matters: libvmaf's `main`/`main2` streams follow the
/// `-lavfi`/`-filter_complex` convention of `[0:v]` as the distorted signal
/// and `[1:v]` as the reference, so `distorted_path` must be given as `-i`
/// before `reference_path`, matching the order here.
pub fn measure_quality_argv(
    ffmpeg_path: &str,
    distorted_path: &str,
    reference_path: &str,
    filter_arg: &str,
) -> Vec<String> {
    vec![
        ffmpeg_path.to_string(),
        "-hide_banner".to_string(),
        "-i".to_string(),
        distorted_path.to_string(),
        "-i".to_string(),
        reference_path.to_string(),
        "-filter_complex".to_string(),
        filter_arg.to_string(),
        "-f".to_string(),
        "null".to_string(),
        "-".to_string(),
    ]
}

pub fn ffmpeg_filters_argv(ffmpeg_path: &str) -> Vec<String> {
    vec![ffmpeg_path.to_string(), "-filters".to_string()]
}

pub fn ffmpeg_version_argv(ffmpeg_path: &str) -> Vec<String> {
    vec![ffmpeg_path.to_string(), "-version".to_string()]
}

/// Whether `ffmpeg -filters` output lists the libvmaf filter.
pub fn ffmpeg_output_has_libvmaf(filters_stdout: &str) -> bool {
    filters_stdout.contains("libvmaf")
}

/// Runs `ffmpeg -filters` and fails with a caller-facing message unless the
/// build was compiled with `--enable-libvmaf`.
pub fn check_ffmpeg_has_libvmaf(ffmpeg_path: &str) -> anyhow::Result<()> {
    let argv = ffmpeg_filters_argv(ffmpeg_path);
    let output = std::process::Command::new(&argv[0])
        .args(&argv[1..])
        .output();
    let stdout = match output {
        Ok(output) => String::from_utf8_lossy(&output.stdout).into_owned(),
        Err(error) => bail!("could not run '{ffmpeg_path} -filters': {error}"),
    };
    if !ffmpeg_output_has_libvmaf(&stdout) {
        bail!(
            "'{ffmpeg_path} -filters' does not list libvmaf. This ffmpeg build was not compiled \
             with --enable-libvmaf. See docs/dev/encoder-quality-matrix.md for where to get one."
        );
    }
    Ok(())
}

pub fn sanity_lossless_argv(ffmpeg_path: &str, clip_path: &str, out_path: &str) -> Vec<String> {
    [
        ffmpeg_path,
        "-hide_banner",
        "-loglevel",
        "error",
        "-i",
        clip_path,
        "-c:v",
        "ffv1",
        "-y",
        out_path,
    ]
    .into_iter()
    .map(str::to_string)
    .collect()
}

pub fn sanity_libx264_argv(
    ffmpeg_path: &str,
    clip_path: &str,
    crf: &str,
    out_path: &str,
) -> Vec<String> {
    [
        ffmpeg_path,
        "-hide_banner",
        "-loglevel",
        "error",
        "-i",
        clip_path,
        "-c:v",
        "libx264",
        "-preset",
        "veryfast",
        "-crf",
        crf,
        "-y",
        out_path,
    ]
    .into_iter()
    .map(str::to_string)
    .collect()
}

/// One-frame temporal offset: drop the first frame, so every compared pair
/// is off by one. Lossless again, to keep the offset the only difference.
pub fn sanity_offset_argv(ffmpeg_path: &str, clip_path: &str, out_path: &str) -> Vec<String> {
    [
        ffmpeg_path,
        "-hide_banner",
        "-loglevel",
        "error",
        "-i",
        clip_path,
        "-vf",
        "trim=start_frame=1,setpts=PTS-STARTPTS",
        "-c:v",
        "ffv1",
        "-y",
        out_path,
    ]
    .into_iter()
    .map(str::to_string)
    .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn probe_encode_argv_matches_the_ported_script_for_cq() {
        let cell = MatrixCell {
            preset: "p4".to_string(),
            rc: RateControl::Cq,
            value: 24,
        };
        assert_eq!(
            probe_encode_argv("probe.exe", "clip.y4m", "out.h264", "h264", &cell),
            vec![
                "probe.exe",
                "--y4m",
                "clip.y4m",
                "--out",
                "out.h264",
                "--vcodec",
                "h264",
                "--preset",
                "p4",
                "--rc",
                "cq",
                "--cq",
                "24",
            ]
        );
    }

    #[test]
    fn probe_encode_argv_matches_the_ported_script_for_vbr() {
        let cell = MatrixCell {
            preset: "p7".to_string(),
            rc: RateControl::Vbr,
            value: 12000,
        };
        assert_eq!(
            probe_encode_argv("probe.exe", "clip.y4m", "out.ivf", "av1", &cell),
            vec![
                "probe.exe",
                "--y4m",
                "clip.y4m",
                "--out",
                "out.ivf",
                "--vcodec",
                "av1",
                "--preset",
                "p7",
                "--rc",
                "vbr",
                "--bitrate",
                "12000",
            ]
        );
    }

    #[test]
    fn measure_quality_argv_puts_the_distorted_input_before_the_reference() {
        let filter_arg = measure_quality_filter_arg("label.vmaf.json");
        assert!(filter_arg.contains("log_path=label.vmaf.json"));
        assert!(filter_arg.contains("feature=name=psnr|name=float_ssim"));
        let argv = measure_quality_argv("ffmpeg", "dist.h264", "ref.y4m", &filter_arg);
        assert_eq!(
            argv,
            vec![
                "ffmpeg",
                "-hide_banner",
                "-i",
                "dist.h264",
                "-i",
                "ref.y4m",
                "-filter_complex",
                &filter_arg,
                "-f",
                "null",
                "-",
            ]
        );
    }

    #[test]
    fn ffmpeg_output_has_libvmaf_matches_a_literal_substring_search() {
        assert!(ffmpeg_output_has_libvmaf(
            "V..C libvmaf  Calculate the VMAF between two video streams."
        ));
        assert!(!ffmpeg_output_has_libvmaf(
            "V..C scale Scale the input video size and/or convert the pixel format."
        ));
    }

    #[test]
    fn sanity_argv_matches_the_ported_script() {
        assert_eq!(
            sanity_lossless_argv("ffmpeg", "clip.y4m", "identity.mkv"),
            vec![
                "ffmpeg",
                "-hide_banner",
                "-loglevel",
                "error",
                "-i",
                "clip.y4m",
                "-c:v",
                "ffv1",
                "-y",
                "identity.mkv",
            ]
        );
        assert_eq!(
            sanity_libx264_argv("ffmpeg", "clip.y4m", "20", "mild.mkv"),
            vec![
                "ffmpeg",
                "-hide_banner",
                "-loglevel",
                "error",
                "-i",
                "clip.y4m",
                "-c:v",
                "libx264",
                "-preset",
                "veryfast",
                "-crf",
                "20",
                "-y",
                "mild.mkv",
            ]
        );
        assert_eq!(
            sanity_offset_argv("ffmpeg", "clip.y4m", "offset.mkv"),
            vec![
                "ffmpeg",
                "-hide_banner",
                "-loglevel",
                "error",
                "-i",
                "clip.y4m",
                "-vf",
                "trim=start_frame=1,setpts=PTS-STARTPTS",
                "-c:v",
                "ffv1",
                "-y",
                "offset.mkv",
            ]
        );
    }

    #[test]
    fn check_ffmpeg_has_libvmaf_reports_a_missing_program() {
        let error = check_ffmpeg_has_libvmaf("exo-dev-no-such-ffmpeg").unwrap_err();
        assert!(error.to_string().contains("could not run"));
    }
}
