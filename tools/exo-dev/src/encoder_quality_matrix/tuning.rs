use clap::Args;

#[derive(Args, Debug, Default, serde::Serialize)]
pub struct TuningArgs {
    #[arg(long, default_value_t = 0, value_parser = clap::value_parser!(u32).range(0..=31))]
    pub bframes: u32,
    #[arg(long, default_value = "off", value_parser = ["off", "each", "middle"])]
    pub b_ref: String,
    #[arg(long)]
    pub lookahead: bool,
    #[arg(long, default_value_t = 16, value_parser = clap::value_parser!(u32).range(1..=31))]
    pub lookahead_depth: u32,
    #[arg(long)]
    pub spatial_aq: bool,
    #[arg(long)]
    pub temporal_aq: bool,
    #[arg(long, default_value = "single", value_parser = ["single", "quarter", "full"])]
    pub multipass: String,
}

impl TuningArgs {
    pub fn append_argv(&self, argv: &mut Vec<String>) {
        argv.extend(["--bframes".into(), self.bframes.to_string()]);
        if !self.b_ref.is_empty() {
            argv.extend(["--b-ref".into(), self.b_ref.clone()]);
        }
        if self.lookahead {
            argv.extend([
                "--lookahead".into(),
                "--lookahead-depth".into(),
                self.lookahead_depth.to_string(),
            ]);
        }
        if self.spatial_aq {
            argv.push("--spatial-aq".into());
        }
        if self.temporal_aq {
            argv.push("--temporal-aq".into());
        }
        if !self.multipass.is_empty() {
            argv.extend(["--multipass".into(), self.multipass.clone()]);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn scout_arguments_preserve_explicit_feature_intent() {
        let tuning = TuningArgs {
            bframes: 3,
            b_ref: "middle".into(),
            lookahead: true,
            lookahead_depth: 16,
            spatial_aq: true,
            temporal_aq: true,
            multipass: "full".into(),
        };
        let mut args = Vec::new();
        tuning.append_argv(&mut args);
        assert_eq!(
            args,
            [
                "--bframes",
                "3",
                "--b-ref",
                "middle",
                "--lookahead",
                "--lookahead-depth",
                "16",
                "--spatial-aq",
                "--temporal-aq",
                "--multipass",
                "full"
            ]
        );
    }
}
