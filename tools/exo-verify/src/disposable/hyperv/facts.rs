//! Measured guest readiness and pure comparisons against a sealed image.

use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use std::path::PathBuf;
use windows::Win32::Graphics::Dxgi::{CreateDXGIFactory1, IDXGIFactory1};
use windows::Win32::Graphics::Gdi::{
    DEVMODEW, DISPLAY_DEVICE_ATTACHED_TO_DESKTOP, DISPLAY_DEVICEW, ENUM_CURRENT_SETTINGS,
    EnumDisplayDevicesW, EnumDisplaySettingsW,
};
use windows::core::PCWSTR;

use super::recipe::GpuConfig;

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct DisplayRequirement {
    pub width: u32,
    pub height: u32,
    pub refresh_hz: u32,
    pub hardware_id: String,
}

fn wide(text: &[u16]) -> String {
    String::from_utf16_lossy(&text[..text.iter().position(|c| *c == 0).unwrap_or(text.len())])
}

pub fn measure() -> Result<Value> {
    let mut displays = Vec::new();
    for index in 0..64 {
        let mut device = DISPLAY_DEVICEW {
            cb: std::mem::size_of::<DISPLAY_DEVICEW>() as u32,
            ..Default::default()
        };
        if !unsafe { EnumDisplayDevicesW(None, index, &mut device, 0) }.as_bool() {
            break;
        }
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
        if !unsafe {
            EnumDisplaySettingsW(
                PCWSTR(device.DeviceName.as_ptr()),
                ENUM_CURRENT_SETTINGS,
                &mut mode,
            )
        }
        .as_bool()
        {
            continue;
        }
        displays.push(json!({"device": wide(&device.DeviceName), "adapter": wide(&device.DeviceString),
            "width": mode.dmPelsWidth, "height": mode.dmPelsHeight, "refreshHz": mode.dmDisplayFrequency}));
    }
    let wmi = wmi::WMIConnection::new()?;
    let factory = unsafe { CreateDXGIFactory1::<IDXGIFactory1>() }?;
    let mut adapters = Vec::new();
    let mut index = 0;
    while let Ok(adapter) = unsafe { factory.EnumAdapters1(index) } {
        index += 1;
        let description = unsafe { adapter.GetDesc1() }?;
        adapters.push(
            json!({"name": wide(&description.Description), "vendorId": description.VendorId,
            "deviceId": description.DeviceId, "software": description.Flags & 2 != 0}),
        );
    }
    let devices: Vec<Value> =
        wmi.raw_query("SELECT HardwareID FROM Win32_PnPEntity WHERE PNPClass = 'Display'")?;
    let root = PathBuf::from(std::env::var_os("SystemRoot").context("SystemRoot missing")?)
        .join(r"System32\HostDriverStore\FileRepository");
    let mut packages = Vec::new();
    if root.is_dir() {
        for entry in std::fs::read_dir(root)? {
            let package = entry?.path();
            if !package.is_dir() {
                continue;
            }
            for inf in crate::package::walk_files(&package)?
                .into_iter()
                .filter(|p| p.extension().is_some_and(|e| e.eq_ignore_ascii_case("inf")))
            {
                if let Some(version) = std::fs::read_to_string(inf)
                    .ok()
                    .and_then(|text| super::recipe::inf_driver_version(&text))
                {
                    packages.push(json!({"package": package.file_name().unwrap_or_default().to_string_lossy(), "driverVersion": version}));
                }
            }
        }
    }
    Ok(
        json!({"session": exo_guest::agent::session_info(), "displayPaths": displays, "adapters": adapters,
        "displayDevices": devices, "hostDriverStore": packages}),
    )
}

pub fn unmet(
    receipt: &Value,
    gpu: Option<&GpuConfig>,
    display: Option<&DisplayRequirement>,
) -> Vec<String> {
    let mut unmet = Vec::new();
    let session = &receipt["session"];
    if session["interactive"].as_bool() != Some(true) {
        unmet.push("agent is not on the measured interactive console desktop".into());
    }
    if !session["user"].as_str().is_some_and(|u| {
        u.rsplit('\\')
            .next()
            .is_some_and(|u| u.eq_ignore_ascii_case("exosnap"))
    }) {
        unmet.push("agent account is not the disposable image's exosnap account".into());
    }
    if let Some(gpu) = gpu {
        if !receipt["adapters"].as_array().is_some_and(|rows| {
            rows.iter().any(|row| {
                row["name"]
                    .as_str()
                    .is_some_and(|name| name.eq_ignore_ascii_case(&gpu.host_name))
                    && row["vendorId"].as_u64() == Some(0x1414)
                    && row["software"].as_bool() == Some(false)
            })
        }) {
            unmet.push(format!(
                "no measured paravirtual adapter presents {}",
                gpu.host_name
            ));
        }
        if !receipt["hostDriverStore"].as_array().is_some_and(|rows| {
            rows.iter().any(|row| {
                row["package"]
                    .as_str()
                    .is_some_and(|v| v.eq_ignore_ascii_case(&gpu.driver_package))
                    && row["driverVersion"] == gpu.driver_version
            })
        }) {
            unmet.push(format!(
                "guest did not stage {} at {}",
                gpu.driver_package, gpu.driver_version
            ));
        }
    }
    if let Some(display) = display {
        if !receipt["displayPaths"].as_array().is_some_and(|paths| {
            paths.iter().any(|path| {
                path["width"] == display.width
                    && path["height"] == display.height
                    && path["refreshHz"] == display.refresh_hz
            })
        }) {
            unmet.push(format!(
                "no attached path measures {}x{}@{} Hz",
                display.width, display.height, display.refresh_hz
            ));
        }
        if !receipt["displayDevices"].as_array().is_some_and(|devices| {
            devices.iter().any(|device| {
                device["HardwareID"].as_array().is_some_and(|ids| {
                    ids.iter().any(|id| {
                        id.as_str()
                            .is_some_and(|id| id.eq_ignore_ascii_case(&display.hardware_id))
                    })
                })
            })
        }) {
            unmet.push(format!(
                "the qualified display device {} was not measured",
                display.hardware_id
            ));
        }
    }
    unmet
}

#[cfg(test)]
mod tests {
    use super::*;
    fn gpu() -> GpuConfig {
        GpuConfig {
            instance_path: "host".into(),
            host_name: "NVIDIA GPU".into(),
            driver_version: "32.1".into(),
            driver_package: "nv_active".into(),
            partition: super::super::recipe::default_partition(),
        }
    }
    fn display() -> DisplayRequirement {
        DisplayRequirement {
            width: 2560,
            height: 1440,
            refresh_hz: 60,
            hardware_id: r"Root\MttVDD".into(),
        }
    }
    fn receipt() -> Value {
        json!({"session": {"interactive": true, "user": "VM\\exosnap"},
        "adapters": [{"name": "NVIDIA GPU", "vendorId": 0x1414, "software": false}],
        "hostDriverStore": [{"package": "nv_active", "driverVersion": "32.1"}],
        "displayPaths": [{"width": 2560, "height": 1440, "refreshHz": 60}],
        "displayDevices": [{"HardwareID": ["Root\\MttVDD"]}]})
    }
    #[test]
    fn matching_paravirtual_identity_does_not_require_host_pci_ids_in_guest() {
        assert!(unmet(&receipt(), Some(&gpu()), Some(&display())).is_empty());
    }
    #[test]
    fn missing_facts_are_unproven_and_every_mismatch_is_reported() {
        let failures = unmet(&json!({}), Some(&gpu()), Some(&display()));
        assert_eq!(failures.len(), 6);
        let mut facts = receipt();
        facts["adapters"][0]["name"] = json!("Microsoft Basic Render Driver");
        facts["hostDriverStore"][0]["driverVersion"] = json!("wrong");
        assert_eq!(unmet(&facts, Some(&gpu()), None).len(), 2);
    }
    #[test]
    fn display_properties_must_belong_to_the_same_attached_path() {
        let mut facts = receipt();
        facts["displayPaths"] = json!([{"width": 2560, "height": 1440, "refreshHz": 144}, {"width": 1024, "height": 768, "refreshHz": 60}]);
        assert_eq!(unmet(&facts, None, Some(&display())).len(), 1);
        facts["displayPaths"]
            .as_array_mut()
            .unwrap()
            .push(json!({"width": 2560,"height":1440,"refreshHz":60}));
        assert!(unmet(&facts, None, Some(&display())).is_empty());
    }
}
