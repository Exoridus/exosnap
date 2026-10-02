//! Which processes hold a directory tree while another process tries to move
//! it. Diagnostic only: the result is evidence and never decides a verdict.
//!
//! A process holds a tree for a rename when its image or its current
//! directory lies inside it. Both are read without opening anything in the
//! tree, so the watch cannot itself cause the failure it is observing. Plain
//! file handles held by other processes are not visible here; a failed move
//! with no process listed points at those.

use serde_json::{Value, json};
use std::collections::BTreeMap;
use std::path::Path;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use windows::Wdk::System::Threading::{NtQueryInformationProcess, ProcessBasicInformation};
use windows::Win32::Foundation::{CloseHandle, HANDLE};
use windows::Win32::System::Diagnostics::Debug::ReadProcessMemory;
use windows::Win32::System::Diagnostics::ToolHelp::{
    CreateToolhelp32Snapshot, PROCESSENTRY32W, Process32FirstW, Process32NextW, TH32CS_SNAPPROCESS,
};
use windows::Win32::System::Threading::{
    OpenProcess, PROCESS_NAME_WIN32, PROCESS_QUERY_LIMITED_INFORMATION, PROCESS_VM_READ,
    QueryFullProcessImageNameW,
};
use windows::core::PWSTR;

use crate::model::now_rfc3339;

const INTERVAL: Duration = Duration::from_millis(10);
// A forgotten watch must not sample for the rest of the lane.
const MAX_DURATION: Duration = Duration::from_secs(900);

pub struct HolderWatch {
    stop: Arc<AtomicBool>,
    thread: Option<JoinHandle<Value>>,
}

impl HolderWatch {
    /// Starts sampling. `owner` is the process whose exit starts the window
    /// that matters; its children are reported even when they hold nothing.
    pub fn start(tree: &Path, owner: u32) -> Self {
        let stop = Arc::new(AtomicBool::new(false));
        let prefix = tree_prefix(tree);
        let flag = Arc::clone(&stop);
        let thread = std::thread::spawn(move || sample(&prefix, owner, &flag));
        Self {
            stop,
            thread: Some(thread),
        }
    }

    pub fn finish(mut self) -> Value {
        self.stop.store(true, Ordering::SeqCst);
        self.thread
            .take()
            .and_then(|thread| thread.join().ok())
            .unwrap_or_else(|| json!({"error": "the holder watch thread panicked"}))
    }
}

impl Drop for HolderWatch {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::SeqCst);
    }
}

struct Seen {
    parent: u32,
    name: String,
    image: Option<String>,
    cwd: Option<String>,
    first_seen: String,
    gone: Option<(String, Instant)>,
}

fn sample(prefix: &str, owner: u32, stop: &AtomicBool) -> Value {
    let started = Instant::now();
    let mut seen: BTreeMap<u32, Seen> = BTreeMap::new();
    let mut owner_exit: Option<(String, Instant)> = None;
    let mut samples = 0u64;
    let mut errors = Vec::new();
    while !stop.load(Ordering::SeqCst) && started.elapsed() < MAX_DURATION {
        let entries = match processes() {
            Ok(entries) => entries,
            Err(error) => {
                if errors.len() < 5 {
                    errors.push(error.to_string());
                }
                std::thread::sleep(INTERVAL);
                continue;
            }
        };
        samples += 1;
        let now = now_rfc3339();
        let live: std::collections::BTreeSet<u32> = entries.iter().map(|e| e.0).collect();
        for (pid, parent, name) in entries {
            seen.entry(pid).or_insert_with(|| {
                let (image, cwd) = inspect(pid);
                Seen {
                    parent,
                    name,
                    image,
                    cwd,
                    first_seen: now.clone(),
                    gone: None,
                }
            });
        }
        for (pid, entry) in &mut seen {
            if entry.gone.is_none() && !live.contains(pid) {
                entry.gone = Some((now.clone(), Instant::now()));
            }
        }
        if owner_exit.is_none() && !live.contains(&owner) {
            owner_exit = Some((now, Instant::now()));
        }
        std::thread::sleep(INTERVAL);
    }
    let processes: Vec<Value> = seen
        .iter()
        .filter_map(|(pid, entry)| {
            let holds = holds(prefix, entry.image.as_deref(), entry.cwd.as_deref());
            if holds.is_empty() && entry.parent != owner && *pid != owner {
                return None;
            }
            let gone_after_owner_ms = match (&entry.gone, &owner_exit) {
                (Some((_, gone)), Some((_, exit))) => Some(signed_ms(*gone, *exit)),
                _ => None,
            };
            Some(json!({
                "pid": pid,
                "parentPid": entry.parent,
                "name": entry.name,
                "image": entry.image,
                "cwd": entry.cwd,
                "holds": holds,
                "firstSeen": entry.first_seen,
                "gone": entry.gone.as_ref().map(|(at, _)| at.clone()),
                "goneAfterOwnerExitMs": gone_after_owner_ms,
            }))
        })
        .collect();
    json!({
        "tree": prefix,
        "ownerPid": owner,
        "ownerExit": owner_exit.map(|(at, _)| at),
        "intervalMs": INTERVAL.as_millis() as u64,
        "samples": samples,
        "processes": processes,
        "errors": errors,
    })
}

/// The processes the Restart Manager reports as using any file in `tree`.
///
/// Only call this after the move being diagnosed has already failed: the
/// query opens the registered files, and an open file inside a directory is
/// itself a reason for that directory's rename to fail.
pub fn restart_manager_users(tree: &Path) -> Value {
    use windows::Win32::Foundation::{ERROR_MORE_DATA, ERROR_SUCCESS};
    use windows::Win32::System::RestartManager::{
        CCH_RM_SESSION_KEY, RM_PROCESS_INFO, RmEndSession, RmGetList, RmRegisterResources,
        RmStartSession,
    };
    use windows::core::PCWSTR;

    // A package has a few hundred files; the cap only guards a wrong root.
    const MAX_FILES: usize = 4096;
    let mut files = Vec::new();
    let mut pending = vec![tree.to_path_buf()];
    while let Some(dir) = pending.pop() {
        let Ok(entries) = std::fs::read_dir(&dir) else {
            continue;
        };
        for entry in entries.flatten() {
            let path = entry.path();
            match entry.file_type() {
                Ok(kind) if kind.is_dir() => pending.push(path),
                Ok(_) if files.len() < MAX_FILES => files.push(path),
                _ => {}
            }
        }
    }
    let at = now_rfc3339();
    let wide: Vec<Vec<u16>> = files
        .iter()
        .map(|path| {
            path.as_os_str()
                .to_string_lossy()
                .encode_utf16()
                .chain(std::iter::once(0))
                .collect()
        })
        .collect();
    let names: Vec<PCWSTR> = wide.iter().map(|name| PCWSTR(name.as_ptr())).collect();

    let mut session = 0u32;
    let mut key = [0u16; CCH_RM_SESSION_KEY as usize + 1];
    let started = unsafe { RmStartSession(&mut session, None, PWSTR(key.as_mut_ptr())) };
    if started != ERROR_SUCCESS {
        return json!({"at": at, "error": format!("RmStartSession failed with {}", started.0)});
    }
    let result = (|| {
        let registered = unsafe { RmRegisterResources(session, Some(&names), None, None) };
        if registered != ERROR_SUCCESS {
            return Err(format!("RmRegisterResources failed with {}", registered.0));
        }
        let mut infos: Vec<RM_PROCESS_INFO> = Vec::new();
        for _ in 0..4 {
            let mut needed = 0u32;
            let mut count = infos.len() as u32;
            let mut reasons = 0u32;
            let listed = unsafe {
                RmGetList(
                    session,
                    &mut needed,
                    &mut count,
                    Some(infos.as_mut_ptr()),
                    &mut reasons,
                )
            };
            if listed == ERROR_SUCCESS {
                infos.truncate(count as usize);
                return Ok(infos);
            }
            if listed != ERROR_MORE_DATA {
                return Err(format!("RmGetList failed with {}", listed.0));
            }
            infos.resize(needed as usize + 4, RM_PROCESS_INFO::default());
        }
        Err("RmGetList kept growing".to_string())
    })();
    unsafe {
        let _ = RmEndSession(session);
    }
    match result {
        Ok(infos) => {
            let users: Vec<Value> = infos
                .iter()
                .map(|info| {
                    let pid = info.Process.dwProcessId;
                    let (image, cwd) = inspect(pid);
                    json!({
                        "pid": pid,
                        "appName": utf16_until_nul(&info.strAppName),
                        "service": utf16_until_nul(&info.strServiceShortName),
                        "appType": info.ApplicationType.0,
                        "image": image,
                        "cwd": cwd,
                    })
                })
                .collect();
            json!({"at": at, "files": files.len(), "users": users})
        }
        Err(error) => json!({"at": at, "files": files.len(), "error": error}),
    }
}

fn utf16_until_nul(units: &[u16]) -> String {
    let end = units
        .iter()
        .position(|unit| *unit == 0)
        .unwrap_or(units.len());
    String::from_utf16_lossy(&units[..end])
}

/// Milliseconds from `origin` to `at`, negative when `at` came first.
fn signed_ms(at: Instant, origin: Instant) -> i64 {
    match at.checked_duration_since(origin) {
        Some(after) => after.as_millis() as i64,
        None => -(origin.duration_since(at).as_millis() as i64),
    }
}

/// Normalized, lowercase, with a trailing separator so that a sibling such as
/// `tree.new` is not mistaken for part of `tree`.
fn tree_prefix(tree: &Path) -> String {
    let absolute = std::path::absolute(tree).unwrap_or_else(|_| tree.to_path_buf());
    let mut text = absolute.to_string_lossy().replace('/', "\\").to_lowercase();
    if !text.ends_with('\\') {
        text.push('\\');
    }
    text
}

fn inside(prefix: &str, path: &str) -> bool {
    let mut text = path.replace('/', "\\").to_lowercase();
    if !text.ends_with('\\') {
        text.push('\\');
    }
    text.starts_with(prefix)
}

fn holds(prefix: &str, image: Option<&str>, cwd: Option<&str>) -> Vec<&'static str> {
    let mut out = Vec::new();
    if image.is_some_and(|image| inside(prefix, image)) {
        out.push("image");
    }
    if cwd.is_some_and(|cwd| inside(prefix, cwd)) {
        out.push("cwd");
    }
    out
}

fn processes() -> windows::core::Result<Vec<(u32, u32, String)>> {
    let snapshot = unsafe { CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0) }?;
    let mut out = Vec::new();
    let mut entry = PROCESSENTRY32W {
        dwSize: std::mem::size_of::<PROCESSENTRY32W>() as u32,
        ..Default::default()
    };
    let mut more = unsafe { Process32FirstW(snapshot, &mut entry) }.is_ok();
    while more {
        let end = entry
            .szExeFile
            .iter()
            .position(|unit| *unit == 0)
            .unwrap_or(entry.szExeFile.len());
        out.push((
            entry.th32ProcessID,
            entry.th32ParentProcessID,
            String::from_utf16_lossy(&entry.szExeFile[..end]),
        ));
        more = unsafe { Process32NextW(snapshot, &mut entry) }.is_ok();
    }
    unsafe {
        let _ = CloseHandle(snapshot);
    }
    Ok(out)
}

fn inspect(pid: u32) -> (Option<String>, Option<String>) {
    let Ok(process) = (unsafe {
        OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
            false,
            pid,
        )
    }) else {
        return (None, None);
    };
    let image = image_path(process);
    let cwd = current_directory(process);
    unsafe {
        let _ = CloseHandle(process);
    }
    (image, cwd)
}

fn image_path(process: HANDLE) -> Option<String> {
    let mut buffer = [0u16; 1024];
    let mut length = buffer.len() as u32;
    unsafe {
        QueryFullProcessImageNameW(
            process,
            PROCESS_NAME_WIN32,
            PWSTR(buffer.as_mut_ptr()),
            &mut length,
        )
    }
    .ok()?;
    Some(String::from_utf16_lossy(&buffer[..length as usize]))
}

#[repr(C)]
#[derive(Default)]
struct BasicInformation {
    exit_status: i32,
    peb: usize,
    affinity: usize,
    priority: i32,
    pid: usize,
    parent: usize,
}

#[repr(C)]
#[derive(Default)]
struct UnicodeString {
    length: u16,
    maximum: u16,
    buffer: usize,
}

// The current directory is not in any documented structure. These are the
// stable x64 offsets of PEB.ProcessParameters and of
// RTL_USER_PROCESS_PARAMETERS.CurrentDirectory.DosPath. A 32-bit process's
// own parameter block is elsewhere, so its answer may be stale or absent.
const PEB_PROCESS_PARAMETERS: usize = 0x20;
const PARAMETERS_CURRENT_DIRECTORY: usize = 0x38;

fn current_directory(process: HANDLE) -> Option<String> {
    let mut basic = BasicInformation::default();
    let status = unsafe {
        NtQueryInformationProcess(
            process,
            ProcessBasicInformation,
            (&mut basic as *mut BasicInformation).cast(),
            std::mem::size_of::<BasicInformation>() as u32,
            std::ptr::null_mut(),
        )
    };
    if status.is_err() || basic.peb == 0 {
        return None;
    }
    let parameters: usize = read(process, basic.peb + PEB_PROCESS_PARAMETERS)?;
    let directory: UnicodeString = read(process, parameters + PARAMETERS_CURRENT_DIRECTORY)?;
    let units = usize::from(directory.length) / 2;
    if units == 0 || directory.buffer == 0 {
        return None;
    }
    let mut text = vec![0u16; units];
    unsafe {
        ReadProcessMemory(
            process,
            directory.buffer as *const _,
            text.as_mut_ptr().cast(),
            units * 2,
            None,
        )
    }
    .ok()?;
    Some(String::from_utf16_lossy(&text))
}

fn read<T: Default>(process: HANDLE, address: usize) -> Option<T> {
    let mut value = T::default();
    unsafe {
        ReadProcessMemory(
            process,
            address as *const _,
            (&mut value as *mut T).cast(),
            std::mem::size_of::<T>(),
            None,
        )
    }
    .ok()?;
    Some(value)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::process::Command;

    #[test]
    fn a_sibling_with_the_same_stem_is_not_inside_the_tree() {
        let prefix = tree_prefix(Path::new(r"C:\x\ExoSnap"));
        assert!(inside(&prefix, r"C:\X\exosnap\bin\crashpad_handler.exe"));
        assert!(inside(&prefix, r"C:\x\ExoSnap"));
        assert!(!inside(&prefix, r"C:\x\ExoSnap.new\exosnap.exe"));
        assert!(!inside(&prefix, r"C:\x"));
    }

    #[test]
    fn restart_manager_names_a_process_holding_a_file_in_the_tree() {
        let dir = tempfile::tempdir().unwrap();
        let tree = dir.path().join("ExoSnap");
        std::fs::create_dir_all(tree.join("bin")).unwrap();
        std::fs::write(tree.join("exosnap.exe"), b"x").unwrap();
        let held = std::fs::File::open(tree.join("exosnap.exe")).unwrap();
        let evidence = restart_manager_users(&tree);
        drop(held);
        let pid = std::process::id();
        assert_eq!(evidence["files"], 1, "{evidence:#}");
        assert!(
            evidence["users"]
                .as_array()
                .is_some_and(|users| users.iter().any(|user| user["pid"] == pid)),
            "{evidence:#}"
        );
        let released = restart_manager_users(&tree);
        assert_eq!(released["users"], json!([]), "{released:#}");
    }

    #[test]
    fn reports_a_child_whose_current_directory_is_inside_the_tree() {
        let dir = tempfile::tempdir().unwrap();
        let tree = dir.path().join("ExoSnap");
        std::fs::create_dir_all(&tree).unwrap();
        let watch = HolderWatch::start(&tree, std::process::id());
        let mut child = Command::new("cmd.exe")
            .args(["/d", "/c", "ping -n 2 127.0.0.1 >nul"])
            .current_dir(&tree)
            .spawn()
            .unwrap();
        let pid = child.id();
        child.wait().unwrap();
        std::thread::sleep(Duration::from_millis(100));
        let evidence = watch.finish();
        let entry = evidence["processes"]
            .as_array()
            .unwrap()
            .iter()
            .find(|entry| entry["pid"] == pid)
            .unwrap_or_else(|| panic!("child {pid} missing from {evidence:#}"));
        assert_eq!(entry["holds"], json!(["cwd"]), "{entry:#}");
        assert!(entry["gone"].is_string(), "{entry:#}");
    }
}
