//! UI Automation driver for the installed WixStdBA Setup window.
//!
//! thmutil hosts every control in a custom window class and offers no UIA
//! provider of its own, so the controls surface through the generic HWND and
//! MSAA bridges as panes whose Name is the window text. This module drives the
//! real controls by name: it activates patterns the element supports and falls
//! back to the button's own window messages (`BM_CLICK`, `BM_GETCHECK`), the
//! same notification path a mouse press takes. It never moves the pointer or
//! synthesizes keyboard input, and every screenshot is captured from the
//! window itself with `PrintWindow`.
//!
//! The flow mirrors what the interactive Setup contract expects: the Install
//! button is enabled without any licence acceptance control, the option
//! checkboxes start unchecked and enabled, View license shows the embedded
//! offline license and returns, Launch starts the installed application, and
//! the uninstall page offers the unchecked local-data option.

use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::thread::sleep;
use std::time::{Duration, Instant};

use anyhow::{Context as _, Result, bail, ensure};
use windows::Win32::Foundation::{HWND, LPARAM, RECT, WPARAM};
use windows::Win32::Graphics::Gdi::{
    BI_RGB, BITMAPINFO, BITMAPINFOHEADER, CreateCompatibleBitmap, CreateCompatibleDC,
    DIB_RGB_COLORS, DeleteDC, DeleteObject, GetDC, GetDIBits, HGDIOBJ, ReleaseDC, SelectObject,
};
use windows::Win32::System::Com::{
    CLSCTX_INPROC_SERVER, COINIT_APARTMENTTHREADED, CoCreateInstance, CoInitializeEx,
};
use windows::Win32::System::Variant::VARIANT;
use windows::Win32::UI::Accessibility::{
    CUIAutomation, IUIAutomation, IUIAutomationCondition, IUIAutomationElement,
    IUIAutomationInvokePattern, IUIAutomationTogglePattern, ToggleState_On, TreeScope_Children,
    TreeScope_Descendants, UIA_InvokePatternId, UIA_NamePropertyId, UIA_TogglePatternId,
};
use windows::Win32::UI::Input::KeyboardAndMouse::IsWindowEnabled;
use windows::Win32::UI::WindowsAndMessaging::{BM_CLICK, BM_GETCHECK, GetWindowRect, SendMessageW};
use windows::core::BSTR;

use super::update::{exosnap_processes, terminate};

/// `BST_CHECKED`, the checked result of `BM_GETCHECK`.
const BST_CHECKED: isize = 1;
/// `PW_RENDERFULLCONTENT`, which captures the window even while it is not
/// foreground.
const PW_RENDERFULLCONTENT: u32 = 2;

// windows-rs generates PrintWindow only under an unrelated feature; the
// function itself is stable user32 API.
#[link(name = "user32")]
unsafe extern "system" {
    fn PrintWindow(hwnd: HWND, hdc: windows::Win32::Graphics::Gdi::HDC, flags: u32) -> i32;
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Mode {
    Install,
    Uninstall,
}

pub struct Run<'a> {
    pub setup: &'a Path,
    pub mode: Mode,
    pub log: &'a Path,
    pub expected_exe: &'a Path,
    pub screenshots: &'a Path,
    pub desktop_shortcut: bool,
    pub remove_user_data: bool,
    pub click_launch: bool,
}

pub fn drive(run: &Run<'_>) -> Result<()> {
    fs::create_dir_all(run.screenshots)
        .with_context(|| format!("create {}", run.screenshots.display()))?;
    unsafe {
        let _ = CoInitializeEx(None, COINIT_APARTMENTTHREADED);
    }
    let automation: IUIAutomation =
        unsafe { CoCreateInstance(&CUIAutomation, None, CLSCTX_INPROC_SERVER) }
            .context("create the UI Automation client")?;

    let verb = match run.mode {
        Mode::Install => "/install",
        Mode::Uninstall => "/uninstall",
    };
    let mut child = Command::new(run.setup)
        .arg(verb)
        .arg("/log")
        .arg(run.log)
        .spawn()
        .with_context(|| format!("start {}", run.setup.display()))?;

    let result = wait_window(&automation, 120.0).and_then(|_| match run.mode {
        Mode::Install => install_flow(&automation, run),
        Mode::Uninstall => uninstall_flow(&automation, run),
    });
    let _ = child.wait();
    result
}

fn install_flow(automation: &IUIAutomation, run: &Run<'_>) -> Result<()> {
    // ExoSnap is GPL-3.0-or-later: no acceptance control exists and the
    // Install button must already be enabled when the page appears.
    let install = wait_control(automation, "Install ExoSnap", Some("Button"), 90.0)?;
    ensure!(
        enabled(&install),
        "the Install button is disabled without a licence acceptance control"
    );
    if find_control(
        automation,
        "I agree to the license terms and conditions",
        Some("Button"),
    )
    .is_some()
    {
        bail!("the removed licence acceptance checkbox is still present");
    }

    // The checkbox caption is clipped to the glyph; the readable text beside
    // it is a Label. Find the real button by class, assert it is enabled and
    // unchecked by default.
    let desktop = wait_control(
        automation,
        "Create a desktop shortcut",
        Some("Button"),
        30.0,
    )?;
    ensure!(
        enabled(&desktop),
        "the desktop shortcut checkbox is disabled"
    );
    let state = check_state(&desktop)?;
    ensure!(
        state == "Off",
        "the desktop shortcut option defaulted to {state}"
    );
    screenshot(automation, run.screenshots, "install-1-install-page.png")?;

    // The canonical GPL text must be viewable inside Setup, offline.
    let view_license = wait_control(automation, "View license", None, 30.0)?;
    invoke(&view_license)?;
    let back = wait_control(automation, "Back", Some("Button"), 60.0)?;
    sleep(Duration::from_millis(500));
    screenshot(automation, run.screenshots, "install-2-license.png")?;
    invoke(&back)?;
    let _ = wait_control(automation, "View license", None, 30.0)?;

    if run.desktop_shortcut {
        toggle(&desktop)?;
    }
    invoke(&install)?;
    sleep(Duration::from_millis(800));
    screenshot(automation, run.screenshots, "install-3-progress.png")?;

    let _ = wait_control(automation, "Launch", Some("Button"), 900.0)?;
    screenshot(automation, run.screenshots, "install-4-success.png")?;
    if run.click_launch {
        let launch = wait_control(automation, "Launch", Some("Button"), 30.0)?;
        invoke(&launch)?;
        let pid = wait_started(run.expected_exe, 90.0)?;
        terminate(pid);
        // WixStdBA closes the Success window itself after launching the target.
    } else {
        let close = wait_control(automation, "Close", Some("Button"), 30.0)?;
        invoke(&close)?;
    }
    Ok(())
}

fn uninstall_flow(automation: &IUIAutomation, run: &Run<'_>) -> Result<()> {
    let remove = wait_control(
        automation,
        "Also remove local ExoSnap data",
        Some("Button"),
        120.0,
    )?;
    ensure!(enabled(&remove), "the remove-data checkbox is disabled");
    let state = check_state(&remove)?;
    ensure!(
        state == "Off",
        "the remove-data option defaulted to {state}"
    );
    screenshot(automation, run.screenshots, "uninstall-1-modify.png")?;
    if run.remove_user_data {
        toggle(&remove)?;
    }

    let uninstall = wait_control(automation, "Uninstall", Some("Button"), 30.0)?;
    invoke(&uninstall)?;
    sleep(Duration::from_millis(800));
    screenshot(automation, run.screenshots, "uninstall-2-progress.png")?;

    let _ = wait_control(automation, "Close", Some("Button"), 900.0)?;
    screenshot(automation, run.screenshots, "uninstall-3-success.png")?;
    let close = wait_control(automation, "Close", Some("Button"), 30.0)?;
    invoke(&close)?;
    Ok(())
}

/// The visible Setup window, re-fetched on every call: Burn can briefly show a
/// splash window with the same caption, and a remembered element would go
/// stale.
fn setup_window(automation: &IUIAutomation) -> Result<IUIAutomationElement> {
    let root = unsafe { automation.GetRootElement() }.context("read the desktop root element")?;
    let condition = name_condition(automation, "ExoSnap Setup")?;
    unsafe { root.FindFirst(TreeScope_Children, &condition) }.context("no ExoSnap Setup window")
}

fn wait_window(automation: &IUIAutomation, seconds: f64) -> Result<IUIAutomationElement> {
    let deadline = Instant::now() + Duration::from_secs_f64(seconds);
    loop {
        if let Ok(window) = setup_window(automation) {
            return Ok(window);
        }
        if Instant::now() >= deadline {
            bail!("no Setup window appeared");
        }
        sleep(Duration::from_millis(250));
    }
}

/// The first descendant whose name matches; thmutil controls surface as panes
/// with the window text as Name, so a class filter separates a real control
/// from a label that shares its text.
fn find_control(
    automation: &IUIAutomation,
    text: &str,
    class: Option<&str>,
) -> Option<IUIAutomationElement> {
    let window = setup_window(automation).ok()?;
    let all =
        unsafe { window.FindAll(TreeScope_Descendants, &true_condition(automation).ok()?) }.ok()?;
    let length = unsafe { all.Length() }.ok()?;
    for index in 0..length {
        let Ok(element) = (unsafe { all.GetElement(index) }) else {
            continue;
        };
        let Ok(name) = (unsafe { element.CurrentName() }) else {
            continue;
        };
        if name.to_string().replace('&', "") != text {
            continue;
        }
        if let Some(class) = class {
            let Ok(actual) = (unsafe { element.CurrentClassName() }) else {
                continue;
            };
            if actual != class {
                continue;
            }
        }
        return Some(element);
    }
    None
}

fn wait_control(
    automation: &IUIAutomation,
    text: &str,
    class: Option<&str>,
    seconds: f64,
) -> Result<IUIAutomationElement> {
    let deadline = Instant::now() + Duration::from_secs_f64(seconds);
    loop {
        if let Some(control) = find_control(automation, text, class) {
            return Ok(control);
        }
        if Instant::now() >= deadline {
            let dump = std::env::current_dir()
                .unwrap_or_else(|_| PathBuf::from("."))
                .join("setup-ui-controls.txt");
            write_control_dump(automation, &dump);
            bail!("control '{text}' did not appear");
        }
        sleep(Duration::from_millis(250));
    }
}

fn name_condition(automation: &IUIAutomation, name: &str) -> Result<IUIAutomationCondition> {
    let value = VARIANT::from(BSTR::from(name));
    unsafe { automation.CreatePropertyCondition(UIA_NamePropertyId, &value) }
        .with_context(|| format!("create a condition for {name}"))
}

fn true_condition(automation: &IUIAutomation) -> Result<IUIAutomationCondition> {
    unsafe { automation.CreateTrueCondition() }.context("create the always-true condition")
}

fn write_control_dump(automation: &IUIAutomation, path: &Path) {
    let Ok(window) = setup_window(automation) else {
        let _ = fs::write(path, "no Setup window\n");
        return;
    };
    let Ok(condition) = true_condition(automation) else {
        return;
    };
    let Ok(all) = (unsafe { window.FindAll(TreeScope_Descendants, &condition) }) else {
        return;
    };
    let Ok(length) = (unsafe { all.Length() }) else {
        return;
    };
    let mut lines = Vec::new();
    for index in 0..length {
        let Ok(element) = (unsafe { all.GetElement(index) }) else {
            continue;
        };
        let name = unsafe { element.CurrentName() }
            .map(|n| n.to_string())
            .unwrap_or_default();
        let class = unsafe { element.CurrentClassName() }
            .map(|c| c.to_string())
            .unwrap_or_default();
        let hwnd = unsafe { element.CurrentNativeWindowHandle() }
            .map(|h| format!("0x{:X}", h.0 as usize))
            .unwrap_or_else(|_| "0x0".into());
        let enabled = enabled(&element);
        let offscreen = unsafe { element.CurrentIsOffscreen() }
            .map(|b| b.as_bool())
            .unwrap_or(false);
        lines.push(format!("{class}\t{hwnd}\t{enabled}\t{offscreen}\t{name}"));
    }
    let _ = fs::write(path, lines.join("\n") + "\n");
}

fn hwnd_of(element: &IUIAutomationElement) -> HWND {
    unsafe { element.CurrentNativeWindowHandle() }.unwrap_or_default()
}

fn enabled(element: &IUIAutomationElement) -> bool {
    let via_uia = unsafe { element.CurrentIsEnabled() }
        .map(|value| value.as_bool())
        .unwrap_or(false);
    let handle = hwnd_of(element);
    via_uia && (handle.0.is_null() || unsafe { IsWindowEnabled(handle) }.as_bool())
}

fn invoke(element: &IUIAutomationElement) -> Result<()> {
    if let Ok(pattern) =
        unsafe { element.GetCurrentPatternAs::<IUIAutomationInvokePattern>(UIA_InvokePatternId) }
    {
        unsafe { pattern.Invoke() }.context("invoke the control")?;
        return Ok(());
    }
    let handle = hwnd_of(element);
    ensure!(
        !handle.0.is_null(),
        "the control exposes no invoke pattern and no window handle"
    );
    unsafe {
        SendMessageW(handle, BM_CLICK, Some(WPARAM(0)), Some(LPARAM(0)));
    }
    Ok(())
}

fn toggle(element: &IUIAutomationElement) -> Result<()> {
    if let Ok(pattern) =
        unsafe { element.GetCurrentPatternAs::<IUIAutomationTogglePattern>(UIA_TogglePatternId) }
    {
        unsafe { pattern.Toggle() }.context("toggle the checkbox")?;
        sleep(Duration::from_millis(300));
        return Ok(());
    }
    let handle = hwnd_of(element);
    ensure!(
        !handle.0.is_null(),
        "the checkbox exposes no toggle pattern and no window handle"
    );
    unsafe {
        SendMessageW(handle, BM_CLICK, Some(WPARAM(0)), Some(LPARAM(0)));
    }
    sleep(Duration::from_millis(300));
    Ok(())
}

fn check_state(element: &IUIAutomationElement) -> Result<&'static str> {
    if let Ok(pattern) =
        unsafe { element.GetCurrentPatternAs::<IUIAutomationTogglePattern>(UIA_TogglePatternId) }
    {
        let state = unsafe { pattern.CurrentToggleState() }.context("read the toggle state")?;
        return Ok(if state == ToggleState_On { "On" } else { "Off" });
    }
    let handle = hwnd_of(element);
    ensure!(
        !handle.0.is_null(),
        "the checkbox exposes no toggle pattern and no window handle"
    );
    let check = unsafe { SendMessageW(handle, BM_GETCHECK, Some(WPARAM(0)), Some(LPARAM(0))) };
    Ok(if check.0 == BST_CHECKED { "On" } else { "Off" })
}

fn screenshot(automation: &IUIAutomation, directory: &Path, name: &str) -> Result<()> {
    let window = setup_window(automation)?;
    capture(&window, directory, name)
}

fn wait_started(expected: &Path, seconds: f64) -> Result<u32> {
    let deadline = Instant::now() + Duration::from_secs_f64(seconds);
    let expected_text = expected.to_string_lossy();
    loop {
        for (pid, image) in exosnap_processes() {
            if image
                .as_deref()
                .is_some_and(|path| path.to_string_lossy().eq_ignore_ascii_case(&expected_text))
            {
                return Ok(pid);
            }
        }
        if Instant::now() >= deadline {
            bail!("Launch did not start {}", expected.display());
        }
        sleep(Duration::from_millis(500));
    }
}

fn capture(element: &IUIAutomationElement, directory: &Path, name: &str) -> Result<()> {
    let handle = hwnd_of(element);
    ensure!(
        !handle.0.is_null(),
        "the Setup window has no handle to capture"
    );
    let mut rect = RECT::default();
    unsafe { GetWindowRect(handle, &mut rect) }.context("read the Setup window rect")?;
    let width = rect.right - rect.left;
    let height = rect.bottom - rect.top;
    ensure!(
        width > 0 && height > 0,
        "the Setup window has no capture area"
    );
    let path = directory.join(name);
    unsafe {
        let screen = GetDC(None);
        let mem = CreateCompatibleDC(Some(screen));
        let bitmap = CreateCompatibleBitmap(screen, width, height);
        let previous: HGDIOBJ = SelectObject(mem, bitmap.into());
        let _ = PrintWindow(handle, mem, PW_RENDERFULLCONTENT);
        let mut info = BITMAPINFO {
            bmiHeader: BITMAPINFOHEADER {
                biSize: std::mem::size_of::<BITMAPINFOHEADER>() as u32,
                biWidth: width,
                biHeight: -height,
                biPlanes: 1,
                biBitCount: 32,
                biCompression: BI_RGB.0,
                ..Default::default()
            },
            ..Default::default()
        };
        let mut buffer = vec![0u8; (width as usize) * (height as usize) * 4];
        GetDIBits(
            mem,
            bitmap,
            0,
            height as u32,
            Some(buffer.as_mut_ptr().cast()),
            &mut info,
            DIB_RGB_COLORS,
        );
        let _ = SelectObject(mem, previous);
        let _ = DeleteObject(bitmap.into());
        let _ = DeleteDC(mem);
        ReleaseDC(None, screen);
        for pixel in buffer.chunks_exact_mut(4) {
            pixel.swap(0, 2);
            pixel[3] = 255;
        }
        let file = fs::File::create(&path).with_context(|| format!("create {}", path.display()))?;
        let mut encoder =
            png::Encoder::new(std::io::BufWriter::new(file), width as u32, height as u32);
        encoder.set_color(png::ColorType::Rgba);
        encoder.set_depth(png::BitDepth::Eight);
        let mut writer = encoder.write_header().context("write the PNG header")?;
        writer
            .write_image_data(&buffer)
            .context("write the PNG data")?;
    }
    Ok(())
}
