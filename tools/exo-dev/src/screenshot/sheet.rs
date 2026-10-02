//! The overlay sheet: every capture-excluded window a HUD shot saved, composed
//! into one PNG per theme and size, one row per shot, on a checkerboard so
//! transparent edges stay visible.
//!
//! The app saves each overlay window as its own file because the windows sit
//! at their own desktop positions. The sheet does not reproduce those
//! positions. It is a side-by-side comparison, not a desktop mock-up.

use std::path::Path;

#[cfg(feature = "dev-tools")]
pub fn compose(out_dir: &Path, name: &str, rows: &[Vec<String>]) -> anyhow::Result<Option<String>> {
    use anyhow::Context;
    use tiny_skia::{Color, Paint, Pixmap, PixmapPaint, Rect, Transform};

    const PADDING: u32 = 24;
    const CHECKER: u32 = 16;

    let mut loaded: Vec<Vec<Pixmap>> = Vec::new();
    for row in rows {
        let mut images = Vec::new();
        for file in row {
            let path = out_dir.join(file);
            images.push(
                Pixmap::load_png(&path)
                    .with_context(|| format!("could not read {}", path.display()))?,
            );
        }
        if !images.is_empty() {
            loaded.push(images);
        }
    }
    if loaded.is_empty() {
        return Ok(None);
    }

    let row_width = |row: &[Pixmap]| row.iter().map(|p| p.width() + PADDING).sum::<u32>() + PADDING;
    let row_height = |row: &[Pixmap]| row.iter().map(Pixmap::height).max().unwrap_or(0);
    let width = loaded
        .iter()
        .map(|row| row_width(row))
        .max()
        .unwrap_or(PADDING);
    let height = loaded
        .iter()
        .map(|row| row_height(row) + PADDING)
        .sum::<u32>()
        + PADDING;
    let mut sheet = Pixmap::new(width, height).context("the overlay sheet is too large")?;

    sheet.fill(Color::from_rgba8(0x2A, 0x2A, 0x2E, 0xFF));
    let mut light = Paint::default();
    light.set_color_rgba8(0x36, 0x36, 0x3B, 0xFF);
    for y in (0..height).step_by(CHECKER as usize) {
        for x in (0..width).step_by(CHECKER as usize) {
            if (x / CHECKER + y / CHECKER) % 2 == 1
                && let Some(square) =
                    Rect::from_xywh(x as f32, y as f32, CHECKER as f32, CHECKER as f32)
            {
                sheet.fill_rect(square, &light, Transform::identity(), None);
            }
        }
    }

    let mut y = PADDING;
    for row in &loaded {
        let mut x = PADDING;
        for image in row {
            sheet.draw_pixmap(
                x as i32,
                y as i32,
                image.as_ref(),
                &PixmapPaint::default(),
                Transform::identity(),
                None,
            );
            x += image.width() + PADDING;
        }
        y += row_height(row) + PADDING;
    }

    let file = format!("{name}.png");
    sheet
        .save_png(out_dir.join(&file))
        .with_context(|| format!("could not write {file}"))?;
    Ok(Some(file))
}

/// Without the `dev-tools` feature there is no rasterizer, so no sheet.
#[cfg(not(feature = "dev-tools"))]
pub fn compose(
    _out_dir: &Path,
    _name: &str,
    _rows: &[Vec<String>],
) -> anyhow::Result<Option<String>> {
    Ok(None)
}

#[cfg(all(test, feature = "dev-tools"))]
mod tests {
    use super::*;
    use tiny_skia::{Color, Pixmap};

    fn png(dir: &Path, name: &str, width: u32, height: u32) -> String {
        let mut pixmap = Pixmap::new(width, height).unwrap();
        pixmap.fill(Color::from_rgba8(255, 0, 0, 255));
        pixmap.save_png(dir.join(name)).unwrap();
        name.to_string()
    }

    #[test]
    fn rows_are_laid_out_with_padding() {
        let dir = tempfile::tempdir().unwrap();
        let rows = vec![
            vec![
                png(dir.path(), "a.png", 100, 40),
                png(dir.path(), "b.png", 50, 60),
            ],
            vec![png(dir.path(), "c.png", 10, 10)],
        ];
        let file = compose(dir.path(), "sheet", &rows).unwrap().unwrap();
        let sheet = Pixmap::load_png(dir.path().join(file)).unwrap();
        assert_eq!(sheet.width(), 24 + 100 + 24 + 50 + 24);
        assert_eq!(sheet.height(), 24 + 60 + 24 + 10 + 24);
        let pixel = sheet.pixel(24, 24).unwrap();
        assert_eq!((pixel.red(), pixel.green(), pixel.blue()), (255, 0, 0));
    }

    #[test]
    fn no_overlays_means_no_sheet() {
        let dir = tempfile::tempdir().unwrap();
        assert!(compose(dir.path(), "sheet", &[vec![]]).unwrap().is_none());
    }
}
