//! Physical display topology checks for the frontend benchmark.
//!
//! The campaign is only meaningful if the workload renders on the capture
//! display and ExoSnap is visible on the other one. Neither Superposition nor
//! Windows reports that directly, so the orchestrator asserts it and aborts
//! instead of quietly benchmarking the wrong monitor.
//!
//! Identity is established from independent facts (primary flag, pixel mode,
//! EDID model string), never from a numeric enumeration index: WMI's EDID
//! enumeration order is not guaranteed to line up with the display
//! enumeration order, and on at least one development machine it is the exact
//! reverse. Pairing monitor identity to a screen by position would attach the
//! wrong model name to the right screen, a mislabelling that would not be
//! merely unverified but actively misleading. The model set is therefore
//! asserted as a whole (attached anywhere), never per screen.

use serde::Deserialize;

/// One attached display and the mode it currently runs, independent of any
/// EDID identity.
#[derive(Clone, Debug, PartialEq)]
pub struct DisplayFact {
    pub device_name: String,
    pub primary: bool,
    pub x: i32,
    pub y: i32,
    pub width: i32,
    pub height: i32,
    pub refresh_hz: i32,
    pub bits_per_pixel: i32,
}

/// One EDID identity read from WMI, in whatever order WMI enumerates it.
#[derive(Clone, Debug, PartialEq)]
pub struct EdidMonitor {
    pub instance_name: String,
    pub model: String,
}

#[derive(Clone, Debug, Default, Deserialize)]
pub struct DisplayExpectation {
    pub width: i32,
    pub height: i32,
    #[serde(default)]
    pub min_refresh_hz: i32,
    #[serde(default)]
    pub model_contains: Option<String>,
}

#[derive(Clone, Debug, Default, Deserialize)]
pub struct TopologyExpectation {
    pub capture_display: DisplayExpectation,
    pub ui_display: DisplayExpectation,
}

#[derive(Clone, Debug, Default)]
pub struct TopologyReport {
    pub ok: bool,
    pub problems: Vec<String>,
    pub capture_display: Option<DisplayFact>,
    pub ui_display: Option<DisplayFact>,
    pub attached_panels: Vec<String>,
    pub all: Vec<DisplayFact>,
}

/// Asserts the scenario's expected capture/UI displays against the machine.
pub fn verify(expected: &TopologyExpectation) -> anyhow::Result<TopologyReport> {
    let topology = enumerate_displays()?;
    let attached_models = edid_monitor_models();
    Ok(assert_topology(&topology, &attached_models, expected))
}

/// The comparison logic, over already-collected facts. Kept separate from
/// `verify` so it can be exercised without a real display or WMI namespace.
fn assert_topology(
    topology: &[DisplayFact],
    attached_models: &[EdidMonitor],
    expected: &TopologyExpectation,
) -> TopologyReport {
    let mut problems = Vec::new();
    let capture = topology.iter().find(|d| d.primary).cloned();
    let ui = topology.iter().find(|d| !d.primary).cloned();

    let model_names: Vec<String> = attached_models
        .iter()
        .map(|m| m.model.trim().to_string())
        .filter(|m| !m.is_empty())
        .collect();

    for wanted in [
        expected.capture_display.model_contains.as_deref(),
        expected.ui_display.model_contains.as_deref(),
    ]
    .into_iter()
    .flatten()
    {
        if model_names.is_empty() {
            problems.push(format!(
                "EDID model names unavailable; cannot confirm '{wanted}' is attached."
            ));
        } else if !model_names.iter().any(|m| m.contains(wanted)) {
            problems.push(format!(
                "Expected panel '*{wanted}*' is not attached (found: {}).",
                model_names.join(", ")
            ));
        }
    }

    match &capture {
        None => problems.push("No primary display was reported.".to_string()),
        Some(capture) => {
            let want = &expected.capture_display;
            if capture.width != want.width || capture.height != want.height {
                problems.push(format!(
                    "Capture display is {}x{}, scenario requires {}x{}.",
                    capture.width, capture.height, want.width, want.height
                ));
            }
            if want.min_refresh_hz > 0 && capture.refresh_hz < want.min_refresh_hz {
                problems.push(format!(
                    "Capture display runs at {} Hz, scenario requires at least {} Hz.",
                    capture.refresh_hz, want.min_refresh_hz
                ));
            }
        }
    }

    match &ui {
        None => problems.push(
            "No secondary display was reported; ExoSnap would have to live inside the captured image."
                .to_string(),
        ),
        Some(ui) => {
            let want = &expected.ui_display;
            if want.width > 0 && (ui.width != want.width || ui.height != want.height) {
                problems.push(format!(
                    "UI display is {}x{}, scenario expects {}x{}.",
                    ui.width, ui.height, want.width, want.height
                ));
            }
        }
    }

    TopologyReport {
        ok: problems.is_empty(),
        problems,
        capture_display: capture,
        ui_display: ui,
        attached_panels: model_names,
        all: topology.to_vec(),
    }
}

#[cfg(windows)]
fn enumerate_displays() -> anyhow::Result<Vec<DisplayFact>> {
    win32::enumerate_displays()
}

#[cfg(not(windows))]
fn enumerate_displays() -> anyhow::Result<Vec<DisplayFact>> {
    anyhow::bail!("display topology enumeration is only available on Windows")
}

#[cfg(windows)]
fn edid_monitor_models() -> Vec<EdidMonitor> {
    win32::edid_monitor_models()
}

#[cfg(not(windows))]
fn edid_monitor_models() -> Vec<EdidMonitor> {
    Vec::new()
}

#[cfg(windows)]
mod win32 {
    use super::{DisplayFact, EdidMonitor};

    use windows::Win32::Graphics::Gdi::{
        DEVMODEW, DISPLAY_DEVICE_ATTACHED_TO_DESKTOP, DISPLAY_DEVICE_PRIMARY_DEVICE,
        DISPLAY_DEVICEW, ENUM_CURRENT_SETTINGS, EnumDisplayDevicesW, EnumDisplaySettingsW,
    };

    /// Every attached display with the facts the campaign asserts on.
    ///
    /// Built from `EnumDisplayDevicesW` (which display devices exist and
    /// which one is primary) rather than `EnumDisplayMonitors` (which visible
    /// monitor rectangles exist): the campaign's facts are per adapter/output
    /// mode, and `EnumDisplayDevicesW` is what carries `dmPosition`/refresh
    /// through `EnumDisplaySettingsW` on the same device name.
    pub fn enumerate_displays() -> anyhow::Result<Vec<DisplayFact>> {
        let mut result = Vec::new();
        let mut index = 0u32;
        loop {
            let mut device = DISPLAY_DEVICEW {
                cb: std::mem::size_of::<DISPLAY_DEVICEW>() as u32,
                ..Default::default()
            };
            // SAFETY: `device.cb` is set to the struct size as the API requires, and
            // the buffer it writes into is a plain fixed-size WCHAR array field.
            let more = unsafe { EnumDisplayDevicesW(None, index, &mut device, 0) };
            if !more.as_bool() {
                break;
            }
            index += 1;

            if !device
                .StateFlags
                .contains(DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)
            {
                continue;
            }

            let mut mode = DEVMODEW {
                dmSize: std::mem::size_of::<DEVMODEW>() as u16,
                ..Default::default()
            };
            // SAFETY: `mode.dmSize` is set as required; the device name comes from a
            // device this same loop just enumerated.
            let has_mode = unsafe {
                EnumDisplaySettingsW(
                    windows::core::PCWSTR(device.DeviceName.as_ptr()),
                    ENUM_CURRENT_SETTINGS,
                    &mut mode,
                )
            };
            if !has_mode.as_bool() {
                continue;
            }

            let name = String::from_utf16_lossy(&device.DeviceName)
                .trim_end_matches('\0')
                .to_string();
            // dmPosition is a union field in windows-rs; the position fields are
            // only meaningful when DM_POSITION is set, which every attached
            // desktop display reports.
            let (x, y) = unsafe {
                (
                    mode.Anonymous1.Anonymous2.dmPosition.x,
                    mode.Anonymous1.Anonymous2.dmPosition.y,
                )
            };

            result.push(DisplayFact {
                device_name: name,
                primary: device.StateFlags.contains(DISPLAY_DEVICE_PRIMARY_DEVICE),
                x,
                y,
                width: mode.dmPelsWidth as i32,
                height: mode.dmPelsHeight as i32,
                refresh_hz: mode.dmDisplayFrequency as i32,
                bits_per_pixel: mode.dmBitsPerPel as i32,
            });
        }
        Ok(result)
    }

    /// EDID model names, in the order WMI enumerates monitors. Deliberately
    /// never paired with `enumerate_displays`'s order: see the module doc.
    /// Returns an empty list when the namespace is unavailable rather than
    /// failing the whole topology check; an unreadable EDID degrades the
    /// model assertion, it must not abort the run before it can say why.
    pub fn edid_monitor_models() -> Vec<EdidMonitor> {
        query_edid_monitor_models().unwrap_or_default()
    }

    fn query_edid_monitor_models() -> anyhow::Result<Vec<EdidMonitor>> {
        use wmi::{Variant, WMIConnection};

        let connection = WMIConnection::with_namespace_path("ROOT\\WMI")?;
        let rows: Vec<std::collections::HashMap<String, Variant>> =
            connection.raw_query("SELECT InstanceName, UserFriendlyName FROM WmiMonitorID")?;

        let mut monitors = Vec::new();
        for row in rows {
            let instance_name = match row.get("InstanceName") {
                Some(Variant::String(value)) => value.clone(),
                _ => String::new(),
            };
            let model = match row.get("UserFriendlyName") {
                Some(Variant::Array(items)) => {
                    let codes: Vec<u16> = items
                        .iter()
                        .filter_map(|item| match item {
                            Variant::UI1(value) => Some(*value as u16),
                            Variant::UI2(value) => Some(*value),
                            Variant::I4(value) => Some(*value as u16),
                            _ => None,
                        })
                        .filter(|&code| code != 0)
                        .collect();
                    String::from_utf16_lossy(&codes)
                }
                _ => String::new(),
            };
            monitors.push(EdidMonitor {
                instance_name,
                model: model.trim().to_string(),
            });
        }
        Ok(monitors)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn display(name: &str, primary: bool, width: i32, height: i32, refresh_hz: i32) -> DisplayFact {
        DisplayFact {
            device_name: name.to_string(),
            primary,
            x: 0,
            y: 0,
            width,
            height,
            refresh_hz,
            bits_per_pixel: 32,
        }
    }

    fn edid(instance: &str, model: &str) -> EdidMonitor {
        EdidMonitor {
            instance_name: instance.to_string(),
            model: model.to_string(),
        }
    }

    fn expectation() -> TopologyExpectation {
        TopologyExpectation {
            capture_display: DisplayExpectation {
                width: 2560,
                height: 1440,
                min_refresh_hz: 60,
                model_contains: Some("27GL850".to_string()),
            },
            ui_display: DisplayExpectation {
                width: 1920,
                height: 1080,
                min_refresh_hz: 0,
                model_contains: Some("27GL650F".to_string()),
            },
        }
    }

    /// The real machine this was written against enumerates WMI monitors in
    /// the REVERSE of the screen order: screen 0 is the primary 1440p panel,
    /// while the first WmiMonitorID entry is the 1080p panel. Both orderings
    /// must be accepted identically, because the assertion never pairs a
    /// screen to a monitor by position.
    #[test]
    fn model_matching_is_correct_regardless_of_which_order_wmi_enumerates_in() {
        let topology = vec![
            display("\\\\.\\DISPLAY1", true, 2560, 1440, 144),
            display("\\\\.\\DISPLAY2", false, 1920, 1080, 60),
        ];

        let screen_order = vec![edid("mon0", "27GL850"), edid("mon1", "27GL650F")];
        let reversed_order = vec![edid("mon0", "27GL650F"), edid("mon1", "27GL850")];

        let report_screen_order = assert_topology(&topology, &screen_order, &expectation());
        let report_reversed_order = assert_topology(&topology, &reversed_order, &expectation());

        assert!(report_screen_order.ok, "{:?}", report_screen_order.problems);
        assert!(
            report_reversed_order.ok,
            "{:?}",
            report_reversed_order.problems
        );
    }

    /// A naive index-paired approach (screen\[i\] identified by edid\[i\]'s
    /// model) is the trap this module refuses to fall into: with the WMI
    /// order reversed relative to screen order, it attaches the wrong model
    /// to the wrong screen and would reject a topology that is in fact
    /// correct.
    #[test]
    fn a_naive_index_paired_match_would_incorrectly_reject_the_reversed_order() {
        let topology = vec![
            display("\\\\.\\DISPLAY1", true, 2560, 1440, 144),
            display("\\\\.\\DISPLAY2", false, 1920, 1080, 60),
        ];
        let reversed_order = vec![edid("mon0", "27GL650F"), edid("mon1", "27GL850")];

        let naively_paired_capture_model = &reversed_order[0].model;
        assert_ne!(naively_paired_capture_model, "27GL850");

        // The real function does not make this mistake.
        let report = assert_topology(&topology, &reversed_order, &expectation());
        assert!(report.ok, "{:?}", report.problems);
    }

    #[test]
    fn a_missing_expected_model_is_reported_and_fails() {
        let topology = vec![
            display("\\\\.\\DISPLAY1", true, 2560, 1440, 144),
            display("\\\\.\\DISPLAY2", false, 1920, 1080, 60),
        ];
        let attached = vec![edid("mon0", "SomeOtherPanel")];
        let report = assert_topology(&topology, &attached, &expectation());
        assert!(!report.ok);
        assert!(report.problems.iter().any(|p| p.contains("27GL850")));
        assert!(report.problems.iter().any(|p| p.contains("27GL650F")));
    }

    #[test]
    fn empty_edid_list_reports_unavailable_rather_than_missing() {
        let topology = vec![display("\\\\.\\DISPLAY1", true, 2560, 1440, 144)];
        let report = assert_topology(&topology, &[], &expectation());
        assert!(!report.ok);
        assert!(
            report
                .problems
                .iter()
                .any(|p| p.contains("EDID model names unavailable"))
        );
    }

    #[test]
    fn no_secondary_display_is_reported() {
        let topology = vec![display("\\\\.\\DISPLAY1", true, 2560, 1440, 144)];
        let attached = vec![edid("mon0", "27GL850"), edid("mon1", "27GL650F")];
        let report = assert_topology(&topology, &attached, &expectation());
        assert!(!report.ok);
        assert!(
            report
                .problems
                .iter()
                .any(|p| p.contains("No secondary display"))
        );
    }

    #[test]
    fn a_low_refresh_capture_display_fails_the_minimum() {
        let topology = vec![
            display("\\\\.\\DISPLAY1", true, 2560, 1440, 30),
            display("\\\\.\\DISPLAY2", false, 1920, 1080, 60),
        ];
        let attached = vec![edid("mon0", "27GL850"), edid("mon1", "27GL650F")];
        let report = assert_topology(&topology, &attached, &expectation());
        assert!(!report.ok);
        assert!(report.problems.iter().any(|p| p.contains("30 Hz")));
    }
}
