//! The preset/rate-control sweep: one [`MatrixCell`] per measurement point.

/// A cell's rate-control mode. NVENC's own two modes this tool sweeps.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RateControl {
    Cq,
    Vbr,
}

impl RateControl {
    pub fn as_str(&self) -> &'static str {
        match self {
            RateControl::Cq => "cq",
            RateControl::Vbr => "vbr",
        }
    }
}

/// One measurement point: a specific preset/rate-control/value combination.
#[derive(Clone, Debug)]
pub struct MatrixCell {
    /// "p1".."p7".
    pub preset: String,
    pub rc: RateControl,
    /// CQ value (for `Cq`) or bitrate in kbps (for `Vbr`).
    pub value: i64,
}

impl MatrixCell {
    pub fn label(&self) -> String {
        format!("{}-{}-{}", self.preset, self.rc.as_str(), self.value)
    }
}

/// The baseline sweep from the workflow doc: P4 and P7, each under CQ (the
/// product default) and VBR, at a spread of rate-control points wide
/// enough for a 4-point BD-rate curve fit.
pub fn default_matrix() -> Vec<MatrixCell> {
    let mut cells = Vec::new();
    for preset in ["p4", "p7"] {
        for cq in [19, 24, 30, 36] {
            cells.push(MatrixCell {
                preset: preset.to_string(),
                rc: RateControl::Cq,
                value: cq,
            });
        }
        for kbps in [3000, 6000, 12000, 24000] {
            cells.push(MatrixCell {
                preset: preset.to_string(),
                rc: RateControl::Vbr,
                value: kbps,
            });
        }
    }
    cells
}

/// A caller-chosen sweep, for calibrating a product decision rather than
/// re-running the baseline.
///
/// Deliberately separate from [`default_matrix`]: the baseline exists so
/// two runs months apart are comparable, and a product sweep that widened
/// it in place would silently redefine what "the matrix" means. A sweep
/// built here is also free to carry more than four points per curve.
/// [`super::bd_rate::bd_rate`] still needs exactly four, so an exploration
/// sweep and a BD-rate comparison are two different reads of the same CSV,
/// not one command.
pub fn explicit_matrix(
    presets: &[String],
    cq_values: &[i64],
    vbr_values: &[i64],
) -> Vec<MatrixCell> {
    let mut cells = Vec::new();
    for preset in presets {
        for &cq in cq_values {
            cells.push(MatrixCell {
                preset: preset.clone(),
                rc: RateControl::Cq,
                value: cq,
            });
        }
        for &kbps in vbr_values {
            cells.push(MatrixCell {
                preset: preset.clone(),
                rc: RateControl::Vbr,
                value: kbps,
            });
        }
    }
    cells
}

/// Parses a comma-separated list of integers, trimming whitespace and
/// skipping empty entries (so a trailing comma is harmless). `what` names
/// the flag in the returned error, matching the CLI's own error text.
pub fn parse_int_list(text: &str, what: &str) -> anyhow::Result<Vec<i64>> {
    let mut values = Vec::new();
    for part in text.split(',') {
        let part = part.trim();
        if part.is_empty() {
            continue;
        }
        let value: i64 = part
            .parse()
            .map_err(|_| anyhow::anyhow!("{what}: '{part}' is not an integer"))?;
        values.push(value);
    }
    Ok(values)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn default_matrix_has_two_presets_of_four_cq_and_four_vbr_points() {
        let cells = default_matrix();
        assert_eq!(cells.len(), 16);
        assert_eq!(cells[0].label(), "p4-cq-19");
        assert_eq!(cells[3].label(), "p4-cq-36");
        assert_eq!(cells[4].label(), "p4-vbr-3000");
        assert_eq!(cells[8].label(), "p7-cq-19");
    }

    #[test]
    fn explicit_matrix_covers_every_preset_and_value_combination() {
        let presets = vec!["p1".to_string(), "p6".to_string()];
        let cells = explicit_matrix(&presets, &[16, 20], &[5000]);
        let labels: Vec<String> = cells.iter().map(MatrixCell::label).collect();
        assert_eq!(
            labels,
            vec![
                "p1-cq-16",
                "p1-cq-20",
                "p1-vbr-5000",
                "p6-cq-16",
                "p6-cq-20",
                "p6-vbr-5000"
            ]
        );
    }

    #[test]
    fn parse_int_list_trims_and_skips_empty_entries() {
        assert_eq!(
            parse_int_list(" 16, 20,,24 ", "--cq-values").unwrap(),
            vec![16, 20, 24]
        );
        assert_eq!(
            parse_int_list("", "--cq-values").unwrap(),
            Vec::<i64>::new()
        );
    }

    #[test]
    fn parse_int_list_rejects_a_non_integer_entry() {
        let error = parse_int_list("16,abc", "--cq-values").unwrap_err();
        assert!(
            error
                .to_string()
                .contains("--cq-values: 'abc' is not an integer")
        );
    }
}
