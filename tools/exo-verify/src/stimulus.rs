//! The stimulus window: the deterministic pattern on a real desktop, with an
//! independent ground-truth log.
//!
//! Every presented frame, beep, cursor position and lifecycle change is logged
//! with its QPC time before or as it happens, from this process. Oracles
//! anchor recordings to this log, never to the product's own timestamps.
//!
//! Commands arrive one per line on stdin: `freeze`, `thaw`, `minimize`,
//! `restore`, `destroy`, `quit`.

use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};
use std::io::Write;
use std::path::PathBuf;

#[derive(clap::Args, Debug, Clone)]
pub struct StimulusArgs {
    /// Ground-truth log (JSON lines).
    #[arg(long)]
    pub log: PathBuf,
    /// Window title; recording scenarios select the window by this text.
    #[arg(long, default_value = "ExoVerify Stimulus")]
    pub title: String,
    /// Monitor device name (\\.\DISPLAYn) or zero-based index.
    #[arg(long)]
    pub monitor: Option<String>,
    /// Cover the whole monitor with a borderless topmost window.
    #[arg(long)]
    pub fullscreen: bool,
    /// Client size in physical pixels when not fullscreen.
    #[arg(long, default_value_t = 1280)]
    pub width: u32,
    #[arg(long, default_value_t = 720)]
    pub height: u32,
    /// Seconds between synchronised flash + beep markers (0 disables).
    #[arg(long, default_value_t = 2.0)]
    pub marker_interval: f64,
    /// Move the cursor along a path over the field and log where it went.
    #[arg(long, value_enum, default_value_t = CursorMode::None)]
    pub cursor: CursorMode,
    /// Cursor shape shown over the window.
    #[arg(long, value_enum, default_value_t = CursorShape::Arrow)]
    pub cursor_shape: CursorShape,
    /// Paint once and never again (a still source).
    #[arg(long)]
    pub still: bool,
    /// Source generation shown in the state barcode.
    #[arg(long, default_value_t = 1)]
    pub generation: u8,
    /// Stop after this many seconds.
    #[arg(long, default_value_t = 600.0)]
    pub seconds: f64,
}

#[derive(clap::ValueEnum, Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum CursorMode {
    None,
    /// A circle across the grey field.
    Circle,
    /// A horizontal sweep across the black invert lane.
    Lane,
}

#[derive(clap::ValueEnum, Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum CursorShape {
    Arrow,
    /// A monochrome cursor whose masks invert what is beneath it.
    Invert,
}

/// One line of the ground-truth log.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(tag = "t", rename_all = "camelCase")]
pub enum LogEvent {
    #[serde(rename_all = "camelCase")]
    Ready {
        pid: u32,
        hwnd: u64,
        monitor: String,
        rect: [i32; 4],
        qpc_frequency: i64,
        dpi: u32,
    },
    #[serde(rename_all = "camelCase")]
    Frame { id: u32, qpc: i64, flash: bool },
    #[serde(rename_all = "camelCase")]
    Beep { qpc: i64 },
    #[serde(rename_all = "camelCase")]
    Cursor { id: u32, x: i32, y: i32, qpc: i64 },
    #[serde(rename_all = "camelCase")]
    State { state: String, qpc: i64 },
    #[serde(rename_all = "camelCase")]
    AudioUnavailable { reason: String },
}

pub struct Log {
    out: std::io::BufWriter<std::fs::File>,
}

impl Log {
    pub fn create(path: &std::path::Path) -> Result<Log> {
        Ok(Log {
            out: std::io::BufWriter::new(std::fs::File::create(path)?),
        })
    }
    pub fn write(&mut self, event: &LogEvent) {
        let _ = serde_json::to_writer(&mut self.out, event);
        let _ = self.out.write_all(b"\n");
        let _ = self.out.flush();
    }
}

pub fn read_log(path: &std::path::Path) -> Result<Vec<LogEvent>> {
    let text = std::fs::read_to_string(path)
        .with_context(|| format!("read stimulus log {}", path.display()))?;
    Ok(text
        .lines()
        .filter_map(|l| serde_json::from_str(l).ok())
        .collect())
}

/// Cursor position for frame `id` in canvas coordinates.
pub fn cursor_at(mode: CursorMode, layout: &crate::pattern::Layout, id: u32) -> Option<(i32, i32)> {
    let f = layout.field();
    match mode {
        CursorMode::None => None,
        CursorMode::Circle => {
            let phase = (id % 240) as f64 / 240.0 * std::f64::consts::TAU;
            let (cx, cy) = f.center();
            let r = (f.w.min(f.h) as f64) * 0.3;
            Some((
                (cx as f64 + r * phase.cos()) as i32,
                (cy as f64 + r * phase.sin()) as i32,
            ))
        }
        CursorMode::Lane => {
            let lane = crate::pattern::invert_lane(layout);
            let span = lane.w.saturating_sub(40).max(1);
            let x = lane.x + 8 + (id % 180) * span / 180;
            Some((x as i32, (lane.y + 4) as i32))
        }
    }
}

#[cfg(windows)]
pub fn run(args: StimulusArgs) -> Result<()> {
    imp::run(args)
}

#[cfg(not(windows))]
pub fn run(_: StimulusArgs) -> Result<()> {
    anyhow::bail!("the stimulus is a Windows window")
}

#[cfg(windows)]
mod imp {
    use super::*;
    use crate::pattern::{self, FrameState, Layout};
    use std::sync::Arc;
    use std::sync::atomic::{AtomicBool, AtomicU8, Ordering};
    use windows::Win32::Foundation::{HWND, LPARAM, LRESULT, POINT, RECT, WPARAM};
    use windows::Win32::Graphics::Dwm::DwmFlush;
    use windows::Win32::Graphics::Gdi::*;
    use windows::Win32::System::LibraryLoader::GetModuleHandleW;
    use windows::Win32::System::Performance::{QueryPerformanceCounter, QueryPerformanceFrequency};
    use windows::Win32::UI::HiDpi::{
        DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2, GetDpiForWindow, SetProcessDpiAwarenessContext,
    };
    use windows::Win32::UI::WindowsAndMessaging::*;
    use windows::core::{HSTRING, PCWSTR, w};

    pub fn qpc() -> i64 {
        let mut v = 0;
        unsafe {
            let _ = QueryPerformanceCounter(&mut v);
        }
        v
    }

    /// The cursor shown over the window, as a raw handle value.
    static CURSOR: std::sync::atomic::AtomicIsize = std::sync::atomic::AtomicIsize::new(0);

    unsafe extern "system" fn wndproc(
        hwnd: HWND,
        msg: u32,
        wparam: WPARAM,
        lparam: LPARAM,
    ) -> LRESULT {
        match msg {
            WM_SETCURSOR => {
                let raw = CURSOR.load(Ordering::SeqCst);
                if raw != 0 {
                    unsafe { SetCursor(Some(HCURSOR(raw as *mut _))) };
                    return LRESULT(1);
                }
                unsafe { DefWindowProcW(hwnd, msg, wparam, lparam) }
            }
            WM_ERASEBKGND => LRESULT(1),
            WM_DESTROY => {
                unsafe { PostQuitMessage(0) };
                LRESULT(0)
            }
            _ => unsafe { DefWindowProcW(hwnd, msg, wparam, lparam) },
        }
    }

    fn monitor_rect(selector: Option<&str>) -> Result<(String, RECT)> {
        unsafe extern "system" fn cb(
            m: HMONITOR,
            _: HDC,
            _: *mut RECT,
            data: LPARAM,
        ) -> windows::core::BOOL {
            let list = unsafe { &mut *(data.0 as *mut Vec<(String, RECT)>) };
            let mut info = MONITORINFOEXW::default();
            info.monitorInfo.cbSize = std::mem::size_of::<MONITORINFOEXW>() as u32;
            if unsafe { GetMonitorInfoW(m, &mut info as *mut _ as *mut MONITORINFO) }.as_bool() {
                let name = String::from_utf16_lossy(&info.szDevice)
                    .trim_end_matches('\0')
                    .to_string();
                list.push((name, info.monitorInfo.rcMonitor));
            }
            true.into()
        }
        let mut list: Vec<(String, RECT)> = Vec::new();
        unsafe {
            let _ = EnumDisplayMonitors(None, None, Some(cb), LPARAM(&mut list as *mut _ as isize));
        }
        anyhow::ensure!(!list.is_empty(), "no monitors");
        let pick = match selector {
            None => list
                .iter()
                .find(|(_, r)| r.left == 0 && r.top == 0)
                .cloned()
                .unwrap_or(list[0].clone()),
            Some(s) => {
                if let Ok(i) = s.parse::<usize>() {
                    list.get(i)
                        .cloned()
                        .with_context(|| format!("no monitor {i}"))?
                } else {
                    list.iter()
                        .find(|(n, _)| n.eq_ignore_ascii_case(s))
                        .cloned()
                        .with_context(|| format!("no monitor {s}"))?
                }
            }
        };
        Ok(pick)
    }

    fn invert_cursor() -> Result<HCURSOR> {
        // AND=1, XOR=1 inverts the screen under every pixel of the 24x24 block.
        let size = 32usize;
        let mut and = vec![0u8; size * size / 8];
        let mut xor = vec![0u8; size * size / 8];
        for y in 0..24 {
            for x in 0..24 {
                let bit = y * size + x;
                and[bit / 8] |= 0x80 >> (bit % 8);
                xor[bit / 8] |= 0x80 >> (bit % 8);
            }
        }
        let instance = unsafe { GetModuleHandleW(None)? };
        Ok(unsafe {
            CreateCursor(
                Some(instance.into()),
                0,
                0,
                size as i32,
                size as i32,
                and.as_ptr() as _,
                xor.as_ptr() as _,
            )?
        })
    }

    pub fn run(args: StimulusArgs) -> Result<()> {
        unsafe {
            let _ = SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
        let mut log = Log::create(&args.log)?;
        let (monitor, mrect) = monitor_rect(args.monitor.as_deref())?;
        let (x, y, w, h) = if args.fullscreen {
            (
                mrect.left,
                mrect.top,
                (mrect.right - mrect.left) as u32,
                (mrect.bottom - mrect.top) as u32,
            )
        } else {
            (mrect.left + 40, mrect.top + 40, args.width, args.height)
        };

        let cursor = match args.cursor_shape {
            CursorShape::Arrow => unsafe { LoadCursorW(None, IDC_ARROW)? },
            CursorShape::Invert => invert_cursor()?,
        };
        CURSOR.store(cursor.0 as isize, Ordering::SeqCst);

        let instance = unsafe { GetModuleHandleW(None)? };
        let class = w!("ExoVerifyStimulus");
        let wc = WNDCLASSW {
            lpfnWndProc: Some(wndproc),
            hInstance: instance.into(),
            lpszClassName: class,
            hCursor: cursor,
            ..Default::default()
        };
        unsafe { RegisterClassW(&wc) };
        let (style, ex) = if args.fullscreen {
            (WS_POPUP | WS_VISIBLE, WS_EX_TOPMOST | WS_EX_TOOLWINDOW)
        } else {
            (WS_OVERLAPPED | WS_CAPTION | WS_VISIBLE, WS_EX_TOPMOST)
        };
        let mut frame = RECT {
            left: 0,
            top: 0,
            right: w as i32,
            bottom: h as i32,
        };
        if !args.fullscreen {
            unsafe {
                let _ = AdjustWindowRectEx(&mut frame, style, false, ex);
            }
        }
        let title = HSTRING::from(args.title.as_str());
        let hwnd = unsafe {
            CreateWindowExW(
                ex,
                class,
                PCWSTR(title.as_ptr()),
                style,
                x,
                y,
                frame.right - frame.left,
                frame.bottom - frame.top,
                None,
                None,
                Some(instance.into()),
                None,
            )?
        };
        let dpi = unsafe { GetDpiForWindow(hwnd) };

        let layout = Layout::new(w, h);
        let hdc = unsafe { GetDC(Some(hwnd)) };
        let mem = unsafe { CreateCompatibleDC(Some(hdc)) };
        let mut info = BITMAPINFO::default();
        info.bmiHeader.biSize = std::mem::size_of::<BITMAPINFOHEADER>() as u32;
        info.bmiHeader.biWidth = w as i32;
        info.bmiHeader.biHeight = -(h as i32);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        let mut bits: *mut core::ffi::c_void = std::ptr::null_mut();
        let dib =
            unsafe { CreateDIBSection(Some(mem), &info, DIB_RGB_COLORS, &mut bits, None, 0)? };
        unsafe { SelectObject(mem, dib.into()) };
        let canvas =
            unsafe { std::slice::from_raw_parts_mut(bits as *mut u8, w as usize * h as usize * 4) };
        pattern::paint_static(&layout, canvas);
        let mut paint = |state: &FrameState, full: bool| {
            pattern::paint_dynamic(&layout, state, canvas);
            let regions: Vec<pattern::Rect> = if full {
                vec![pattern::Rect { x: 0, y: 0, w, h }]
            } else {
                pattern::dynamic_regions(&layout).to_vec()
            };
            for r in regions {
                unsafe {
                    let _ = BitBlt(
                        hdc,
                        r.x as i32,
                        r.y as i32,
                        r.w as i32,
                        r.h as i32,
                        Some(mem),
                        r.x as i32,
                        r.y as i32,
                        SRCCOPY,
                    );
                }
            }
        };

        let mut rect = RECT::default();
        unsafe {
            let _ = GetWindowRect(hwnd, &mut rect);
        }
        let mut freq = 0;
        unsafe {
            let _ = QueryPerformanceFrequency(&mut freq);
        }
        log.write(&LogEvent::Ready {
            pid: std::process::id(),
            hwnd: hwnd.0 as u64,
            monitor: monitor.clone(),
            rect: [rect.left, rect.top, rect.right, rect.bottom],
            qpc_frequency: freq,
            dpi,
        });

        let frozen = Arc::new(AtomicBool::new(args.still));
        let command = Arc::new(AtomicU8::new(0));
        {
            let command = Arc::clone(&command);
            std::thread::spawn(move || {
                let stdin = std::io::stdin();
                let mut line = String::new();
                while stdin.read_line(&mut line).map(|n| n > 0).unwrap_or(false) {
                    let code = match line.trim() {
                        "freeze" => 1,
                        "thaw" => 2,
                        "minimize" => 3,
                        "restore" => 4,
                        "destroy" => 5,
                        "quit" => 6,
                        _ => 0,
                    };
                    command.store(code, Ordering::SeqCst);
                    line.clear();
                }
            });
        }

        let marker_ticks = (args.marker_interval * freq as f64) as i64;
        let start = qpc();
        let audio = if marker_ticks > 0 {
            audio::Beeper::start(start, marker_ticks, freq).map_err(|e| e.to_string())
        } else {
            Err("markers disabled".into())
        };
        if let Err(reason) = &audio {
            log.write(&LogEvent::AudioUnavailable {
                reason: reason.clone(),
            });
        }

        let mut id: u32 = 0;
        let mut next_marker = if marker_ticks > 0 {
            start + marker_ticks
        } else {
            i64::MAX
        };
        let mut state = FrameState {
            frame_id: 0,
            generation: args.generation,
            flash: false,
            flags: 0,
        };
        paint(&state, true);
        let deadline = start + (args.seconds * freq as f64) as i64;
        let mut msg = MSG::default();
        loop {
            while unsafe { PeekMessageW(&mut msg, None, 0, 0, PM_REMOVE) }.as_bool() {
                if msg.message == WM_QUIT {
                    return Ok(());
                }
                unsafe {
                    let _ = TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
            match command.swap(0, Ordering::SeqCst) {
                1 => {
                    frozen.store(true, Ordering::SeqCst);
                    log.write(&LogEvent::State {
                        state: "frozen".into(),
                        qpc: qpc(),
                    });
                }
                2 => {
                    frozen.store(false, Ordering::SeqCst);
                    log.write(&LogEvent::State {
                        state: "thawed".into(),
                        qpc: qpc(),
                    });
                }
                3 => unsafe {
                    let _ = ShowWindow(hwnd, SW_MINIMIZE);
                    log.write(&LogEvent::State {
                        state: "minimized".into(),
                        qpc: qpc(),
                    });
                },
                4 => unsafe {
                    let _ = ShowWindow(hwnd, SW_RESTORE);
                    log.write(&LogEvent::State {
                        state: "restored".into(),
                        qpc: qpc(),
                    });
                },
                5 => unsafe {
                    let _ = DestroyWindow(hwnd);
                    // Ground truth for source-loss scenarios: the window is gone.
                    let gone = !IsWindow(Some(hwnd)).as_bool();
                    log.write(&LogEvent::State {
                        state: if gone {
                            "destroyed".into()
                        } else {
                            "destroy-failed".into()
                        },
                        qpc: qpc(),
                    });
                },
                6 => return Ok(()),
                _ => {}
            }
            if qpc() >= deadline {
                return Ok(());
            }
            if !frozen.load(Ordering::SeqCst) {
                id += 1;
                let now = qpc();
                state.frame_id = id;
                state.flash = now >= next_marker;
                paint(&state, false);
            }
            // DwmFlush returns once the compositor has taken the frame; the QPC
            // after it is the closest a GDI client gets to its present time.
            unsafe {
                let _ = DwmFlush();
            }
            let presented = qpc();
            if !frozen.load(Ordering::SeqCst) {
                log.write(&LogEvent::Frame {
                    id,
                    qpc: presented,
                    flash: state.flash,
                });
                if state.flash {
                    if let Ok(b) = &audio {
                        log.write(&LogEvent::Beep {
                            qpc: b.onset_for(next_marker),
                        });
                    }
                    next_marker += marker_ticks;
                }
            }
            if let Some((cx, cy)) = cursor_at(args.cursor, &layout, id) {
                let (px, py) = (x + cx, y + cy);
                unsafe {
                    let _ = SetCursorPos(px, py);
                }
                let mut p = POINT::default();
                unsafe {
                    let _ = GetCursorPos(&mut p);
                }
                log.write(&LogEvent::Cursor {
                    id,
                    x: p.x - x,
                    y: p.y - y,
                    qpc: qpc(),
                });
            }
        }
    }

    /// Schedules tone bursts on the default render endpoint so that each
    /// burst starts playing at a marker's QPC time, using the endpoint's own
    /// clock to map samples to QPC.
    mod audio {
        use super::qpc;
        use anyhow::{Result, bail};
        use std::sync::atomic::{AtomicBool, Ordering};
        use std::sync::{Arc, Mutex};
        use windows::Win32::Media::Audio::*;
        use windows::Win32::System::Com::*;

        pub struct Beeper {
            /// Measured onset QPC per scheduled marker QPC.
            onsets: Arc<Mutex<Vec<(i64, i64)>>>,
            _stop: Arc<AtomicBool>,
        }

        impl Beeper {
            pub fn onset_for(&self, marker: i64) -> i64 {
                self.onsets
                    .lock()
                    .unwrap()
                    .iter()
                    .find(|(m, _)| *m == marker)
                    .map(|(_, o)| *o)
                    .unwrap_or(marker)
            }

            pub fn start(start: i64, interval: i64, freq: i64) -> Result<Beeper> {
                let onsets = Arc::new(Mutex::new(Vec::new()));
                let stop = Arc::new(AtomicBool::new(false));
                let (tx, rx) = std::sync::mpsc::channel();
                {
                    let onsets = Arc::clone(&onsets);
                    let stop = Arc::clone(&stop);
                    std::thread::spawn(move || {
                        if let Err(e) = play(start, interval, freq, &onsets, &stop, &tx) {
                            let _ = tx.send(Err(e.to_string()));
                        }
                    });
                }
                match rx.recv_timeout(std::time::Duration::from_secs(5)) {
                    Ok(Ok(())) => Ok(Beeper {
                        onsets,
                        _stop: stop,
                    }),
                    Ok(Err(e)) => bail!(e),
                    Err(_) => bail!("audio endpoint did not start"),
                }
            }
        }

        fn play(
            start: i64,
            interval: i64,
            freq: i64,
            onsets: &Mutex<Vec<(i64, i64)>>,
            stop: &AtomicBool,
            ready: &std::sync::mpsc::Sender<Result<(), String>>,
        ) -> Result<()> {
            unsafe {
                let _ = CoInitializeEx(None, COINIT_MULTITHREADED);
                let enumerator: IMMDeviceEnumerator =
                    CoCreateInstance(&MMDeviceEnumerator, None, CLSCTX_ALL)?;
                let device = enumerator.GetDefaultAudioEndpoint(eRender, eConsole)?;
                let client: IAudioClient = device.Activate::<IAudioClient>(CLSCTX_ALL, None)?;
                let format = client.GetMixFormat()?;
                let rate = (*format).nSamplesPerSec;
                let channels = (*format).nChannels as usize;
                if (*format).wBitsPerSample != 32 {
                    bail!("mix format is not 32-bit float");
                }
                client.Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1_000_000, 0, format, None)?;
                let buffer_frames = client.GetBufferSize()?;
                let render: IAudioRenderClient = client.GetService()?;
                let clock: IAudioClock = client.GetService()?;
                let device_freq = clock.GetFrequency()?;
                client.Start()?;
                let _ = ready.send(Ok(()));
                let tone_samples = rate as u64 / 20; // 50 ms
                let mut written: u64 = 0;
                let mut next_marker = start + interval;
                let mut tone_left: u64 = 0;
                let mut phase = 0.0f64;
                while !stop.load(Ordering::SeqCst) {
                    let padding = client.GetCurrentPadding()?;
                    let free = buffer_frames - padding;
                    if free == 0 {
                        std::thread::sleep(std::time::Duration::from_millis(2));
                        continue;
                    }
                    // Map the next unwritten sample to a QPC time via the device clock.
                    let (mut pos, mut pos_qpc) = (0u64, 0u64);
                    clock.GetPosition(&mut pos, Some(&mut pos_qpc))?;
                    let played = pos as f64 / device_freq as f64 * rate as f64;
                    let pos_qpc_ticks = (pos_qpc as f64 / 1e7 * freq as f64) as i64;
                    let data = render.GetBuffer(free)?;
                    let samples =
                        std::slice::from_raw_parts_mut(data as *mut f32, free as usize * channels);
                    for i in 0..free as usize {
                        let sample_index = written + i as u64;
                        let at = pos_qpc_ticks
                            + ((sample_index as f64 - played) / rate as f64 * freq as f64) as i64;
                        if tone_left == 0 && at >= next_marker {
                            tone_left = tone_samples;
                            onsets.lock().unwrap().push((next_marker, at));
                            next_marker += interval;
                        }
                        let v = if tone_left > 0 {
                            tone_left -= 1;
                            phase += 2.0 * std::f64::consts::PI * 1000.0 / rate as f64;
                            (0.5 * phase.sin()) as f32
                        } else {
                            0.0
                        };
                        for c in 0..channels {
                            samples[i * channels + c] = v;
                        }
                    }
                    render.ReleaseBuffer(free, 0)?;
                    written += free as u64;
                    let _ = qpc;
                    std::thread::sleep(std::time::Duration::from_millis(5));
                }
                client.Stop()?;
            }
            Ok(())
        }
    }
}
