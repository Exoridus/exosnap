"""Read an ExoSnap crash minidump and name the faulting function.

Usage:
    python scripts/read-crash-dump.py <dump.dmp> [symbol_dir]

    python scripts/read-crash-dump.py \
        "%LOCALAPPDATA%/ExoSnap/crashes/exosnap-20260710-083105-3092.dmp" \
        build/windows-x64-release/app/Release

Prints the exception, the faulting instruction resolved to file and line, and the
return addresses found on the crashed thread's stack.

Why this exists: the Windows SDK debugging tools (cdb, WinDbg) are not always
installed, and without them a minidump is unreadable. This parses the few streams
that matter by hand and lets dbghelp.dll -- present on every Windows -- resolve
addresses against the build's PDB. Release builds emit a PDB by default
(the Release link always passes /DEBUG), so a crash from a local Release build is symbolicated
without installing anything.

The stack listing is a scan for values that fall inside a loaded module, not a
true unwind: entries may be stale frames left on the stack. The faulting
instruction is exact; treat everything below it as a lead, not a call chain.

Symbol identity is verified, not assumed: every module records the PDB GUID it
was linked against (the RSDS record), and the file on disk at the module's path
records its own. If they differ, the build tree has been rebuilt since the
crash and every function/line attribution below would be fiction -- the script
says so loudly instead of printing a confident wrong answer.
"""
import ctypes as C
import datetime
import os
import struct
import sys
from ctypes import wintypes

DUMP = sys.argv[1]
SYM_PATH = sys.argv[2] if len(sys.argv) > 2 else ""

data = open(DUMP, "rb").read()

sig, ver, nstreams, dir_rva = struct.unpack_from("<IIII", data, 0)
assert sig == 0x504D444D, "not a minidump"

streams = {}
for i in range(nstreams):
    t, sz, rva = struct.unpack_from("<III", data, dir_rva + 12 * i)
    streams.setdefault(t, (sz, rva))

MODULE_LIST, EXCEPTION, THREAD_LIST, SYSTEM_INFO, MEMORY64_LIST = 4, 6, 3, 7, 9


def read_str(rva):
    (nbytes,) = struct.unpack_from("<I", data, rva)
    return data[rva + 4 : rva + 4 + nbytes].decode("utf-16-le", "replace")


def cv_record(record):
    """(guid_hex, age, pdb_path) from an RSDS CodeView record, or None."""
    if len(record) < 24 or record[:4] != b"RSDS":
        return None
    guid = record[4:20].hex()
    (age,) = struct.unpack_from("<I", record, 20)
    pdb = record[24:].split(b"\x00")[0].decode("utf-8", "replace")
    return guid, age, pdb


def pe_cv_record(path):
    """RSDS identity of the PE file on disk, read from its debug directory."""
    try:
        pe = open(path, "rb").read()
        (e_lfanew,) = struct.unpack_from("<I", pe, 0x3C)
        nsections, opt_size = struct.unpack_from("<HH", pe, e_lfanew + 6)[0], struct.unpack_from("<H", pe, e_lfanew + 20)[0]
        opt = e_lfanew + 24
        # DataDirectory[6] = DEBUG; PE32+ directories start at opt+112
        dbg_rva, dbg_size = struct.unpack_from("<II", pe, opt + 112 + 6 * 8)
        if dbg_rva == 0:
            return None
        # map rva -> file offset via section headers
        sec = opt + opt_size
        def to_off(rva):
            for i in range(nsections):
                s = sec + 40 * i
                va, raw_size, raw_ptr = (
                    struct.unpack_from("<I", pe, s + 12)[0],
                    struct.unpack_from("<I", pe, s + 16)[0],
                    struct.unpack_from("<I", pe, s + 20)[0],
                )
                if va <= rva < va + raw_size:
                    return raw_ptr + (rva - va)
            return None
        off = to_off(dbg_rva)
        if off is None:
            return None
        for i in range(dbg_size // 28):
            dtype, dsize, _, dptr = struct.unpack_from("<IIII", pe, off + 28 * i + 12)
            if dtype == 2:  # IMAGE_DEBUG_TYPE_CODEVIEW
                return cv_record(pe[dptr : dptr + dsize])
        return None
    except (OSError, struct.error):
        return None


# ---- modules ----
modules = []
if MODULE_LIST in streams:
    _, rva = streams[MODULE_LIST]
    (count,) = struct.unpack_from("<I", data, rva)
    off = rva + 4
    for i in range(count):
        base, size, checksum, timestamp, name_rva = struct.unpack_from("<QIIII", data, off)
        # CvRecord locator sits after VersionInfo (52 bytes) in MINIDUMP_MODULE.
        cv_size, cv_rva = struct.unpack_from("<II", data, off + 24 + 52)
        cv = cv_record(data[cv_rva : cv_rva + cv_size]) if cv_size else None
        modules.append((base, size, read_str(name_rva), timestamp, cv))
        off += 108

# ---- exception ----
print("=" * 78)
print("EXCEPTION")
print("=" * 78)
exc_thread = None
rip = rsp = rbp = None
if EXCEPTION in streams:
    _, rva = streams[EXCEPTION]
    thread_id, _align = struct.unpack_from("<II", data, rva)
    code, flags, nested, addr = struct.unpack_from("<IIQQ", data, rva + 8)
    nparams = struct.unpack_from("<I", data, rva + 32)[0]
    params = struct.unpack_from("<15Q", data, rva + 40)
    ctx_size, ctx_rva = struct.unpack_from("<II", data, rva + 160)
    exc_thread = thread_id

    names = {
        0xC0000005: "EXCEPTION_ACCESS_VIOLATION",
        0xC0000374: "STATUS_HEAP_CORRUPTION",
        0xC00000FD: "STATUS_STACK_OVERFLOW",
        0xC000001D: "ILLEGAL_INSTRUCTION",
        0x80000003: "BREAKPOINT",
        0xE06D7363: "C++ exception (MSVC)",
    }
    print(f"thread id      : {thread_id}")
    print(f"exception code : 0x{code:08X}  {names.get(code, '?')}")
    print(f"exception addr : 0x{addr:016X}")
    if code == 0xC0000005 and nparams >= 2:
        kind = {0: "read", 1: "write", 8: "execute"}.get(params[0], f"op={params[0]}")
        print(f"access violation: {kind} at 0x{params[1]:016X}")
        if params[1] < 0x10000:
            print("  -> near-null pointer dereference")

    # AMD64 CONTEXT — integer registers start at offset 0x78 (Rax).
    reg_names = ["rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                 "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "rip"]
    regs = dict(zip(reg_names, struct.unpack_from("<17Q", data, ctx_rva + 0x78)))
    rip, rsp, rbp = regs["rip"], regs["rsp"], regs["rbp"]
    for row in range(0, 16, 4):
        print("  ".join(f"{n:3s}=0x{regs[n]:016X}" for n in reg_names[row : row + 4]))
    print(f"rip=0x{rip:016X}")

# ---- faulting thread stack memory ----
stack_ranges = []
if THREAD_LIST in streams:
    _, rva = streams[THREAD_LIST]
    (count,) = struct.unpack_from("<I", data, rva)
    off = rva + 4
    for i in range(count):
        tid, susp, pri_c, pri, teb, s_start, s_size, s_rva, c_size, c_rva = struct.unpack_from(
            "<IIIIQQII II".replace(" ", ""), data, off
        )
        if tid == exc_thread:
            stack_ranges.append((s_start, s_size, s_rva))
        off += 48

# ---- symbolize with dbghelp ----
dbghelp = C.WinDLL("dbghelp.dll")
kernel32 = C.WinDLL("kernel32.dll")

SYMOPT_UNDNAME = 0x00000002
SYMOPT_DEFERRED_LOADS = 0x00000004
SYMOPT_LOAD_LINES = 0x00000010
dbghelp.SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES)

hproc = C.c_void_p(0x1234)
if not dbghelp.SymInitializeW(hproc, C.c_wchar_p(SYM_PATH or None), False):
    print("SymInitialize failed", kernel32.GetLastError())
    sys.exit(1)


class SYMBOL_INFOW(C.Structure):
    _fields_ = [
        ("SizeOfStruct", wintypes.ULONG),
        ("TypeIndex", wintypes.ULONG),
        ("Reserved", C.c_ulonglong * 2),
        ("Index", wintypes.ULONG),
        ("Size", wintypes.ULONG),
        ("ModBase", C.c_ulonglong),
        ("Flags", wintypes.ULONG),
        ("Value", C.c_ulonglong),
        ("Address", C.c_ulonglong),
        ("Register", wintypes.ULONG),
        ("Scope", wintypes.ULONG),
        ("Tag", wintypes.ULONG),
        ("NameLen", wintypes.ULONG),
        ("MaxNameLen", wintypes.ULONG),
        ("Name", C.c_wchar * 1024),
    ]


class IMAGEHLP_LINEW64(C.Structure):
    _fields_ = [
        ("SizeOfStruct", wintypes.DWORD),
        ("Key", C.c_void_p),
        ("LineNumber", wintypes.DWORD),
        ("FileName", C.c_wchar_p),
        ("Address", C.c_ulonglong),
    ]


# ---- symbol identity check: refuse to lie about a rebuilt tree ----
# dbghelp resolves each module against the file currently on disk at the
# module's recorded path (or in symbol_dir). If that file was rebuilt since
# the crash, its PDB describes a different binary and every name/line printed
# below would be plausible-looking fiction.
mismatched = set()
for base, size, name, ts, cv in modules:
    short = name.split("\\")[-1]
    if not short.lower().startswith("exosnap"):
        continue
    candidates = [name]
    if SYM_PATH:
        candidates.insert(0, os.path.join(SYM_PATH, short))
    disk_cv = next((c for c in (pe_cv_record(p) for p in candidates if os.path.isfile(p)) if c), None)
    linked = datetime.datetime.fromtimestamp(ts, datetime.timezone.utc)
    print()
    print(f"module {short}: linked {linked:%Y-%m-%d %H:%M:%S} UTC")
    if cv:
        print(f"  dump PDB id : {cv[0]} age {cv[1]}")
    if disk_cv:
        print(f"  disk PDB id : {disk_cv[0]} age {disk_cv[1]}")
    if cv and disk_cv and (cv[0], cv[1]) != (disk_cv[0], disk_cv[1]):
        mismatched.add(base)
        print("  !! SYMBOL MISMATCH: the binary on disk is NOT the build that crashed.")
        print("  !! The tree was rebuilt after the crash; function/line attribution")
        print("  !! below is unreliable. Rebuild the crashing commit (module link time")
        print("  !! above narrows it down via git reflog) or analyze raw bytes instead.")

loaded = {}
for base, size, name, ts, cv in modules:
    r = dbghelp.SymLoadModuleExW(hproc, None, C.c_wchar_p(name), None,
                                 C.c_ulonglong(base), wintypes.DWORD(size), None, wintypes.DWORD(0))
    loaded[base] = (name, size)


def module_for(addr):
    for base, size, name, ts, cv in modules:
        if base <= addr < base + size:
            return name, base
    return None, None


def sym(addr):
    s = SYMBOL_INFOW()
    s.SizeOfStruct = 88
    s.MaxNameLen = 1024
    disp = C.c_ulonglong(0)
    name, base = module_for(addr)
    short = name.split("\\")[-1] if name else "?"
    if base in mismatched:
        return f"{short}+0x{addr - base:x}   [UNRELIABLE: symbols are from a different build]"
    if dbghelp.SymFromAddrW(hproc, C.c_ulonglong(addr), C.byref(disp), C.byref(s)):
        line = IMAGEHLP_LINEW64()
        line.SizeOfStruct = C.sizeof(IMAGEHLP_LINEW64)
        ldisp = wintypes.DWORD(0)
        loc = ""
        if dbghelp.SymGetLineFromAddrW64(hproc, C.c_ulonglong(addr), C.byref(ldisp), C.byref(line)):
            loc = f"   [{line.FileName}:{line.LineNumber}]"
        return f"{short}!{s.Name}+0x{disp.value:x}{loc}"
    if base:
        return f"{short}+0x{addr - base:x}"
    return f"0x{addr:016X}"


print()
print("=" * 78)
print("FAULTING INSTRUCTION")
print("=" * 78)
if rip:
    print(f"  {sym(rip)}")

# ---- crude backtrace: scan stack for return addresses into loaded modules ----
print()
print("=" * 78)
print("STACK (return addresses found in the faulting thread's stack)")
print("=" * 78)
seen = []
for s_start, s_size, s_rva in stack_ranges:
    stack = data[s_rva : s_rva + s_size]
    for off in range(0, len(stack) - 8, 8):
        (val,) = struct.unpack_from("<Q", stack, off)
        name, base = module_for(val)
        if not name:
            continue
        short = name.split("\\")[-1].lower()
        if not (short.startswith("exosnap") or short.startswith("qt6") or short.startswith("recorder")):
            continue
        text = sym(val)
        if text in seen:
            continue
        seen.append(text)
        print(f"  {s_start + off:016X}  {text}")
        if len(seen) > 60:
            break

print()
print("=" * 78)
print("MODULES (app only)")
print("=" * 78)
for base, size, name, ts, cv in modules:
    short = name.split("\\")[-1]
    if short.lower().startswith("exosnap"):
        print(f"  {base:016X} +{size:08X}  {name}")
