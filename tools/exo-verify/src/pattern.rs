//! The deterministic verification pattern.
//!
//! One pure render function draws every frame of the stimulus window and of
//! the synthetic fixtures the oracles are tested against, so an oracle that
//! passes on a fixture reads exactly what the stimulus shows on a desktop.
//!
//! Layout, in fractions of the canvas so it survives capture scaling:
//! - rows 0..10%: frame-id barcode (sync blocks, 20 id bits, 4 check bits);
//! - rows 10..16%: state barcode (8 bits: source generation and flags);
//! - left column: reference colour patches;
//! - a flash square that is white on sync frames and black otherwise;
//! - a vertical motion bar whose position is the frame id;
//! - a uniform mid-grey field where cursor and overlay oracles look.

pub const ID_BITS: u32 = 20;
pub const CHECK_BITS: u32 = 4;
pub const SYNC_BLOCKS: u32 = 2;
pub const BLOCKS: u32 = SYNC_BLOCKS + ID_BITS + CHECK_BITS + SYNC_BLOCKS;
pub const STATE_BITS: u32 = 8;

/// Mid-grey of the oracle field (8-bit sRGB).
pub const FIELD: u8 = 128;
/// Background outside the field.
pub const BACKGROUND: u8 = 32;

/// Reference patches, sRGB 8-bit, top to bottom.
pub const PATCHES: [[u8; 3]; 6] = [
    [0, 0, 0],
    [255, 255, 255],
    [255, 0, 0],
    [0, 255, 0],
    [0, 0, 255],
    [128, 128, 128],
];

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Rect {
    pub x: u32,
    pub y: u32,
    pub w: u32,
    pub h: u32,
}

impl Rect {
    pub fn center(&self) -> (u32, u32) {
        (self.x + self.w / 2, self.y + self.h / 2)
    }
}

/// Where each element sits on a canvas of the given size.
#[derive(Debug, Clone, Copy)]
pub struct Layout {
    pub width: u32,
    pub height: u32,
}

impl Layout {
    pub fn new(width: u32, height: u32) -> Self {
        Layout { width, height }
    }

    fn frac(v: u32, num: u32, den: u32) -> u32 {
        (v as u64 * num as u64 / den as u64) as u32
    }

    pub fn id_block(&self, i: u32) -> Rect {
        let x0 = Self::frac(self.width, i, BLOCKS);
        let x1 = Self::frac(self.width, i + 1, BLOCKS);
        Rect {
            x: x0,
            y: 0,
            w: x1 - x0,
            h: Self::frac(self.height, 10, 100),
        }
    }

    pub fn state_block(&self, i: u32) -> Rect {
        let x0 = Self::frac(self.width, i, STATE_BITS * 2);
        let x1 = Self::frac(self.width, i + 1, STATE_BITS * 2);
        let y0 = Self::frac(self.height, 10, 100);
        Rect {
            x: x0,
            y: y0,
            w: x1 - x0,
            h: Self::frac(self.height, 16, 100) - y0,
        }
    }

    pub fn patch(&self, i: usize) -> Rect {
        let top = Self::frac(self.height, 20, 100);
        let h = Self::frac(self.height, 10, 100);
        Rect {
            x: Self::frac(self.width, 2, 100),
            y: top + i as u32 * h,
            w: Self::frac(self.width, 10, 100),
            h: h * 9 / 10,
        }
    }

    pub fn flash(&self) -> Rect {
        Rect {
            x: Self::frac(self.width, 85, 100),
            y: Self::frac(self.height, 20, 100),
            w: Self::frac(self.width, 12, 100),
            h: Self::frac(self.height, 15, 100),
        }
    }

    /// The uniform field cursor and overlay oracles search.
    pub fn field(&self) -> Rect {
        Rect {
            x: Self::frac(self.width, 16, 100),
            y: Self::frac(self.height, 20, 100),
            w: Self::frac(self.width, 66, 100),
            h: Self::frac(self.height, 55, 100),
        }
    }

    /// The band the motion bar travels, below the field.
    pub fn motion_band(&self) -> Rect {
        Rect {
            x: 0,
            y: Self::frac(self.height, 80, 100),
            w: self.width,
            h: Self::frac(self.height, 15, 100),
        }
    }

    pub fn motion_bar(&self, frame_id: u32) -> Rect {
        let band = self.motion_band();
        let bar_w = (self.width / 40).max(2);
        let travel = band.w.saturating_sub(bar_w).max(1);
        // One traversal per 120 frames.
        let x = (frame_id % 120) as u64 * travel as u64 / 119;
        Rect {
            x: x as u32,
            y: band.y,
            w: bar_w,
            h: band.h,
        }
    }
}

pub fn check_bits(id: u32) -> u32 {
    // CRC-4/ITU over the 20 id bits: a misread block changes the check value.
    let mut crc: u32 = 0;
    for i in (0..ID_BITS).rev() {
        let bit = (id >> i) & 1;
        let top = (crc >> 3) & 1;
        crc = (crc << 1) & 0xF;
        if top ^ bit == 1 {
            crc ^= 0x3;
        }
    }
    crc
}

/// Block values left to right: 1 = white, 0 = black.
pub fn id_blocks(id: u32) -> Vec<u8> {
    let id = id & ((1 << ID_BITS) - 1);
    let mut blocks = vec![1, 0];
    for i in (0..ID_BITS).rev() {
        blocks.push(((id >> i) & 1) as u8);
    }
    let check = check_bits(id);
    for i in (0..CHECK_BITS).rev() {
        blocks.push(((check >> i) & 1) as u8);
    }
    blocks.extend([0, 1]);
    blocks
}

#[derive(Debug, Clone, Copy, Default)]
pub struct FrameState {
    pub frame_id: u32,
    /// 0..=15; bumped when the stimulus changes source identity (recreated window).
    pub generation: u8,
    pub flash: bool,
    /// Bits 4..7 of the state barcode, free for scenario flags.
    pub flags: u8,
}

/// A black lane inside the field: an inverting cursor over it must turn white.
pub fn invert_lane(layout: &Layout) -> Rect {
    let f = layout.field();
    Rect {
        x: f.x + f.w / 20,
        y: f.y + f.h * 7 / 10,
        w: f.w * 9 / 10,
        h: f.h / 5,
    }
}

fn fill(px: &mut [u8], layout: &Layout, r: Rect, rgb: [u8; 3]) {
    let w = layout.width as usize;
    let pixel = [rgb[2], rgb[1], rgb[0], 255];
    for y in r.y as usize..(r.y + r.h).min(layout.height) as usize {
        let row = &mut px[y * w * 4..(y + 1) * w * 4];
        for x in r.x as usize..(r.x + r.w).min(layout.width) as usize {
            row[x * 4..x * 4 + 4].copy_from_slice(&pixel);
        }
    }
}

/// Renders one frame as tightly packed BGRA.
#[allow(dead_code, reason = "Used by media oracle tests only")]
pub fn render(layout: &Layout, state: &FrameState) -> Vec<u8> {
    let mut px = vec![0u8; layout.width as usize * layout.height as usize * 4];
    paint_static(layout, &mut px);
    paint_dynamic(layout, state, &mut px);
    px
}

/// Everything that never changes between frames.
pub fn paint_static(layout: &Layout, px: &mut [u8]) {
    fill(
        px,
        layout,
        Rect {
            x: 0,
            y: 0,
            w: layout.width,
            h: layout.height,
        },
        [BACKGROUND; 3],
    );
    for (i, rgb) in PATCHES.iter().enumerate() {
        fill(px, layout, layout.patch(i), *rgb);
    }
    fill(px, layout, layout.field(), [FIELD; 3]);
    fill(px, layout, invert_lane(layout), [0; 3]);
}

/// The regions `paint_dynamic` touches, for partial presentation.
pub fn dynamic_regions(layout: &Layout) -> [Rect; 3] {
    let bands = layout.state_block(0);
    [
        Rect {
            x: 0,
            y: 0,
            w: layout.width,
            h: bands.y + bands.h,
        },
        layout.flash(),
        layout.motion_band(),
    ]
}

/// The per-frame elements: barcodes, flash and motion bar.
pub fn paint_dynamic(layout: &Layout, state: &FrameState, px: &mut [u8]) {
    let mut fill = |r: Rect, rgb: [u8; 3]| fill(px, layout, r, rgb);
    for (i, v) in id_blocks(state.frame_id).iter().enumerate() {
        fill(
            layout.id_block(i as u32),
            if *v == 1 { [255; 3] } else { [0; 3] },
        );
    }
    let state_value = (state.generation as u32 & 0xF) | ((state.flags as u32 & 0xF) << 4);
    for i in 0..STATE_BITS {
        let bit = (state_value >> (STATE_BITS - 1 - i)) & 1;
        // Each state bit is a white/black pair so it decodes without a threshold guess.
        let (a, b) = if bit == 1 {
            ([255; 3], [0; 3])
        } else {
            ([0; 3], [255; 3])
        };
        fill(layout.state_block(i * 2), a);
        fill(layout.state_block(i * 2 + 1), b);
    }
    fill(layout.flash(), if state.flash { [255; 3] } else { [0; 3] });
    fill(layout.motion_band(), [0; 3]);
    fill(layout.motion_bar(state.frame_id), [255; 3]);
}

/// A luminance plane (0..255), `width` × `height`, as decoded from a recording.
pub struct Luma<'a> {
    pub width: u32,
    pub height: u32,
    pub data: &'a [u8],
}

impl Luma<'_> {
    pub fn at(&self, x: u32, y: u32) -> u8 {
        self.data[(y.min(self.height - 1) * self.width + x.min(self.width - 1)) as usize]
    }

    /// Mean over the central half of a rectangle, away from edges where
    /// scaling and chroma subsampling blur neighbouring blocks together.
    pub fn mean(&self, r: Rect) -> f64 {
        let (x0, y0) = (r.x + r.w / 4, r.y + r.h / 4);
        let (x1, y1) = (
            (r.x + r.w * 3 / 4).max(x0 + 1),
            (r.y + r.h * 3 / 4).max(y0 + 1),
        );
        let mut sum = 0u64;
        let mut n = 0u64;
        for y in y0..y1 {
            for x in x0..x1 {
                sum += self.at(x, y) as u64;
                n += 1;
            }
        }
        sum as f64 / n.max(1) as f64
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Decoded {
    /// A frame id whose check bits agree.
    Id(u32),
    /// The sync blocks are present but the id does not check: a damaged frame.
    Corrupt,
    /// No pattern at all: the stimulus is not what was captured.
    Absent,
}

/// Reads the frame id from a luminance plane of any size. The layout is
/// resolved against the plane's own dimensions, so capture scaling is free.
pub fn decode_id(luma: &Luma) -> Decoded {
    let layout = Layout::new(luma.width, luma.height);
    let levels: Vec<f64> = (0..BLOCKS).map(|i| luma.mean(layout.id_block(i))).collect();
    // Sync blocks give the white and black references for this frame.
    let white = (levels[0] + levels[(BLOCKS - 1) as usize]) / 2.0;
    let black = (levels[1] + levels[(BLOCKS - 2) as usize]) / 2.0;
    if white - black < 80.0 {
        return Decoded::Absent;
    }
    let threshold = (white + black) / 2.0;
    let bit = |i: u32| u32::from(levels[i as usize] > threshold);
    let mut id = 0;
    for i in 0..ID_BITS {
        id = (id << 1) | bit(SYNC_BLOCKS + i);
    }
    let mut check = 0;
    for i in 0..CHECK_BITS {
        check = (check << 1) | bit(SYNC_BLOCKS + ID_BITS + i);
    }
    if check == check_bits(id) {
        Decoded::Id(id)
    } else {
        Decoded::Corrupt
    }
}

#[allow(dead_code, reason = "Used by pattern tests and pending state checks")]
pub fn decode_state(luma: &Luma) -> Option<u8> {
    let layout = Layout::new(luma.width, luma.height);
    let mut value = 0u8;
    for i in 0..STATE_BITS {
        let a = luma.mean(layout.state_block(i * 2));
        let b = luma.mean(layout.state_block(i * 2 + 1));
        if (a - b).abs() < 60.0 {
            return None;
        }
        value = (value << 1) | u8::from(a > b);
    }
    Some(value)
}

pub fn flash_on(luma: &Luma) -> bool {
    luma.mean(Layout::new(luma.width, luma.height).flash()) > 128.0
}

/// BGRA to BT.709 limited-range-agnostic luma (full-range 0..255 output).
#[allow(dead_code, reason = "Used by pattern tests and pending screen checks")]
pub fn bgra_to_luma(bgra: &[u8]) -> Vec<u8> {
    bgra.chunks_exact(4)
        .map(|p| (0.0722 * p[0] as f64 + 0.7152 * p[1] as f64 + 0.2126 * p[2] as f64).round() as u8)
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn luma_of(layout: &Layout, state: &FrameState) -> Vec<u8> {
        bgra_to_luma(&render(layout, state))
    }

    #[test]
    fn ids_round_trip_at_several_sizes() {
        for (w, h) in [(1920, 1080), (1280, 720), (853, 480), (2560, 1440)] {
            let layout = Layout::new(w, h);
            for id in [0, 1, 59, 60, 1023, 777_777, (1 << ID_BITS) - 1] {
                let data = luma_of(
                    &layout,
                    &FrameState {
                        frame_id: id,
                        ..Default::default()
                    },
                );
                assert_eq!(
                    decode_id(&Luma {
                        width: w,
                        height: h,
                        data: &data
                    }),
                    Decoded::Id(id),
                    "{w}x{h} {id}"
                );
            }
        }
    }

    #[test]
    fn a_flipped_block_is_corrupt_not_another_id() {
        let layout = Layout::new(640, 360);
        let mut data = luma_of(
            &layout,
            &FrameState {
                frame_id: 4242,
                ..Default::default()
            },
        );
        let block = layout.id_block(SYNC_BLOCKS + 5);
        for y in block.y..block.y + block.h {
            for x in block.x..block.x + block.w {
                let i = (y * 640 + x) as usize;
                data[i] = 255 - data[i];
            }
        }
        assert_eq!(
            decode_id(&Luma {
                width: 640,
                height: 360,
                data: &data
            }),
            Decoded::Corrupt
        );
    }

    #[test]
    fn a_frame_without_the_pattern_is_absent() {
        let data = vec![90u8; 640 * 360];
        assert_eq!(
            decode_id(&Luma {
                width: 640,
                height: 360,
                data: &data
            }),
            Decoded::Absent
        );
    }

    #[test]
    fn state_and_flash_round_trip() {
        let layout = Layout::new(960, 540);
        let data = luma_of(
            &layout,
            &FrameState {
                frame_id: 3,
                generation: 9,
                flash: true,
                flags: 0b0101,
            },
        );
        let luma = Luma {
            width: 960,
            height: 540,
            data: &data,
        };
        assert_eq!(decode_state(&luma), Some(9 | (0b0101 << 4)));
        assert!(flash_on(&luma));
    }
}
