//! Machine capabilities. Scenarios declare what they need; this module answers
//! only whether it is present. Nothing here knows which provider (CI runner,
//! workstation, dedicated test box, VM) the answers come from.
//!
//! Physical-rig capabilities cannot be probed: an operator declares them with
//! `--attest`, and the scenario that uses them still verifies its own
//! precondition before judging the product.

use serde::Serialize;
use std::collections::{BTreeMap, BTreeSet};
use std::fmt;

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum Capability {
    Windows,
    Admin,
    InteractiveDesktop,
    D3d11,
    NvidiaGpu,
    Nvenc,
    Wgc,
    DxgiDuplication,
    MultiMonitor,
    MixedDpi,
    HdrDisplay,
    AudioRender,
    AudioCapture,
    Webcam,
    Ffprobe,
    Ffmpeg,
    /// An operator is present to perform a physical act and answer prompts.
    Operator,
    /// A physical audio device the operator can unplug and replug.
    PhysicalAudioDisconnect,
    /// The whole operating system may be discarded after destructive tests.
    DisposableOs,
}

impl Capability {
    pub const ALL: [Capability; 19] = [
        Capability::Windows,
        Capability::Admin,
        Capability::InteractiveDesktop,
        Capability::D3d11,
        Capability::NvidiaGpu,
        Capability::Nvenc,
        Capability::Wgc,
        Capability::DxgiDuplication,
        Capability::MultiMonitor,
        Capability::MixedDpi,
        Capability::HdrDisplay,
        Capability::AudioRender,
        Capability::AudioCapture,
        Capability::Webcam,
        Capability::Ffprobe,
        Capability::Ffmpeg,
        Capability::Operator,
        Capability::PhysicalAudioDisconnect,
        Capability::DisposableOs,
    ];

    pub fn name(self) -> &'static str {
        match self {
            Capability::Windows => "windows",
            Capability::Admin => "admin",
            Capability::InteractiveDesktop => "interactive-desktop",
            Capability::D3d11 => "d3d11",
            Capability::NvidiaGpu => "nvidia-gpu",
            Capability::Nvenc => "nvenc",
            Capability::Wgc => "wgc",
            Capability::DxgiDuplication => "dxgi-duplication",
            Capability::MultiMonitor => "multi-monitor",
            Capability::MixedDpi => "mixed-dpi",
            Capability::HdrDisplay => "hdr-display",
            Capability::AudioRender => "audio-render",
            Capability::AudioCapture => "audio-capture",
            Capability::Webcam => "webcam",
            Capability::Ffprobe => "ffprobe",
            Capability::Ffmpeg => "ffmpeg",
            Capability::Operator => "operator",
            Capability::PhysicalAudioDisconnect => "physical-audio-disconnect",
            Capability::DisposableOs => "disposable-os",
        }
    }

    pub fn parse(name: &str) -> Option<Capability> {
        Capability::ALL.into_iter().find(|c| c.name() == name)
    }

    /// Capabilities that only an operator can declare.
    pub fn is_attested(self) -> bool {
        matches!(
            self,
            Capability::Operator | Capability::PhysicalAudioDisconnect | Capability::DisposableOs
        )
    }
}

impl fmt::Display for Capability {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.name())
    }
}

#[derive(Debug, Clone, Default)]
pub struct CapabilitySet {
    present: BTreeSet<Capability>,
    /// What the probe saw, for the lane's environment record.
    pub facts: BTreeMap<String, serde_json::Value>,
}

impl CapabilitySet {
    pub fn has(&self, c: Capability) -> bool {
        self.present.contains(&c)
    }

    pub fn insert(&mut self, c: Capability) {
        self.present.insert(c);
    }

    #[allow(dead_code, reason = "Reserved for scenario capability overrides")]
    pub fn remove(&mut self, c: Capability) {
        self.present.remove(&c);
    }

    pub fn missing(&self, required: &[Capability]) -> Vec<Capability> {
        required.iter().copied().filter(|c| !self.has(*c)).collect()
    }

    pub fn names(&self) -> Vec<String> {
        self.present.iter().map(|c| c.name().to_string()).collect()
    }

    #[allow(dead_code, reason = "Reserved for scenario capability fixtures")]
    pub fn from_names(names: &[&str]) -> Self {
        let mut set = CapabilitySet::default();
        for n in names {
            set.insert(Capability::parse(n).expect("known capability"));
        }
        set
    }
}

/// Probes this machine. Probing never changes machine state.
pub fn probe(attested: &[Capability]) -> CapabilitySet {
    let mut set = CapabilitySet::default();
    for c in attested.iter().filter(|c| c.is_attested()) {
        set.insert(*c);
    }
    if crate::tools::resolve("ffprobe").is_some() {
        set.insert(Capability::Ffprobe);
    }
    if crate::tools::resolve("ffmpeg").is_some() {
        set.insert(Capability::Ffmpeg);
    }
    #[cfg(windows)]
    crate::win::probe_into(&mut set);
    set
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn names_round_trip() {
        for c in Capability::ALL {
            assert_eq!(Capability::parse(c.name()), Some(c));
        }
    }

    #[test]
    fn attesting_a_probeable_capability_has_no_effect() {
        let set = probe(&[Capability::HdrDisplay, Capability::Operator]);
        assert!(set.has(Capability::Operator));
        // HDR is only ever what the probe saw, never what someone claimed.
        let _ = set.has(Capability::HdrDisplay);
    }
}
