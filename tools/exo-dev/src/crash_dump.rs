//! Reads an ExoSnap crash minidump and resolves the faulting instruction to a
//! source location.
//!
//! The Windows SDK debugging tools (cdb, WinDbg) are not always installed,
//! and without them a minidump is unreadable. This parses the few streams
//! that matter by hand and lets dbghelp.dll, present on every Windows,
//! resolve addresses against the build's PDB. A Release build's linker
//! passes `/DEBUG` by default, so a crash from a local Release build is
//! symbolicated without installing anything.
//!
//! The stack listing is a scan for values that fall inside a loaded module,
//! not a true unwind: entries may be stale frames left on the stack. The
//! faulting instruction is exact; treat everything below it as a lead, not a
//! call chain.
//!
//! Symbol identity is verified, not assumed: every module records the PDB
//! GUID it was linked against (the RSDS record), and the file on disk at the
//! module's path records its own. If they differ, the build tree has been
//! rebuilt since the crash and every function/line attribution would be
//! fiction, so a mismatched module is reported as unreliable instead of
//! printing a confident wrong answer.

use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};

use anyhow::{Context as _, Result};

const MINIDUMP_SIGNATURE: u32 = 0x504D_444D;
const MODULE_LIST: u32 = 4;
const THREAD_LIST: u32 = 3;
const EXCEPTION: u32 = 6;

const MODULE_ENTRY_STRIDE: usize = 108;
const THREAD_ENTRY_STRIDE: usize = 48;
const EXCEPTION_ACCESS_VIOLATION: u32 = 0xC000_0005;

/// The names a crash report keeps stack entries for: the crashing binary
/// itself and the libraries most likely to explain a crash in it.
const STACK_MODULE_PREFIXES: [&str; 3] = ["exosnap", "qt6", "recorder"];

/// The PDB a module was linked against: its GUID and age from the RSDS
/// CodeView debug record, and the path recorded at link time.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PdbIdentity {
    pub guid: String,
    pub age: u32,
    pub pdb_path: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ModuleInfo {
    pub base: u64,
    pub size: u32,
    pub name: String,
    pub timestamp: u32,
    pub cv: Option<PdbIdentity>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AccessKind {
    Read,
    Write,
    Execute,
    Other(u64),
}

impl AccessKind {
    fn from_code(code: u64) -> Self {
        match code {
            0 => AccessKind::Read,
            1 => AccessKind::Write,
            8 => AccessKind::Execute,
            other => AccessKind::Other(other),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct AccessViolation {
    pub kind: AccessKind,
    pub address: u64,
}

/// The AMD64 integer registers captured at the fault, in the order dbghelp's
/// CONTEXT lays them out starting at offset 0x78.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Registers {
    pub rax: u64,
    pub rcx: u64,
    pub rdx: u64,
    pub rbx: u64,
    pub rsp: u64,
    pub rbp: u64,
    pub rsi: u64,
    pub rdi: u64,
    pub r8: u64,
    pub r9: u64,
    pub r10: u64,
    pub r11: u64,
    pub r12: u64,
    pub r13: u64,
    pub r14: u64,
    pub r15: u64,
    pub rip: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ExceptionRecord {
    pub thread_id: u32,
    pub code: u32,
    pub address: u64,
    pub access_violation: Option<AccessViolation>,
    pub registers: Registers,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct ThreadStack {
    thread_id: u32,
    stack_size: u32,
    stack_rva: usize,
}

/// An address resolved against a module's symbols. `reliable` is false when
/// the module's on-disk PDB identity did not match the dump's, in which case
/// every field below `module_offset` describes a different build and must
/// not be trusted.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Symbolication {
    pub address: u64,
    pub module: Option<String>,
    pub module_offset: u64,
    pub function: Option<String>,
    pub function_offset: u64,
    pub file: Option<String>,
    pub line: Option<u32>,
    pub reliable: bool,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct CrashReport {
    pub modules: Vec<ModuleInfo>,
    pub exception: Option<ExceptionRecord>,
    pub faulting_instruction: Option<Symbolication>,
    pub stack: Vec<Symbolication>,
}

fn read_u32(data: &[u8], offset: usize) -> Option<u32> {
    Some(u32::from_le_bytes(
        data.get(offset..offset + 4)?.try_into().ok()?,
    ))
}

fn read_u64(data: &[u8], offset: usize) -> Option<u64> {
    Some(u64::from_le_bytes(
        data.get(offset..offset + 8)?.try_into().ok()?,
    ))
}

fn hex_lower(bytes: &[u8]) -> String {
    bytes.iter().map(|byte| format!("{byte:02x}")).collect()
}

fn utf16le_lossy(bytes: &[u8]) -> String {
    let units: Vec<u16> = bytes
        .chunks_exact(2)
        .map(|pair| u16::from_le_bytes([pair[0], pair[1]]))
        .collect();
    String::from_utf16_lossy(&units)
}

/// A minidump `MINIDUMP_STRING`: a length-prefixed, null-terminated UTF-16LE
/// string, read at a stream's recorded RVA.
fn read_minidump_string(data: &[u8], rva: usize) -> Option<String> {
    let byte_len = read_u32(data, rva)? as usize;
    let bytes = data.get(rva + 4..rva + 4 + byte_len)?;
    Some(utf16le_lossy(bytes))
}

/// The last path component, matching the module names a minidump records
/// (backslash-separated, regardless of the host's own path style).
fn module_short_name(path: &str) -> &str {
    path.rsplit('\\').next().unwrap_or(path)
}

/// The identity in an RSDS CodeView debug record: `b"RSDS"`, a 16-byte GUID,
/// a 4-byte age, then the PDB path the linker recorded, null-terminated.
pub fn cv_record(record: &[u8]) -> Option<PdbIdentity> {
    if record.len() < 24 || &record[0..4] != b"RSDS" {
        return None;
    }
    let guid = hex_lower(&record[4..20]);
    let age = u32::from_le_bytes(record[20..24].try_into().ok()?);
    let path_bytes = &record[24..];
    let end = path_bytes
        .iter()
        .position(|&b| b == 0)
        .unwrap_or(path_bytes.len());
    let pdb_path = String::from_utf8_lossy(&path_bytes[..end]).into_owned();
    Some(PdbIdentity {
        guid,
        age,
        pdb_path,
    })
}

/// The RSDS identity of a PE file's own CodeView debug directory entry, read
/// from the file's on-disk bytes.
pub fn parse_pe_cv_record(pe: &[u8]) -> Option<PdbIdentity> {
    let e_lfanew = read_u32(pe, 0x3C)? as usize;
    let nsections =
        u16::from_le_bytes(pe.get(e_lfanew + 6..e_lfanew + 8)?.try_into().ok()?) as usize;
    let opt_size =
        u16::from_le_bytes(pe.get(e_lfanew + 20..e_lfanew + 22)?.try_into().ok()?) as usize;
    let opt = e_lfanew + 24;

    // DataDirectory[6] is IMAGE_DIRECTORY_ENTRY_DEBUG; PE32+ directories
    // start at opt+112 (the optional header's fixed part ends there on the
    // 64-bit layout, the only one ExoSnap ships).
    let debug_dir_entry = opt + 112 + 6 * 8;
    let dbg_rva = read_u32(pe, debug_dir_entry)?;
    let dbg_size = read_u32(pe, debug_dir_entry + 4)? as usize;
    if dbg_rva == 0 {
        return None;
    }

    let sections = opt + opt_size;
    let file_offset_of = |rva: u32| -> Option<usize> {
        for i in 0..nsections {
            let section = sections + 40 * i;
            let virtual_address = read_u32(pe, section + 12)?;
            let raw_size = read_u32(pe, section + 16)?;
            let raw_pointer = read_u32(pe, section + 20)?;
            if virtual_address <= rva && rva < virtual_address + raw_size {
                return Some((raw_pointer + (rva - virtual_address)) as usize);
            }
        }
        None
    };
    let debug_directory = file_offset_of(dbg_rva)?;

    for i in 0..dbg_size / 28 {
        let entry = debug_directory + 28 * i;
        let debug_type = read_u32(pe, entry + 12)?;
        // IMAGE_DEBUG_TYPE_CODEVIEW.
        if debug_type == 2 {
            let record_size = read_u32(pe, entry + 16)? as usize;
            let record_ptr = read_u32(pe, entry + 24)? as usize;
            return cv_record(pe.get(record_ptr..record_ptr + record_size)?);
        }
    }
    None
}

fn pe_cv_record(path: &Path) -> Option<PdbIdentity> {
    let bytes = std::fs::read(path).ok()?;
    parse_pe_cv_record(&bytes)
}

/// Parses `MINIDUMP_MODULE_LIST` at `rva`: module count, then a fixed-stride
/// array of base address, size, timestamp, name RVA and CodeView location.
pub fn parse_module_list(data: &[u8], rva: usize) -> Vec<ModuleInfo> {
    let mut modules = Vec::new();
    let Some(count) = read_u32(data, rva) else {
        return modules;
    };
    let mut offset = rva + 4;
    for _ in 0..count {
        let Some(entry) = data.get(offset..offset + MODULE_ENTRY_STRIDE) else {
            break;
        };
        let base = u64::from_le_bytes(entry[0..8].try_into().unwrap());
        let size = u32::from_le_bytes(entry[8..12].try_into().unwrap());
        let timestamp = u32::from_le_bytes(entry[16..20].try_into().unwrap());
        let name_rva = u32::from_le_bytes(entry[20..24].try_into().unwrap()) as usize;
        // CvRecord's MINIDUMP_LOCATION_DESCRIPTOR sits after VersionInfo
        // (a 52-byte VS_FIXEDFILEINFO) in MINIDUMP_MODULE.
        let cv_size = u32::from_le_bytes(entry[76..80].try_into().unwrap()) as usize;
        let cv_rva = u32::from_le_bytes(entry[80..84].try_into().unwrap()) as usize;

        let name = read_minidump_string(data, name_rva).unwrap_or_default();
        let cv = (cv_size > 0)
            .then(|| data.get(cv_rva..cv_rva + cv_size))
            .flatten()
            .and_then(cv_record);

        modules.push(ModuleInfo {
            base,
            size,
            name,
            timestamp,
            cv,
        });
        offset += MODULE_ENTRY_STRIDE;
    }
    modules
}

/// Parses `MINIDUMP_THREAD_LIST` at `rva`, keeping only each thread's stack
/// memory location: the rest of `MINIDUMP_THREAD` is not needed here.
fn parse_thread_list(data: &[u8], rva: usize) -> Vec<ThreadStack> {
    let mut threads = Vec::new();
    let Some(count) = read_u32(data, rva) else {
        return threads;
    };
    let mut offset = rva + 4;
    for _ in 0..count {
        let Some(entry) = data.get(offset..offset + THREAD_ENTRY_STRIDE) else {
            break;
        };
        let thread_id = u32::from_le_bytes(entry[0..4].try_into().unwrap());
        let stack_size = u32::from_le_bytes(entry[32..36].try_into().unwrap());
        let stack_rva = u32::from_le_bytes(entry[36..40].try_into().unwrap()) as usize;
        threads.push(ThreadStack {
            thread_id,
            stack_size,
            stack_rva,
        });
        offset += THREAD_ENTRY_STRIDE;
    }
    threads
}

/// The AMD64 CONTEXT's integer registers, 17 contiguous `u64`s starting at
/// `Rax`.
fn parse_registers(data: &[u8], offset: usize) -> Option<Registers> {
    let mut values = [0u64; 17];
    for (i, slot) in values.iter_mut().enumerate() {
        *slot = read_u64(data, offset + i * 8)?;
    }
    Some(Registers {
        rax: values[0],
        rcx: values[1],
        rdx: values[2],
        rbx: values[3],
        rsp: values[4],
        rbp: values[5],
        rsi: values[6],
        rdi: values[7],
        r8: values[8],
        r9: values[9],
        r10: values[10],
        r11: values[11],
        r12: values[12],
        r13: values[13],
        r14: values[14],
        r15: values[15],
        rip: values[16],
    })
}

/// Parses `MINIDUMP_EXCEPTION_STREAM` at `rva`, including the crashed
/// thread's CONTEXT it points to.
fn parse_exception(data: &[u8], rva: usize) -> Option<ExceptionRecord> {
    let thread_id = read_u32(data, rva)?;
    let code = read_u32(data, rva + 8)?;
    let address = read_u64(data, rva + 24)?;
    let number_parameters = read_u32(data, rva + 32)?;
    let param0 = read_u64(data, rva + 40)?;
    let param1 = read_u64(data, rva + 48)?;
    let access_violation = (code == EXCEPTION_ACCESS_VIOLATION && number_parameters >= 2)
        .then_some(AccessViolation {
            kind: AccessKind::from_code(param0),
            address: param1,
        });

    let context_rva = read_u32(data, rva + 164)? as usize;
    // Integer registers start at offset 0x78 (Rax) in the AMD64 CONTEXT.
    let registers = parse_registers(data, context_rva + 0x78)?;

    Some(ExceptionRecord {
        thread_id,
        code,
        address,
        access_violation,
        registers,
    })
}

/// The minidump's stream directory: stream type to (size, RVA), keeping only
/// the first entry of a repeated type.
fn parse_directory(data: &[u8], rva: usize, count: u32) -> BTreeMap<u32, (u32, usize)> {
    let mut streams = BTreeMap::new();
    for i in 0..count as usize {
        let entry = rva + 12 * i;
        let (Some(stream_type), Some(size), Some(stream_rva)) = (
            read_u32(data, entry),
            read_u32(data, entry + 4),
            read_u32(data, entry + 8),
        ) else {
            break;
        };
        streams
            .entry(stream_type)
            .or_insert((size, stream_rva as usize));
    }
    streams
}

struct Header {
    number_of_streams: u32,
    stream_directory_rva: usize,
}

fn parse_header(data: &[u8]) -> Option<Header> {
    let signature = read_u32(data, 0)?;
    if signature != MINIDUMP_SIGNATURE {
        return None;
    }
    Some(Header {
        number_of_streams: read_u32(data, 8)?,
        stream_directory_rva: read_u32(data, 12)? as usize,
    })
}

fn module_for(modules: &[ModuleInfo], addr: u64) -> Option<&ModuleInfo> {
    modules
        .iter()
        .find(|module| module.base <= addr && addr < module.base + u64::from(module.size))
}

fn identities_differ(dump: &PdbIdentity, disk: &PdbIdentity) -> bool {
    (dump.guid.as_str(), dump.age) != (disk.guid.as_str(), disk.age)
}

/// The base addresses of every ExoSnap module whose on-disk PDB identity
/// does not match the one recorded in the dump: the tree was rebuilt since
/// the crash, and their symbols must be reported unreliable rather than
/// resolved.
fn mismatched_modules(modules: &[ModuleInfo], pdb_search_path: &Path) -> BTreeSet<u64> {
    let mut mismatched = BTreeSet::new();
    for module in modules {
        let short = module_short_name(&module.name);
        if !short.to_ascii_lowercase().starts_with("exosnap") {
            continue;
        }
        let mut candidates = Vec::new();
        if !pdb_search_path.as_os_str().is_empty() {
            candidates.push(pdb_search_path.join(short));
        }
        candidates.push(PathBuf::from(&module.name));

        let disk_cv = candidates
            .iter()
            .filter(|path| path.is_file())
            .find_map(|path| pe_cv_record(path));
        if let (Some(dump_cv), Some(disk_cv)) = (&module.cv, &disk_cv)
            && identities_differ(dump_cv, disk_cv)
        {
            mismatched.insert(module.base);
        }
    }
    mismatched
}

#[cfg(windows)]
struct Symbolizer {
    handle: windows::Win32::Foundation::HANDLE,
}

#[cfg(windows)]
impl Symbolizer {
    fn new(pdb_search_path: &Path) -> Result<Self> {
        use windows::Win32::Foundation::HANDLE;
        use windows::Win32::System::Diagnostics::Debug::{
            SYMOPT_DEFERRED_LOADS, SYMOPT_LOAD_LINES, SYMOPT_UNDNAME, SymInitializeW, SymSetOptions,
        };
        use windows::core::{HSTRING, PCWSTR};

        unsafe {
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        }

        let search_path_owner;
        let search_path = if pdb_search_path.as_os_str().is_empty() {
            PCWSTR::null()
        } else {
            search_path_owner = HSTRING::from(pdb_search_path);
            PCWSTR(search_path_owner.as_ptr())
        };

        // dbghelp identifies a process by an opaque handle value: it never
        // dereferences it, only uses it as a key for the symbol tables this
        // call creates. A fixed sentinel serves as well as a real process
        // handle and needs no process to actually exist.
        let handle = HANDLE(0x1234 as *mut core::ffi::c_void);
        unsafe {
            SymInitializeW(handle, search_path, false).context("SymInitializeW failed")?;
        }
        Ok(Self { handle })
    }

    fn load_module(&self, module: &ModuleInfo) {
        use windows::Win32::System::Diagnostics::Debug::SymLoadModuleExW;
        use windows::core::{HSTRING, PCWSTR};

        let name = HSTRING::from(module.name.as_str());
        unsafe {
            SymLoadModuleExW(
                self.handle,
                None,
                PCWSTR(name.as_ptr()),
                PCWSTR::null(),
                module.base,
                module.size,
                None,
                None,
            );
        }
    }

    /// Resolves `addr` against the module it falls in. A module in
    /// `mismatched` returns only the module name and offset: dbghelp may
    /// still answer for it, but the answer would describe a different build.
    fn resolve(
        &self,
        addr: u64,
        modules: &[ModuleInfo],
        mismatched: &BTreeSet<u64>,
    ) -> Symbolication {
        let module = module_for(modules, addr);
        let module_name = module.map(|m| module_short_name(&m.name).to_string());
        let module_offset = module.map_or(0, |m| addr - m.base);

        if module.is_some_and(|m| mismatched.contains(&m.base)) {
            return Symbolication {
                address: addr,
                module: module_name,
                module_offset,
                reliable: false,
                ..Symbolication::default()
            };
        }

        let Some((function, function_offset)) = self.symbol_at(addr) else {
            return Symbolication {
                address: addr,
                module: module_name,
                module_offset,
                reliable: true,
                ..Symbolication::default()
            };
        };
        let (file, line) = self.line_at(addr).unzip();

        Symbolication {
            address: addr,
            module: module_name,
            module_offset,
            function: Some(function),
            function_offset,
            file,
            line,
            reliable: true,
        }
    }

    fn symbol_at(&self, addr: u64) -> Option<(String, u64)> {
        use windows::Win32::System::Diagnostics::Debug::{SYMBOL_INFOW, SymFromAddrW};

        const MAX_NAME_LEN: usize = 1024;
        let header_size = std::mem::size_of::<SYMBOL_INFOW>();
        // SYMBOL_INFOW's Name field is a one-element trailing array: dbghelp
        // writes into whatever memory follows the struct, up to MaxNameLen
        // characters, so the allocation must be sized for that.
        let mut buffer = vec![0u8; header_size + (MAX_NAME_LEN - 1) * 2];
        let symbol = buffer.as_mut_ptr().cast::<SYMBOL_INFOW>();
        let mut displacement = 0u64;
        unsafe {
            (*symbol).SizeOfStruct = header_size as u32;
            (*symbol).MaxNameLen = MAX_NAME_LEN as u32;
            SymFromAddrW(self.handle, addr, Some(&mut displacement), symbol).ok()?;
            let name_ptr = core::ptr::addr_of!((*symbol).Name).cast::<u16>();
            let name_len = ((*symbol).NameLen as usize).min(MAX_NAME_LEN);
            let name = core::slice::from_raw_parts(name_ptr, name_len);
            Some((String::from_utf16_lossy(name), displacement))
        }
    }

    fn line_at(&self, addr: u64) -> Option<(String, u32)> {
        use windows::Win32::System::Diagnostics::Debug::{IMAGEHLP_LINEW64, SymGetLineFromAddrW64};

        let mut line = IMAGEHLP_LINEW64 {
            SizeOfStruct: std::mem::size_of::<IMAGEHLP_LINEW64>() as u32,
            ..Default::default()
        };
        let mut displacement = 0u32;
        unsafe {
            SymGetLineFromAddrW64(self.handle, addr, &mut displacement, &mut line).ok()?;
            let file = line.FileName.to_string().ok()?;
            Some((file, line.LineNumber))
        }
    }
}

#[cfg(windows)]
impl Drop for Symbolizer {
    fn drop(&mut self) {
        use windows::Win32::System::Diagnostics::Debug::SymCleanup;
        unsafe {
            let _ = SymCleanup(self.handle);
        }
    }
}

/// Reads a minidump and resolves its faulting instruction and the crashed
/// thread's stack scan against the modules named in it.
///
/// `pdb_search_path` is tried ahead of each ExoSnap module's own recorded
/// path when locating the on-disk binary to verify PDB identity against; an
/// empty path skips that override. A module whose on-disk identity does not
/// match the dump's is still symbolized (dbghelp answers from whatever it
/// finds), but every field of its `Symbolication` beyond the module name and
/// offset is reported unreliable.
#[cfg(windows)]
pub fn read(dump_path: &Path, pdb_search_path: &Path) -> Result<CrashReport> {
    let data = std::fs::read(dump_path)
        .with_context(|| format!("could not read '{}'", dump_path.display()))?;
    let header = parse_header(&data)
        .with_context(|| format!("'{}' is not a minidump", dump_path.display()))?;
    let streams = parse_directory(&data, header.stream_directory_rva, header.number_of_streams);

    let modules = streams
        .get(&MODULE_LIST)
        .map(|&(_, rva)| parse_module_list(&data, rva))
        .unwrap_or_default();
    let exception = streams
        .get(&EXCEPTION)
        .and_then(|&(_, rva)| parse_exception(&data, rva));
    let threads = streams
        .get(&THREAD_LIST)
        .map(|&(_, rva)| parse_thread_list(&data, rva))
        .unwrap_or_default();

    let mismatched = mismatched_modules(&modules, pdb_search_path);
    let symbolizer = Symbolizer::new(pdb_search_path)?;
    for module in &modules {
        symbolizer.load_module(module);
    }

    let faulting_instruction = exception
        .as_ref()
        .map(|exc| symbolizer.resolve(exc.registers.rip, &modules, &mismatched));

    let mut stack = Vec::new();
    if let Some(exc) = &exception {
        for thread in threads
            .iter()
            .filter(|thread| thread.thread_id == exc.thread_id)
        {
            let Some(bytes) =
                data.get(thread.stack_rva..thread.stack_rva + thread.stack_size as usize)
            else {
                continue;
            };
            for word in bytes.chunks_exact(8) {
                let value = u64::from_le_bytes(word.try_into().unwrap());
                let Some(module) = module_for(&modules, value) else {
                    continue;
                };
                let short = module_short_name(&module.name).to_ascii_lowercase();
                if !STACK_MODULE_PREFIXES
                    .iter()
                    .any(|prefix| short.starts_with(prefix))
                {
                    continue;
                }
                let symbolication = symbolizer.resolve(value, &modules, &mismatched);
                if stack.contains(&symbolication) {
                    continue;
                }
                stack.push(symbolication);
                if stack.len() > 60 {
                    break;
                }
            }
        }
    }

    Ok(CrashReport {
        modules,
        exception,
        faulting_instruction,
        stack,
    })
}

#[cfg(not(windows))]
pub fn read(_dump_path: &Path, _pdb_search_path: &Path) -> Result<CrashReport> {
    anyhow::bail!("reading a crash minidump needs dbghelp.dll and exists only on Windows")
}

fn format_symbolication(symbolication: &Symbolication) -> String {
    let module = symbolication.module.as_deref().unwrap_or("?");
    if !symbolication.reliable {
        return format!(
            "{module}+0x{:x}   [UNRELIABLE: symbols are from a different build]",
            symbolication.module_offset
        );
    }
    let Some(function) = &symbolication.function else {
        return format!("{module}+0x{:x}", symbolication.module_offset);
    };
    let location = match (&symbolication.file, symbolication.line) {
        (Some(file), Some(line)) => format!("   [{file}:{line}]"),
        _ => String::new(),
    };
    format!(
        "{module}!{function}+0x{:x}{location}",
        symbolication.function_offset
    )
}

/// Renders a crash report the way `read-crash-dump` prints it on the
/// console: the exception, the faulting instruction, the crashed thread's
/// stack scan, and the modules whose PDB identity did not verify.
pub fn render(report: &CrashReport) -> String {
    let mut out = String::new();
    if let Some(exception) = &report.exception {
        out.push_str(&format!("thread id      : {}\n", exception.thread_id));
        out.push_str(&format!("exception code : 0x{:08X}\n", exception.code));
        out.push_str(&format!("exception addr : 0x{:016X}\n", exception.address));
        if let Some(access) = &exception.access_violation {
            let kind = match access.kind {
                AccessKind::Read => "read".to_string(),
                AccessKind::Write => "write".to_string(),
                AccessKind::Execute => "execute".to_string(),
                AccessKind::Other(code) => format!("op={code}"),
            };
            out.push_str(&format!(
                "access violation: {kind} at 0x{:016X}\n",
                access.address
            ));
        }
    }
    if let Some(faulting) = &report.faulting_instruction {
        out.push_str("\nFAULTING INSTRUCTION\n");
        out.push_str(&format!("  {}\n", format_symbolication(faulting)));
    }
    if !report.stack.is_empty() {
        out.push_str("\nSTACK\n");
        for entry in &report.stack {
            out.push_str(&format!("  {}\n", format_symbolication(entry)));
        }
    }
    let unreliable: Vec<&str> = report
        .modules
        .iter()
        .filter(|m| m.name.to_ascii_lowercase().contains("exosnap"))
        .map(|m| module_short_name(&m.name))
        .collect();
    if !unreliable.is_empty() {
        out.push_str("\nMODULES\n");
        for name in unreliable {
            out.push_str(&format!("  {name}\n"));
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn utf16le(text: &str) -> Vec<u8> {
        text.encode_utf16().flat_map(u16::to_le_bytes).collect()
    }

    #[test]
    fn cv_record_parses_a_valid_rsds_record() {
        let mut record = Vec::new();
        record.extend_from_slice(b"RSDS");
        record.extend_from_slice(&[0x11; 16]);
        record.extend_from_slice(&7u32.to_le_bytes());
        record.extend_from_slice(b"C:\\build\\app.pdb\0");

        let identity = cv_record(&record).unwrap();
        assert_eq!(identity.guid, "11".repeat(16));
        assert_eq!(identity.age, 7);
        assert_eq!(identity.pdb_path, "C:\\build\\app.pdb");
    }

    #[test]
    fn cv_record_rejects_a_non_rsds_signature() {
        let mut record = Vec::new();
        record.extend_from_slice(b"BADS");
        record.extend_from_slice(&[0u8; 20]);
        assert!(cv_record(&record).is_none());
    }

    #[test]
    fn cv_record_rejects_a_buffer_too_short_to_hold_a_guid_and_age() {
        assert!(cv_record(b"RSDS").is_none());
    }

    #[test]
    fn parse_pe_cv_record_reads_the_debug_directory() {
        let mut pe = vec![0u8; 4096];
        let e_lfanew: u32 = 0x80;
        pe[0x3C..0x40].copy_from_slice(&e_lfanew.to_le_bytes());
        let p = e_lfanew as usize;
        pe[p..p + 4].copy_from_slice(b"PE\0\0");
        pe[p + 6..p + 8].copy_from_slice(&1u16.to_le_bytes()); // NumberOfSections
        let opt_size: u16 = 200;
        pe[p + 20..p + 22].copy_from_slice(&opt_size.to_le_bytes());
        let opt = p + 24;

        let debug_dir_entry = opt + 112 + 6 * 8;
        let dbg_rva: u32 = 0x2000;
        let dbg_size: u32 = 28;
        pe[debug_dir_entry..debug_dir_entry + 4].copy_from_slice(&dbg_rva.to_le_bytes());
        pe[debug_dir_entry + 4..debug_dir_entry + 8].copy_from_slice(&dbg_size.to_le_bytes());

        let sections = opt + opt_size as usize;
        let raw_ptr: u32 = 0x600;
        pe[sections + 12..sections + 16].copy_from_slice(&dbg_rva.to_le_bytes());
        pe[sections + 16..sections + 20].copy_from_slice(&0x1000u32.to_le_bytes());
        pe[sections + 20..sections + 24].copy_from_slice(&raw_ptr.to_le_bytes());

        let entry_off = raw_ptr as usize;
        pe[entry_off + 12..entry_off + 16].copy_from_slice(&2u32.to_le_bytes()); // IMAGE_DEBUG_TYPE_CODEVIEW
        let mut rsds = Vec::new();
        rsds.extend_from_slice(b"RSDS");
        rsds.extend_from_slice(&[0xAB; 16]);
        rsds.extend_from_slice(&3u32.to_le_bytes());
        rsds.extend_from_slice(b"test.pdb\0");
        let record_off: u32 = 0x700;
        pe[entry_off + 16..entry_off + 20].copy_from_slice(&(rsds.len() as u32).to_le_bytes());
        pe[entry_off + 24..entry_off + 28].copy_from_slice(&record_off.to_le_bytes());
        pe[record_off as usize..record_off as usize + rsds.len()].copy_from_slice(&rsds);

        let identity = parse_pe_cv_record(&pe).unwrap();
        assert_eq!(identity.guid, "ab".repeat(16));
        assert_eq!(identity.age, 3);
        assert_eq!(identity.pdb_path, "test.pdb");
    }

    #[test]
    fn parse_pe_cv_record_returns_none_without_a_debug_directory() {
        let mut pe = vec![0u8; 4096];
        let e_lfanew: u32 = 0x80;
        pe[0x3C..0x40].copy_from_slice(&e_lfanew.to_le_bytes());
        let p = e_lfanew as usize;
        pe[p + 6..p + 8].copy_from_slice(&1u16.to_le_bytes());
        pe[p + 20..p + 22].copy_from_slice(&200u16.to_le_bytes());
        // dbg_rva stays zero.
        assert!(parse_pe_cv_record(&pe).is_none());
    }

    #[test]
    fn parse_module_list_reads_base_size_name_and_pdb_identity() {
        let mut data = vec![0u8; 4096];
        data[0..4].copy_from_slice(&1u32.to_le_bytes());
        let entry = 4;
        let base: u64 = 0x1_4000_0000;
        let size: u32 = 0x2000;
        let timestamp: u32 = 0x6600_0000;
        let name_rva: u32 = 3000;
        data[entry..entry + 8].copy_from_slice(&base.to_le_bytes());
        data[entry + 8..entry + 12].copy_from_slice(&size.to_le_bytes());
        data[entry + 16..entry + 20].copy_from_slice(&timestamp.to_le_bytes());
        data[entry + 20..entry + 24].copy_from_slice(&name_rva.to_le_bytes());

        let name = utf16le("exosnap.exe");
        data[name_rva as usize..name_rva as usize + 4]
            .copy_from_slice(&(name.len() as u32).to_le_bytes());
        data[name_rva as usize + 4..name_rva as usize + 4 + name.len()].copy_from_slice(&name);

        let cv_rva: u32 = 3200;
        let mut rsds = Vec::new();
        rsds.extend_from_slice(b"RSDS");
        rsds.extend_from_slice(&[0x11; 16]);
        rsds.extend_from_slice(&5u32.to_le_bytes());
        rsds.extend_from_slice(b"exosnap.pdb\0");
        data[cv_rva as usize..cv_rva as usize + rsds.len()].copy_from_slice(&rsds);
        data[entry + 76..entry + 80].copy_from_slice(&(rsds.len() as u32).to_le_bytes());
        data[entry + 80..entry + 84].copy_from_slice(&cv_rva.to_le_bytes());

        let modules = parse_module_list(&data, 0);
        assert_eq!(modules.len(), 1);
        let module = &modules[0];
        assert_eq!(module.base, base);
        assert_eq!(module.size, size);
        assert_eq!(module.timestamp, timestamp);
        assert_eq!(module.name, "exosnap.exe");
        let cv = module.cv.as_ref().unwrap();
        assert_eq!(cv.age, 5);
        assert_eq!(cv.pdb_path, "exosnap.pdb");
    }

    #[test]
    fn parse_module_list_leaves_cv_none_when_the_record_is_absent() {
        let mut data = vec![0u8; 200];
        data[0..4].copy_from_slice(&1u32.to_le_bytes());
        // cv_size stays zero; base/size/name_rva/timestamp are irrelevant here.
        let modules = parse_module_list(&data, 0);
        assert_eq!(modules.len(), 1);
        assert!(modules[0].cv.is_none());
    }

    #[test]
    fn parse_module_list_stops_at_a_truncated_buffer_instead_of_panicking() {
        let mut data = vec![0u8; 10];
        data[0..4].copy_from_slice(&5u32.to_le_bytes());
        assert!(parse_module_list(&data, 0).is_empty());
    }

    #[test]
    fn parse_thread_list_reads_thread_id_and_stack_range() {
        let mut data = vec![0u8; 200];
        data[0..4].copy_from_slice(&1u32.to_le_bytes());
        let entry = 4;
        data[entry..entry + 4].copy_from_slice(&42u32.to_le_bytes());
        data[entry + 32..entry + 36].copy_from_slice(&64u32.to_le_bytes());
        data[entry + 36..entry + 40].copy_from_slice(&100u32.to_le_bytes());

        let threads = parse_thread_list(&data, 0);
        assert_eq!(threads.len(), 1);
        assert_eq!(threads[0].thread_id, 42);
        assert_eq!(threads[0].stack_size, 64);
        assert_eq!(threads[0].stack_rva, 100);
    }

    #[test]
    fn parse_exception_reads_access_violation_and_registers() {
        let mut data = vec![0u8; 500];
        let rva = 0;
        data[rva..rva + 4].copy_from_slice(&7u32.to_le_bytes()); // thread id
        data[rva + 8..rva + 12].copy_from_slice(&EXCEPTION_ACCESS_VIOLATION.to_le_bytes());
        data[rva + 24..rva + 32].copy_from_slice(&0x1400_1000u64.to_le_bytes()); // address
        data[rva + 32..rva + 36].copy_from_slice(&2u32.to_le_bytes()); // number of parameters
        data[rva + 40..rva + 48].copy_from_slice(&1u64.to_le_bytes()); // write access
        data[rva + 48..rva + 56].copy_from_slice(&0x99u64.to_le_bytes()); // faulting address
        let context_rva: u32 = 200;
        data[rva + 164..rva + 168].copy_from_slice(&context_rva.to_le_bytes());
        for (i, value) in (1u64..=17).enumerate() {
            let offset = context_rva as usize + 0x78 + i * 8;
            data[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
        }

        let exception = parse_exception(&data, rva).unwrap();
        assert_eq!(exception.thread_id, 7);
        assert_eq!(exception.code, EXCEPTION_ACCESS_VIOLATION);
        assert_eq!(exception.address, 0x1400_1000);
        assert_eq!(
            exception.access_violation,
            Some(AccessViolation {
                kind: AccessKind::Write,
                address: 0x99
            })
        );
        assert_eq!(exception.registers.rax, 1);
        assert_eq!(exception.registers.rip, 17);
    }

    #[test]
    fn parse_exception_omits_access_violation_for_other_codes() {
        let mut data = vec![0u8; 500];
        data[8..12].copy_from_slice(&0xC000_001Du32.to_le_bytes()); // ILLEGAL_INSTRUCTION
        data[32..36].copy_from_slice(&2u32.to_le_bytes());
        let context_rva: u32 = 200;
        data[164..168].copy_from_slice(&context_rva.to_le_bytes());

        let exception = parse_exception(&data, 0).unwrap();
        assert!(exception.access_violation.is_none());
    }

    #[test]
    fn parse_header_rejects_a_bad_signature() {
        let data = vec![0u8; 32];
        assert!(parse_header(&data).is_none());
    }

    #[test]
    fn parse_header_reads_stream_directory_location() {
        let mut data = vec![0u8; 32];
        data[0..4].copy_from_slice(&MINIDUMP_SIGNATURE.to_le_bytes());
        data[8..12].copy_from_slice(&3u32.to_le_bytes());
        data[12..16].copy_from_slice(&16u32.to_le_bytes());

        let header = parse_header(&data).unwrap();
        assert_eq!(header.number_of_streams, 3);
        assert_eq!(header.stream_directory_rva, 16);
    }

    #[test]
    fn parse_directory_keeps_the_first_entry_of_a_repeated_stream_type() {
        let mut data = vec![0u8; 100];
        let rva = 0;
        data[0..4].copy_from_slice(&MODULE_LIST.to_le_bytes());
        data[4..8].copy_from_slice(&10u32.to_le_bytes());
        data[8..12].copy_from_slice(&20u32.to_le_bytes());
        data[12..16].copy_from_slice(&MODULE_LIST.to_le_bytes());
        data[16..20].copy_from_slice(&99u32.to_le_bytes());
        data[20..24].copy_from_slice(&99u32.to_le_bytes());

        let streams = parse_directory(&data, rva, 2);
        assert_eq!(streams.get(&MODULE_LIST), Some(&(10, 20)));
    }

    #[test]
    fn module_for_finds_the_module_containing_an_address() {
        let modules = vec![ModuleInfo {
            base: 0x1000,
            size: 0x100,
            name: "exosnap.exe".to_string(),
            timestamp: 0,
            cv: None,
        }];
        assert_eq!(
            module_for(&modules, 0x1050).map(|m| m.name.as_str()),
            Some("exosnap.exe")
        );
        assert!(module_for(&modules, 0x2000).is_none());
    }

    #[test]
    fn module_short_name_takes_the_last_backslash_component() {
        assert_eq!(module_short_name("C:\\build\\exosnap.exe"), "exosnap.exe");
        assert_eq!(module_short_name("exosnap.exe"), "exosnap.exe");
    }

    #[test]
    fn identities_differ_compares_guid_and_age_together() {
        let a = PdbIdentity {
            guid: "abc".to_string(),
            age: 1,
            pdb_path: String::new(),
        };
        let same = PdbIdentity {
            guid: "abc".to_string(),
            age: 1,
            pdb_path: "different.pdb".to_string(),
        };
        let different_age = PdbIdentity {
            guid: "abc".to_string(),
            age: 2,
            pdb_path: String::new(),
        };
        assert!(!identities_differ(&a, &same));
        assert!(identities_differ(&a, &different_age));
    }

    // A full read() run needs a real Windows minidump file, which these unit
    // tests do not construct; manual verification against a real crash
    // remains the practical path, the same way WinDbg/cdb are the practical
    // alternative to this tool.
    #[test]
    #[ignore]
    fn read_symbolicates_a_real_minidump() {
        let report = read(Path::new("needs-a-real-minidump.dmp"), Path::new("")).unwrap();
        assert!(!report.modules.is_empty());
    }
}
