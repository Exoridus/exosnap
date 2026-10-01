//! How one screenshot is asked of `exosnap.exe`: the `--visual-test` command
//! line and the `EXOSNAP_VISUAL_*` seeds.
//!
//! Every app option this command passes is spelled in this file and nowhere
//! else, so the CLI flag registry check can scan one source for them.

use clap::{Args, ValueEnum};

/// Navigation destinations, in the product's navigation order. The index is
/// what `--visual-page` takes.
#[derive(Clone, Copy, Debug, PartialEq, Eq, ValueEnum)]
pub enum Page {
    Record,
    Settings,
    Diagnostics,
    Logs,
    About,
}

impl Page {
    fn index(self) -> u8 {
        match self {
            Page::Record => 0,
            Page::Settings => 1,
            Page::Diagnostics => 2,
            Page::Logs => 3,
            Page::About => 4,
        }
    }

    fn id(self) -> &'static str {
        match self {
            Page::Record => "record",
            Page::Settings => "settings",
            Page::Diagnostics => "diagnostics",
            Page::Logs => "logs",
            Page::About => "about",
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, ValueEnum)]
pub enum Appearance {
    Dark,
    Light,
}

impl Appearance {
    pub fn id(self) -> &'static str {
        match self {
            Appearance::Dark => "dark",
            Appearance::Light => "light",
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, ValueEnum)]
pub enum Popup {
    SourcePicker,
    NotificationHub,
}

impl Popup {
    fn id(self) -> &'static str {
        match self {
            Popup::SourcePicker => "source-picker",
            Popup::NotificationHub => "notification-hub",
        }
    }
}

/// What the application shows. Everything that is not an axis of a sweep
/// (appearance, accent, window size, scale) lives here.
#[derive(Args, Clone, Debug, Default, PartialEq)]
pub struct ShotSpec {
    /// Navigation destination. The app's landing page when omitted.
    #[arg(long, value_enum)]
    pub page: Option<Page>,
    /// Record lifecycle state (`--record-visual-state`). Forces the Record
    /// page unless `--page` names another one.
    #[arg(long)]
    pub record_state: Option<String>,
    /// Runtime overlay surface (`--overlay-visual-state`), for example
    /// `recovery-multi` or `recording-error`.
    #[arg(long)]
    pub overlay_state: Option<String>,
    /// Edit workspace fixture (`EXOSNAP_VISUAL_EDIT_SCENARIO`), for example
    /// `edit-trimmed`. Needs the Record page.
    #[arg(long)]
    pub edit: Option<String>,
    /// Diagnostics fixture (`EXOSNAP_VISUAL_DIAG_SCENARIO`), for example `issues`.
    #[arg(long)]
    pub diag: Option<String>,
    /// Live Diagnostics sequence (`EXOSNAP_VISUAL_DIAG_LIVE`), for example
    /// `healthy`, `degraded` or `after-stop`.
    #[arg(long)]
    pub diag_live: Option<String>,
    /// Log view fixture (`EXOSNAP_VISUAL_LOG_SCENARIO`): `empty`, `long-message`.
    #[arg(long)]
    pub log: Option<String>,
    /// Notification fixture (`EXOSNAP_VISUAL_NOTIFICATION_SCENARIO`): `many`, `many-info`.
    #[arg(long)]
    pub notifications: Option<String>,
    /// Source list fixture (`EXOSNAP_VISUAL_SOURCE_SCENARIO`): `many-windows`.
    #[arg(long)]
    pub sources: Option<String>,
    /// Fixed audio meter level in dBFS (`EXOSNAP_VISUAL_METER_DBFS`), or `-inf`.
    #[arg(long, allow_hyphen_values = true)]
    pub meter_dbfs: Option<String>,
    /// Opens a lazily built popup.
    #[arg(long, value_enum)]
    pub popup: Option<Popup>,
    /// Named modal dialog: `close-guard`, `preset-delete`, `preset-rename`,
    /// `preset-save-as`. Preset dialogs need `--page settings`.
    #[arg(long)]
    pub dialog: Option<String>,
    /// Expert arrangement of Settings and Diagnostics.
    #[arg(long)]
    pub expert: bool,
    /// Scroll position as a fraction of the page's scrollable range (0 to 1).
    #[arg(long, value_parser = parse_fraction)]
    pub scroll: Option<f64>,
    /// Scrolls Settings to its last card.
    #[arg(long)]
    pub settings_bottom: bool,
    /// Opens the Record button's countdown menu.
    #[arg(long)]
    pub countdown_menu: bool,
    /// The window as if it were tall enough for the whole page: the scrollable
    /// content in full, with the window above and below it.
    #[arg(long)]
    pub full_page: bool,
    /// Opens every collapsed element on the page before the capture.
    #[arg(long)]
    pub expand_all: bool,
    /// Deterministic moving content on the desktop. Implies the live preview.
    #[arg(long)]
    pub desktop_pattern: bool,
    /// Shows the real capture in the preview instead of the test card. The
    /// screenshot then contains whatever is on this screen.
    #[arg(long)]
    pub live_preview: bool,
    /// Appearance of shell-owned surfaces (desktop toast), independent of the
    /// app appearance.
    #[arg(long, value_enum)]
    pub shell_appearance: Option<Appearance>,
    /// Capture delay in milliseconds. The app picks a default per scenario.
    #[arg(long)]
    pub delay_ms: Option<u32>,
}

fn parse_fraction(value: &str) -> Result<f64, String> {
    let fraction: f64 = value
        .parse()
        .map_err(|_| format!("'{value}' is not a number"))?;
    if (0.0..=1.0).contains(&fraction) {
        Ok(fraction)
    } else {
        Err(format!("{value} is outside 0 to 1"))
    }
}

/// A logical window size.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Size {
    pub width: u32,
    pub height: u32,
}

impl std::str::FromStr for Size {
    type Err = String;

    fn from_str(value: &str) -> Result<Self, Self::Err> {
        let (width, height) = value
            .split_once('x')
            .ok_or_else(|| format!("'{value}' is not WIDTHxHEIGHT"))?;
        let parse = |part: &str| part.parse::<u32>().ok().filter(|n| *n > 0);
        match (parse(width), parse(height)) {
            (Some(width), Some(height)) => Ok(Size { width, height }),
            _ => Err(format!("'{value}' is not WIDTHxHEIGHT")),
        }
    }
}

impl std::fmt::Display for Size {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}x{}", self.width, self.height)
    }
}

/// The dimensions a sweep multiplies every shot by. Each takes a
/// comma-separated list.
#[derive(Args, Clone, Debug)]
pub struct Axes {
    /// App appearance(s). Defaults to the one in your ExoSnap settings, or to
    /// both for `sweep`.
    #[arg(long, value_enum, value_delimiter = ',')]
    pub appearance: Vec<Appearance>,
    /// Accent id(s), checked against the accents the product defines.
    /// Defaults to the one in your ExoSnap settings.
    #[arg(long, value_delimiter = ',')]
    pub accent: Vec<String>,
    /// Logical window size(s), WIDTHxHEIGHT.
    #[arg(long, value_delimiter = ',', default_value = "1600x1000")]
    pub size: Vec<Size>,
    /// Qt scale factor(s) (`QT_SCALE_FACTOR`); 1.5 yields PNGs at 1.5x the
    /// logical size.
    #[arg(long, value_delimiter = ',', default_value = "1")]
    pub scale: Vec<f64>,
}

/// One point on the axes.
#[derive(Clone, Debug, PartialEq)]
pub struct Variant {
    pub appearance: Appearance,
    pub accent: String,
    pub size: Size,
    pub scale: f64,
}

impl Axes {
    /// Fills an appearance or accent the caller left unspecified.
    pub fn or_defaults(mut self, appearance: &[Appearance], accent: &str) -> Axes {
        if self.appearance.is_empty() {
            self.appearance = appearance.to_vec();
        }
        if self.accent.is_empty() {
            self.accent = vec![accent.to_string()];
        }
        self
    }

    pub fn variants(&self) -> Vec<Variant> {
        let mut out = Vec::new();
        for appearance in &self.appearance {
            for accent in &self.accent {
                for size in &self.size {
                    for scale in &self.scale {
                        out.push(Variant {
                            appearance: *appearance,
                            accent: accent.clone(),
                            size: *size,
                            scale: *scale,
                        });
                    }
                }
            }
        }
        out
    }
}

impl Variant {
    /// The `<theme>_<resolution>` end of a file name, for example
    /// `dark-aqua_1600x1000` or `light-violet_860x700@1.5x`.
    pub fn suffix(&self) -> String {
        let mut suffix = format!(
            "{}-{}_{}",
            self.appearance.id(),
            sanitize(&self.accent),
            self.size
        );
        if self.scale != 1.0 {
            suffix.push_str(&format!("@{}x", self.scale));
        }
        suffix
    }
}

impl ShotSpec {
    /// The `<page>_<state>` start of a file name, used when the caller gives
    /// none. The state joins everything else the spec selects (lifecycle
    /// state, fixture, dialog, popup, arrangement) and is `default` when
    /// nothing is selected.
    /// The page part of a file name. The app lands on Record, and every
    /// selection without its own page flag belongs to Record too.
    pub fn page_id(&self) -> &'static str {
        self.page.unwrap_or(Page::Record).id()
    }

    pub fn derived_name(&self) -> String {
        let mut parts: Vec<String> = Vec::new();
        let mut push = |prefix: &str, value: &Option<String>| {
            if let Some(value) = value {
                parts.push(format!("{prefix}{value}"));
            }
        };
        push("", &self.record_state);
        push("", &self.overlay_state);
        push("", &self.edit);
        push("", &self.diag);
        push("live-", &self.diag_live);
        push("", &self.log);
        push("", &self.dialog);
        if let Some(popup) = self.popup {
            parts.push(popup.id().to_string());
        }
        let mut push = |prefix: &str, value: &Option<String>| {
            if let Some(value) = value {
                parts.push(format!("{prefix}{value}"));
            }
        };
        push("", &self.notifications);
        push("", &self.sources);
        push("meter", &self.meter_dbfs);
        if self.expert {
            parts.push("expert".to_string());
        }
        if let Some(scroll) = self.scroll {
            parts.push(format!("scroll{}", (scroll * 100.0).round()));
        }
        if self.settings_bottom {
            parts.push("bottom".to_string());
        }
        if self.countdown_menu {
            parts.push("countdown-menu".to_string());
        }
        if self.full_page {
            parts.push("full".to_string());
        }
        if self.expand_all {
            parts.push("expanded".to_string());
        }
        let state = if parts.is_empty() {
            "default".to_string()
        } else {
            sanitize(&parts.join("-"))
        };
        format!("{}_{state}", self.page_id())
    }

    /// The app command line and environment for this spec at one variant.
    pub fn invocation(&self, variant: &Variant, png: &str) -> (Vec<String>, Vec<(String, String)>) {
        let mut args = vec![
            "--visual-test".to_string(),
            png.to_string(),
            "--visual-test-size".to_string(),
            variant.size.to_string(),
            "--visual-appearance".to_string(),
            variant.appearance.id().to_string(),
            "--visual-accent".to_string(),
            variant.accent.clone(),
        ];
        let mut value = |flag: &str, value: String| {
            args.push(flag.to_string());
            args.push(value);
        };
        if let Some(page) = self.page {
            value("--visual-page", page.index().to_string());
        }
        if let Some(state) = &self.record_state {
            value("--record-visual-state", state.clone());
        }
        if let Some(state) = &self.overlay_state {
            value("--overlay-visual-state", state.clone());
        }
        if let Some(popup) = self.popup {
            value("--visual-popup", popup.id().to_string());
        }
        if let Some(dialog) = &self.dialog {
            value("--visual-dialog", dialog.clone());
        }
        if let Some(scroll) = self.scroll {
            value("--visual-scroll", scroll.to_string());
        }
        if let Some(shell) = self.shell_appearance {
            value("--visual-shell-appearance", shell.id().to_string());
        }
        if let Some(delay) = self.delay_ms {
            value("--visual-delay-ms", delay.to_string());
        }
        let mut flag = |enabled: bool, flag: &str| {
            if enabled {
                args.push(flag.to_string());
            }
        };
        flag(self.expert, "--visual-expert");
        flag(self.settings_bottom, "--settings-visual-bottom");
        flag(self.countdown_menu, "--record-visual-menu");
        flag(self.desktop_pattern, "--desktop-pattern");
        flag(self.live_preview, "--visual-live-preview");
        flag(self.full_page, "--visual-full-page");
        flag(self.expand_all, "--visual-expand-all");

        let mut env = Vec::new();
        let mut seed = |name: &str, value: &Option<String>| {
            if let Some(value) = value {
                env.push((name.to_string(), value.clone()));
            }
        };
        seed("EXOSNAP_VISUAL_EDIT_SCENARIO", &self.edit);
        seed("EXOSNAP_VISUAL_DIAG_SCENARIO", &self.diag);
        seed("EXOSNAP_VISUAL_DIAG_LIVE", &self.diag_live);
        seed("EXOSNAP_VISUAL_LOG_SCENARIO", &self.log);
        seed("EXOSNAP_VISUAL_NOTIFICATION_SCENARIO", &self.notifications);
        seed("EXOSNAP_VISUAL_SOURCE_SCENARIO", &self.sources);
        seed("EXOSNAP_VISUAL_METER_DBFS", &self.meter_dbfs);
        if variant.scale != 1.0 {
            env.push(("QT_SCALE_FACTOR".to_string(), variant.scale.to_string()));
        }
        (args, env)
    }
}

/// Lower-case ASCII letters, digits, hyphens and underscores, so a name is a
/// safe file stem on every file system and in a URL.
pub fn sanitize(name: &str) -> String {
    let mut out = String::with_capacity(name.len());
    for c in name.chars() {
        let c = c.to_ascii_lowercase();
        if c.is_ascii_alphanumeric() || c == '_' {
            out.push(c);
        } else if !out.ends_with('-') {
            out.push('-');
        }
    }
    out.trim_matches(['-', '_']).to_string()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn variant() -> Variant {
        Variant {
            appearance: Appearance::Light,
            accent: "violet".to_string(),
            size: Size {
                width: 860,
                height: 700,
            },
            scale: 1.0,
        }
    }

    #[test]
    fn a_page_is_passed_as_its_navigation_index() {
        let spec = ShotSpec {
            page: Some(Page::Diagnostics),
            ..ShotSpec::default()
        };
        let (args, _) = spec.invocation(&variant(), "out.png");
        let at = args.iter().position(|a| a == "--visual-page").unwrap();
        assert_eq!(args[at + 1], "2");
    }

    #[test]
    fn fixtures_become_environment_seeds_not_arguments() {
        let spec = ShotSpec {
            page: Some(Page::Record),
            edit: Some("edit-trimmed".to_string()),
            ..ShotSpec::default()
        };
        let (args, env) = spec.invocation(&variant(), "out.png");
        assert!(!args.iter().any(|a| a.contains("edit-trimmed")));
        assert_eq!(
            env,
            vec![(
                "EXOSNAP_VISUAL_EDIT_SCENARIO".to_string(),
                "edit-trimmed".to_string()
            )]
        );
    }

    #[test]
    fn appearance_and_accent_are_always_explicit() {
        let (args, _) = ShotSpec::default().invocation(&variant(), "out.png");
        assert!(
            args.windows(2)
                .any(|w| w == ["--visual-appearance", "light"])
        );
        assert!(args.windows(2).any(|w| w == ["--visual-accent", "violet"]));
        assert!(
            args.windows(2)
                .any(|w| w == ["--visual-test-size", "860x700"])
        );
    }

    #[test]
    fn scale_is_set_only_when_it_differs_from_one() {
        let (_, env) = ShotSpec::default().invocation(&variant(), "out.png");
        assert!(env.is_empty());
        let scaled = Variant {
            scale: 1.5,
            ..variant()
        };
        let (_, env) = ShotSpec::default().invocation(&scaled, "out.png");
        assert_eq!(
            env,
            vec![("QT_SCALE_FACTOR".to_string(), "1.5".to_string())]
        );
        assert_eq!(scaled.suffix(), "light-violet_860x700@1.5x");
    }

    #[test]
    fn derived_names_describe_the_selection() {
        let spec = ShotSpec {
            page: Some(Page::Settings),
            dialog: Some("preset-rename".to_string()),
            ..ShotSpec::default()
        };
        assert_eq!(spec.derived_name(), "settings_preset-rename");
        assert_eq!(ShotSpec::default().derived_name(), "record_default");
        let edit = ShotSpec {
            edit: Some("edit-trimmed".to_string()),
            ..ShotSpec::default()
        };
        assert_eq!(edit.derived_name(), "record_edit-trimmed");
    }

    #[test]
    fn sizes_parse_and_reject_garbage() {
        assert_eq!(
            "1280x720".parse::<Size>().unwrap(),
            Size {
                width: 1280,
                height: 720
            }
        );
        assert!("1280".parse::<Size>().is_err());
        assert!("0x720".parse::<Size>().is_err());
    }

    #[test]
    fn scroll_is_a_fraction() {
        assert!(parse_fraction("0.5").is_ok());
        assert!(parse_fraction("1.2").is_err());
    }

    #[test]
    fn sanitize_keeps_file_stems_portable() {
        assert_eq!(sanitize("Record / Paused!"), "record-paused");
        assert_eq!(sanitize("_record_paused-"), "record_paused");
    }
}
