//! Y4M reference-clip header parsing: just enough to get width, height and
//! frame rate for the per-encode bitrate/duration calculation.
//!
//! The header is a single ASCII text line, but everything after it is raw
//! binary frame data, so the file is always read in binary mode and only
//! the header line is decoded as text.

use std::path::Path;

use anyhow::Context as _;

pub struct Y4mHeader {
    pub width: u64,
    pub height: u64,
    pub fps_num: u64,
    pub fps_den: u64,
}

/// Parses a `YUV4MPEG2 W<width> H<height> F<num>:<den> ...` header line.
/// Fields are whitespace-separated tokens, each starting with a distinct
/// letter, so only the `W`, `H` and `F` tokens are inspected; unrecognized
/// fields (interlacing, aspect ratio, colorspace, a trailing comment) are
/// ignored.
pub fn parse_header_line(line: &str) -> anyhow::Result<Y4mHeader> {
    let mut width = None;
    let mut height = None;
    let mut fps = None;
    for token in line.split_whitespace() {
        if let Some(rest) = token.strip_prefix('W') {
            width = rest.parse().ok();
        } else if let Some(rest) = token.strip_prefix('H') {
            height = rest.parse().ok();
        } else if let Some(rest) = token.strip_prefix('F') {
            if let Some((num, den)) = rest.split_once(':') {
                if let (Ok(num), Ok(den)) = (num.parse(), den.parse()) {
                    fps = Some((num, den));
                }
            }
        }
    }
    let width = width.context("Y4M header has no W (width) field")?;
    let height = height.context("Y4M header has no H (height) field")?;
    let (fps_num, fps_den) = fps.context("Y4M header has no F (frame rate) field")?;
    Ok(Y4mHeader {
        width,
        height,
        fps_num,
        fps_den,
    })
}

/// The header line (without its trailing newline) and the byte length of
/// that line including the newline, needed to locate where frame data
/// starts.
pub fn read_header_line(path: &Path) -> anyhow::Result<(String, usize)> {
    let bytes =
        std::fs::read(path).with_context(|| format!("could not read {}", path.display()))?;
    let newline = bytes
        .iter()
        .position(|&b| b == b'\n')
        .context("Y4M file has no header line")?;
    let header_bytes = &bytes[..=newline];
    let text = std::str::from_utf8(header_bytes)
        .context("Y4M header is not valid ASCII/UTF-8")?
        .trim_end()
        .to_string();
    Ok((text, header_bytes.len()))
}

/// Frame count times frame interval, derived from the header plus a frame
/// count obtained by dividing the remaining payload size by the per-frame
/// I420 (4:2:0) size. No external demuxer: just the Y4M container's own
/// fixed frame layout (a `FRAME\n` marker followed by one raw I420 frame,
/// repeated).
pub fn clip_duration_seconds(path: &Path) -> anyhow::Result<f64> {
    let (header_line, header_len) = read_header_line(path)?;
    let header = parse_header_line(&header_line)?;
    let file_size = std::fs::metadata(path)
        .with_context(|| format!("could not read {}", path.display()))?
        .len();

    let frame_size = header.width * header.height + 2 * ((header.width / 2) * (header.height / 2));
    let frame_marker_len = "FRAME\n".len() as u64;
    let payload_size = file_size.saturating_sub(header_len as u64);
    let frame_count = payload_size / (frame_size + frame_marker_len);

    let fps = header.fps_num as f64 / header.fps_den as f64;
    Ok(frame_count as f64 / fps)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_width_height_and_frame_rate_from_a_header_line() {
        let header =
            parse_header_line("YUV4MPEG2 W1920 H1080 F30000:1001 Ip A1:1 C420mpeg2").unwrap();
        assert_eq!(header.width, 1920);
        assert_eq!(header.height, 1080);
        assert_eq!(header.fps_num, 30000);
        assert_eq!(header.fps_den, 1001);
    }

    #[test]
    fn a_header_missing_a_required_field_is_an_error() {
        assert!(parse_header_line("YUV4MPEG2 H1080 F30:1").is_err());
        assert!(parse_header_line("YUV4MPEG2 W1920 F30:1").is_err());
        assert!(parse_header_line("YUV4MPEG2 W1920 H1080").is_err());
    }

    fn write_synthetic_y4m(
        path: &Path,
        width: u64,
        height: u64,
        fps_num: u64,
        fps_den: u64,
        frames: u64,
    ) {
        let header =
            format!("YUV4MPEG2 W{width} H{height} F{fps_num}:{fps_den} Ip A1:1 C420jpeg\n");
        let frame_size = width * height + 2 * ((width / 2) * (height / 2));
        let mut bytes = header.into_bytes();
        for _ in 0..frames {
            bytes.extend_from_slice(b"FRAME\n");
            bytes.extend(std::iter::repeat_n(0u8, frame_size as usize));
        }
        std::fs::write(path, bytes).unwrap();
    }

    #[test]
    fn clip_duration_seconds_matches_frame_count_over_frame_rate() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("clip.y4m");
        write_synthetic_y4m(&path, 64, 32, 30, 1, 90);
        let duration = clip_duration_seconds(&path).unwrap();
        assert!((duration - 3.0).abs() < 1e-9, "got {duration}");
    }
}
