//! Synthetic desktop input, centralized for the few scenarios where the
//! gesture itself is the product under test.
//!
//! Everything here is real input: `SendInput` against the interactive desktop,
//! in virtual-screen physical pixels. A scenario that uses this module is
//! desktop-interactive by definition and must declare
//! `Capability::InteractiveDesktop`. Semantic IPC is the rule; this is the
//! exception for drag hit-testing, overlay geometry and commit/cancel keys,
//! which no product command can stand in for without testing a different path.

use std::time::Duration;

/// Drags the primary pointer from `start` to `end` in virtual-screen physical
/// pixels, holds the button through the move, releases, and restores the
/// pointer to where it was. The pause between steps is the drag's own pacing,
/// not a settle delay.
#[cfg(windows)]
pub fn drag_rect(start: (i32, i32), end: (i32, i32), step_pause: Duration) -> anyhow::Result<()> {
    use windows::Win32::Foundation::POINT;
    use windows::Win32::UI::HiDpi::{
        DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2, SetProcessDpiAwarenessContext,
    };
    use windows::Win32::UI::WindowsAndMessaging::GetCursorPos;

    // The coordinates handed to SendInput are physical; a DPI-unaware caller
    // would virtualize them and miss on a scaled display.
    unsafe {
        let _ = SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
    let mut saved = POINT::default();
    let restore = unsafe { GetCursorPos(&mut saved).is_ok() };

    send(&[pointer_move(start)])?;
    std::thread::sleep(step_pause);
    send(&[pointer_button(true)])?;
    std::thread::sleep(step_pause);
    const STEPS: i32 = 16;
    for step in 1..=STEPS {
        let x = start.0 + (end.0 - start.0) * step / STEPS;
        let y = start.1 + (end.1 - start.1) * step / STEPS;
        send(&[pointer_move((x, y))])?;
        std::thread::sleep(step_pause);
    }
    std::thread::sleep(step_pause);
    send(&[pointer_button(false)])?;
    std::thread::sleep(step_pause);

    if restore {
        let _ = send(&[pointer_move((saved.x, saved.y))]);
    }
    Ok(())
}

/// Clicks at one virtual-screen physical point, restoring the pointer after.
#[cfg(windows)]
pub fn click(point: (i32, i32), step_pause: Duration) -> anyhow::Result<()> {
    use windows::Win32::Foundation::POINT;
    use windows::Win32::UI::HiDpi::{
        DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2, SetProcessDpiAwarenessContext,
    };
    use windows::Win32::UI::WindowsAndMessaging::GetCursorPos;

    unsafe {
        let _ = SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
    let mut saved = POINT::default();
    let restore = unsafe { GetCursorPos(&mut saved).is_ok() };
    send(&[pointer_move(point)])?;
    std::thread::sleep(step_pause);
    send(&[pointer_button(true)])?;
    std::thread::sleep(step_pause);
    send(&[pointer_button(false)])?;
    std::thread::sleep(step_pause);
    if restore {
        let _ = send(&[pointer_move((saved.x, saved.y))]);
    }
    Ok(())
}

/// Presses and releases one virtual key (a Windows virtual-key code).
#[cfg(windows)]
pub fn press_key(vk: u16, settle: Duration) -> anyhow::Result<()> {
    use windows::Win32::UI::Input::KeyboardAndMouse::{
        INPUT, INPUT_0, INPUT_KEYBOARD, KEYBDINPUT, KEYEVENTF_KEYUP, VIRTUAL_KEY,
    };
    let down = INPUT {
        r#type: INPUT_KEYBOARD,
        Anonymous: INPUT_0 {
            ki: KEYBDINPUT {
                wVk: VIRTUAL_KEY(vk),
                ..Default::default()
            },
        },
    };
    let up = INPUT {
        r#type: INPUT_KEYBOARD,
        Anonymous: INPUT_0 {
            ki: KEYBDINPUT {
                wVk: VIRTUAL_KEY(vk),
                dwFlags: KEYEVENTF_KEYUP,
                ..Default::default()
            },
        },
    };
    send(&[down, up])?;
    std::thread::sleep(settle);
    Ok(())
}

#[cfg(windows)]
fn pointer_move(point: (i32, i32)) -> windows::Win32::UI::Input::KeyboardAndMouse::INPUT {
    use windows::Win32::UI::Input::KeyboardAndMouse::{
        INPUT, INPUT_0, INPUT_MOUSE, MOUSEEVENTF_ABSOLUTE, MOUSEEVENTF_MOVE,
        MOUSEEVENTF_VIRTUALDESK, MOUSEINPUT,
    };
    use windows::Win32::UI::WindowsAndMessaging::{
        GetSystemMetrics, SM_CXVIRTUALSCREEN, SM_CYVIRTUALSCREEN, SM_XVIRTUALSCREEN,
        SM_YVIRTUALSCREEN,
    };
    let vx = unsafe { GetSystemMetrics(SM_XVIRTUALSCREEN) };
    let vy = unsafe { GetSystemMetrics(SM_YVIRTUALSCREEN) };
    let vw = unsafe { GetSystemMetrics(SM_CXVIRTUALSCREEN) }.max(1);
    let vh = unsafe { GetSystemMetrics(SM_CYVIRTUALSCREEN) }.max(1);
    // 0..65535 spans the virtual desktop; the last mapped pixel is width-1.
    let nx = ((point.0 - vx) as i64 * 65535 / (vw - 1).max(1) as i64) as i32;
    let ny = ((point.1 - vy) as i64 * 65535 / (vh - 1).max(1) as i64) as i32;
    INPUT {
        r#type: INPUT_MOUSE,
        Anonymous: INPUT_0 {
            mi: MOUSEINPUT {
                dx: nx,
                dy: ny,
                mouseData: 0,
                dwFlags: MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK,
                time: 0,
                dwExtraInfo: 0,
            },
        },
    }
}

#[cfg(windows)]
fn pointer_button(down: bool) -> windows::Win32::UI::Input::KeyboardAndMouse::INPUT {
    use windows::Win32::UI::Input::KeyboardAndMouse::{
        INPUT, INPUT_0, INPUT_MOUSE, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, MOUSEINPUT,
    };
    INPUT {
        r#type: INPUT_MOUSE,
        Anonymous: INPUT_0 {
            mi: MOUSEINPUT {
                dx: 0,
                dy: 0,
                mouseData: 0,
                dwFlags: if down {
                    MOUSEEVENTF_LEFTDOWN
                } else {
                    MOUSEEVENTF_LEFTUP
                },
                time: 0,
                dwExtraInfo: 0,
            },
        },
    }
}

#[cfg(windows)]
fn send(inputs: &[windows::Win32::UI::Input::KeyboardAndMouse::INPUT]) -> anyhow::Result<()> {
    use windows::Win32::UI::Input::KeyboardAndMouse::SendInput;
    let sent = unsafe {
        SendInput(
            inputs,
            std::mem::size_of::<windows::Win32::UI::Input::KeyboardAndMouse::INPUT>() as i32,
        )
    };
    anyhow::ensure!(
        sent as usize == inputs.len(),
        "SendInput delivered {sent} of {} events; the desktop may be locked",
        inputs.len()
    );
    Ok(())
}

#[cfg(not(windows))]
pub fn drag_rect(_: (i32, i32), _: (i32, i32), _: Duration) -> anyhow::Result<()> {
    anyhow::bail!("synthetic pointer input requires Windows")
}

#[cfg(not(windows))]
pub fn click(_: (i32, i32), _: Duration) -> anyhow::Result<()> {
    anyhow::bail!("synthetic pointer input requires Windows")
}

#[cfg(not(windows))]
pub fn press_key(_: u16, _: Duration) -> anyhow::Result<()> {
    anyhow::bail!("synthetic keyboard input requires Windows")
}
