//! Windows probes. Read-only: nothing here changes display, audio or device state.

use serde_json::json;
use windows::Win32::Foundation::{HANDLE, LPARAM, RECT};
use windows::Win32::Graphics::Direct3D::D3D_DRIVER_TYPE_UNKNOWN;
use windows::Win32::Graphics::Direct3D11::{
    D3D11_CREATE_DEVICE_BGRA_SUPPORT, D3D11_SDK_VERSION, D3D11CreateDevice,
};
use windows::Win32::Graphics::Dxgi::Common::DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
use windows::Win32::Graphics::Dxgi::{
    CreateDXGIFactory1, IDXGIAdapter1, IDXGIFactory1, IDXGIOutput1, IDXGIOutput6,
};
use windows::Win32::Graphics::Gdi::{EnumDisplayMonitors, HDC, HMONITOR};
use windows::Win32::Media::Audio::{
    DEVICE_STATE_ACTIVE, IMMDeviceEnumerator, MMDeviceEnumerator, eCapture, eRender,
};
use windows::Win32::Security::{GetTokenInformation, TOKEN_ELEVATION, TOKEN_QUERY, TokenElevation};
use windows::Win32::System::Com::{
    CLSCTX_ALL, COINIT_MULTITHREADED, CoCreateInstance, CoInitializeEx,
};
use windows::Win32::System::LibraryLoader::LoadLibraryW;
use windows::Win32::System::RemoteDesktop::ProcessIdToSessionId;
use windows::Win32::System::StationsAndDesktops::{
    CloseDesktop, DESKTOP_READOBJECTS, OpenInputDesktop,
};
use windows::Win32::System::Threading::{GetCurrentProcess, GetCurrentProcessId, OpenProcessToken};
use windows::Win32::UI::HiDpi::{GetDpiForMonitor, MDT_EFFECTIVE_DPI};
use windows::core::{BOOL, Interface, w};

use crate::capability::{Capability, CapabilitySet};

pub fn probe_into(set: &mut CapabilitySet) {
    set.insert(Capability::Windows);
    unsafe {
        let _ = CoInitializeEx(None, COINIT_MULTITHREADED);
    }
    if is_elevated() {
        set.insert(Capability::Admin);
    }
    let session = session_id();
    set.facts.insert("sessionId".into(), json!(session));
    if session.is_some_and(|s| s != 0) && input_desktop_accessible() {
        set.insert(Capability::InteractiveDesktop);
    }

    let adapters = adapters();
    set.facts.insert(
        "adapters".into(),
        json!(adapters.iter().map(|a| &a.description).collect::<Vec<_>>()),
    );
    if adapters
        .iter()
        .any(|a| a.vendor_id == 0x10DE && !a.software)
    {
        set.insert(Capability::NvidiaGpu);
        if unsafe { LoadLibraryW(w!("nvEncodeAPI64.dll")) }.is_ok() {
            set.insert(Capability::Nvenc);
        }
    }
    if adapters.iter().any(|a| !a.software && a.d3d11) {
        set.insert(Capability::D3d11);
    }
    let outputs: Vec<&OutputInfo> = adapters.iter().flat_map(|a| a.outputs.iter()).collect();
    set.facts.insert(
        "outputs".into(),
        json!(
            outputs
                .iter()
                .map(|o| json!({"name": o.name, "hdr": o.hdr, "duplication": o.duplication}))
                .collect::<Vec<_>>()
        ),
    );
    if outputs.iter().any(|o| o.hdr) {
        set.insert(Capability::HdrDisplay);
    }
    if set.has(Capability::InteractiveDesktop) && outputs.iter().any(|o| o.duplication) {
        set.insert(Capability::DxgiDuplication);
    }
    if set.has(Capability::InteractiveDesktop) && set.has(Capability::D3d11) && wgc_supported() {
        set.insert(Capability::Wgc);
    }

    let dpis = monitor_dpis();
    set.facts.insert("monitorDpi".into(), json!(dpis));
    if dpis.len() >= 2 {
        set.insert(Capability::MultiMonitor);
        if dpis.iter().any(|d| *d != dpis[0]) {
            set.insert(Capability::MixedDpi);
        }
    }

    let (render, capture) = audio_endpoint_counts();
    set.facts.insert(
        "audioEndpoints".into(),
        json!({"render": render, "capture": capture}),
    );
    if render > 0 {
        set.insert(Capability::AudioRender);
    }
    if capture > 0 {
        set.insert(Capability::AudioCapture);
    }
    let cameras = webcam_count();
    set.facts.insert("webcams".into(), json!(cameras));
    if cameras > 0 {
        set.insert(Capability::Webcam);
    }
}

fn is_elevated() -> bool {
    unsafe {
        let mut token = HANDLE::default();
        if OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut token).is_err() {
            return false;
        }
        let mut elevation = TOKEN_ELEVATION::default();
        let mut size = 0u32;
        let ok = GetTokenInformation(
            token,
            TokenElevation,
            Some(&mut elevation as *mut _ as *mut _),
            std::mem::size_of::<TOKEN_ELEVATION>() as u32,
            &mut size,
        )
        .is_ok();
        let _ = windows::Win32::Foundation::CloseHandle(token);
        ok && elevation.TokenIsElevated != 0
    }
}

fn session_id() -> Option<u32> {
    let mut session = 0u32;
    unsafe { ProcessIdToSessionId(GetCurrentProcessId(), &mut session) }
        .ok()
        .map(|_| session)
}

fn input_desktop_accessible() -> bool {
    unsafe {
        match OpenInputDesktop(Default::default(), false, DESKTOP_READOBJECTS) {
            Ok(desktop) => {
                let _ = CloseDesktop(desktop);
                true
            }
            Err(_) => false,
        }
    }
}

struct OutputInfo {
    name: String,
    hdr: bool,
    duplication: bool,
}

struct AdapterInfo {
    description: String,
    vendor_id: u32,
    software: bool,
    d3d11: bool,
    outputs: Vec<OutputInfo>,
}

fn adapters() -> Vec<AdapterInfo> {
    let mut out = Vec::new();
    let Ok(factory) = (unsafe { CreateDXGIFactory1::<IDXGIFactory1>() }) else {
        return out;
    };
    let mut index = 0;
    while let Ok(adapter) = unsafe { factory.EnumAdapters1(index) } {
        index += 1;
        let Ok(desc) = (unsafe { adapter.GetDesc1() }) else {
            continue;
        };
        let description = String::from_utf16_lossy(&desc.Description)
            .trim_end_matches('\0')
            .to_string();
        let software = (desc.Flags & 2) != 0; // DXGI_ADAPTER_FLAG_SOFTWARE
        let device = create_device(&adapter);
        let mut outputs = Vec::new();
        let mut o = 0;
        while let Ok(output) = unsafe { adapter.EnumOutputs(o) } {
            o += 1;
            let name = unsafe { output.GetDesc() }
                .map(|d| {
                    String::from_utf16_lossy(&d.DeviceName)
                        .trim_end_matches('\0')
                        .to_string()
                })
                .unwrap_or_default();
            let hdr = output
                .cast::<IDXGIOutput6>()
                .ok()
                .and_then(|o6| unsafe { o6.GetDesc1() }.ok())
                .is_some_and(|d| d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
            let duplication = match (&device, output.cast::<IDXGIOutput1>()) {
                (Some(device), Ok(o1)) => unsafe { o1.DuplicateOutput(device) }.is_ok(),
                _ => false,
            };
            outputs.push(OutputInfo {
                name,
                hdr,
                duplication,
            });
        }
        out.push(AdapterInfo {
            description,
            vendor_id: desc.VendorId,
            software,
            d3d11: device.is_some(),
            outputs,
        });
    }
    out
}

fn create_device(
    adapter: &IDXGIAdapter1,
) -> Option<windows::Win32::Graphics::Direct3D11::ID3D11Device> {
    let mut device = None;
    unsafe {
        D3D11CreateDevice(
            adapter,
            D3D_DRIVER_TYPE_UNKNOWN,
            Default::default(),
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            None,
            D3D11_SDK_VERSION,
            Some(&mut device),
            None,
            None,
        )
    }
    .ok()?;
    device
}

fn wgc_supported() -> bool {
    windows::Graphics::Capture::GraphicsCaptureSession::IsSupported().unwrap_or(false)
}

fn monitor_dpis() -> Vec<u32> {
    unsafe extern "system" fn callback(
        monitor: HMONITOR,
        _: HDC,
        _: *mut RECT,
        data: LPARAM,
    ) -> BOOL {
        let dpis = unsafe { &mut *(data.0 as *mut Vec<u32>) };
        let (mut x, mut y) = (0u32, 0u32);
        if unsafe { GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &mut x, &mut y) }.is_ok() {
            dpis.push(x);
        }
        true.into()
    }
    let mut dpis: Vec<u32> = Vec::new();
    unsafe {
        let _ = EnumDisplayMonitors(
            None,
            None,
            Some(callback),
            LPARAM(&mut dpis as *mut _ as isize),
        );
    }
    dpis
}

fn audio_endpoint_counts() -> (u32, u32) {
    unsafe {
        let Ok(enumerator) =
            CoCreateInstance::<_, IMMDeviceEnumerator>(&MMDeviceEnumerator, None, CLSCTX_ALL)
        else {
            return (0, 0);
        };
        let count = |flow| {
            enumerator
                .EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE)
                .and_then(|c| c.GetCount())
                .unwrap_or(0)
        };
        (count(eRender), count(eCapture))
    }
}

/// Names of the named pipes that currently exist under `\\.\pipe\` and start with `prefix`.
pub fn named_pipes(prefix: &str) -> Vec<String> {
    use windows::Win32::Storage::FileSystem::{
        FindClose, FindFirstFileW, FindNextFileW, WIN32_FIND_DATAW,
    };
    use windows::core::w;
    let mut out = Vec::new();
    let mut data = WIN32_FIND_DATAW::default();
    let Ok(handle) = (unsafe { FindFirstFileW(w!(r"\\.\pipe\*"), &mut data) }) else {
        return out;
    };
    loop {
        let name = String::from_utf16_lossy(&data.cFileName)
            .trim_end_matches('\0')
            .to_string();
        if name.starts_with(prefix) {
            out.push(name);
        }
        if unsafe { FindNextFileW(handle, &mut data) }.is_err() {
            break;
        }
    }
    unsafe {
        let _ = FindClose(handle);
    }
    out
}

/// Visible top-level windows of a process.
pub fn process_windows(pid: u32) -> Vec<windows::Win32::Foundation::HWND> {
    use windows::Win32::Foundation::HWND;
    use windows::Win32::UI::WindowsAndMessaging::{
        EnumWindows, GetWindowThreadProcessId, IsWindowVisible,
    };
    unsafe extern "system" fn cb(hwnd: HWND, data: LPARAM) -> BOOL {
        let (pid, list) = unsafe { &mut *(data.0 as *mut (u32, Vec<HWND>)) };
        let mut owner = 0u32;
        unsafe { GetWindowThreadProcessId(hwnd, Some(&mut owner)) };
        if owner == *pid && unsafe { IsWindowVisible(hwnd) }.as_bool() {
            list.push(hwnd);
        }
        true.into()
    }
    let mut state: (u32, Vec<HWND>) = (pid, Vec::new());
    unsafe {
        let _ = EnumWindows(Some(cb), LPARAM(&mut state as *mut _ as isize));
    }
    state.1
}

/// Asks every visible window of a process to close, exactly as the window
/// manager's close button does. Not input synthesis: no cursor, no keyboard.
pub fn close_windows(pid: u32) -> usize {
    use windows::Win32::Foundation::{LPARAM as L, WPARAM};
    use windows::Win32::UI::WindowsAndMessaging::{PostMessageW, WM_CLOSE};
    let windows = process_windows(pid);
    for hwnd in &windows {
        unsafe {
            let _ = PostMessageW(Some(*hwnd), WM_CLOSE, WPARAM(0), L(0));
        }
    }
    windows.len()
}

/// Luma of the desktop inside `rect` (virtual-screen physical pixels), read
/// through GDI. Used only to prove a precondition (the stimulus is actually
/// on screen) independently of the product.
pub fn screen_luma(rect: [i32; 4], width: u32) -> anyhow::Result<(u32, u32, Vec<u8>)> {
    use windows::Win32::Graphics::Gdi::*;
    let (w, h) = (rect[2] - rect[0], rect[3] - rect[1]);
    anyhow::ensure!(w > 0 && h > 0, "empty rectangle");
    let height = ((h as u64 * width as u64 / w as u64) as u32).max(1);
    unsafe {
        let screen = GetDC(None);
        let mem = CreateCompatibleDC(Some(screen));
        let mut info = BITMAPINFO::default();
        info.bmiHeader.biSize = std::mem::size_of::<BITMAPINFOHEADER>() as u32;
        info.bmiHeader.biWidth = width as i32;
        info.bmiHeader.biHeight = -(height as i32);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        let mut bits: *mut core::ffi::c_void = std::ptr::null_mut();
        let dib = CreateDIBSection(Some(mem), &info, DIB_RGB_COLORS, &mut bits, None, 0)?;
        let old = SelectObject(mem, dib.into());
        SetStretchBltMode(mem, HALFTONE);
        let ok = StretchBlt(
            mem,
            0,
            0,
            width as i32,
            height as i32,
            Some(screen),
            rect[0],
            rect[1],
            w,
            h,
            SRCCOPY,
        )
        .as_bool();
        let bgra =
            std::slice::from_raw_parts(bits as *const u8, (width * height * 4) as usize).to_vec();
        SelectObject(mem, old);
        let _ = DeleteObject(dib.into());
        let _ = DeleteDC(mem);
        ReleaseDC(None, screen);
        anyhow::ensure!(ok, "StretchBlt from the screen failed");
        Ok((width, height, crate::pattern::bgra_to_luma(&bgra)))
    }
}

/// String fields of an image's VERSIONINFO resource, keyed by field name
/// (`ProductVersion`, `FileVersion`, `ProductName`, ...).
pub fn version_strings(
    path: &std::path::Path,
) -> anyhow::Result<std::collections::BTreeMap<String, String>> {
    use windows::Win32::Storage::FileSystem::{
        GetFileVersionInfoSizeW, GetFileVersionInfoW, VerQueryValueW,
    };
    use windows::core::{HSTRING, PCWSTR};
    let wide = HSTRING::from(path.as_os_str());
    let size = unsafe { GetFileVersionInfoSizeW(&wide, None) };
    anyhow::ensure!(size > 0, "{} has no version resource", path.display());
    let mut data = vec![0u8; size as usize];
    unsafe { GetFileVersionInfoW(&wide, None, size, data.as_mut_ptr() as *mut _)? };
    let query = |key: &str| -> Option<(*const u8, u32)> {
        let key = HSTRING::from(key);
        let mut ptr = std::ptr::null_mut();
        let mut len = 0u32;
        unsafe {
            VerQueryValueW(
                data.as_ptr() as *const _,
                PCWSTR(key.as_ptr()),
                &mut ptr,
                &mut len,
            )
        }
        .as_bool()
        .then_some((ptr as *const u8, len))
    };
    let (ptr, len) = query("\\VarFileInfo\\Translation")
        .ok_or_else(|| anyhow::anyhow!("no translation table"))?;
    anyhow::ensure!(len >= 4, "empty translation table");
    let (lang, codepage) = unsafe { (*(ptr as *const u16), *(ptr.add(2) as *const u16)) };
    let mut out = std::collections::BTreeMap::new();
    for field in [
        "ProductVersion",
        "FileVersion",
        "ProductName",
        "CompanyName",
        "FileDescription",
        "OriginalFilename",
    ] {
        if let Some((p, l)) = query(&format!(
            "\\StringFileInfo\\{lang:04x}{codepage:04x}\\{field}"
        )) {
            let chars = unsafe { std::slice::from_raw_parts(p as *const u16, l as usize) };
            let text = String::from_utf16_lossy(chars)
                .trim_end_matches('\0')
                .to_string();
            out.insert(field.to_string(), text);
        }
    }
    Ok(out)
}

fn webcam_count() -> u32 {
    use windows::Win32::Media::MediaFoundation::{
        IMFActivate, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID, MF_VERSION, MFCreateAttributes,
        MFEnumDeviceSources, MFSTARTUP_LITE, MFStartup,
    };
    unsafe {
        if MFStartup(MF_VERSION, MFSTARTUP_LITE).is_err() {
            return 0;
        }
        let mut attributes = None;
        if MFCreateAttributes(&mut attributes, 1).is_err() {
            return 0;
        }
        let Some(attributes) = attributes else {
            return 0;
        };
        if attributes
            .SetGUID(
                &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID,
            )
            .is_err()
        {
            return 0;
        }
        let mut devices: *mut Option<IMFActivate> = std::ptr::null_mut();
        let mut count = 0u32;
        if MFEnumDeviceSources(&attributes, &mut devices, &mut count).is_err() {
            return 0;
        }
        for i in 0..count as usize {
            drop(std::ptr::read(devices.add(i)));
        }
        windows::Win32::System::Com::CoTaskMemFree(Some(devices as *const _));
        count
    }
}
