//! Minimal PE reader: the statically imported DLL names of an x64 image.
//!
//! Delay-loaded imports are deliberately excluded. They cannot fail process
//! start, so the runtime-dependency audit only judges the static table.

use anyhow::{Result, anyhow, bail, ensure};
use std::path::Path;

struct Section {
    virtual_address: u32,
    virtual_size: u32,
    raw_offset: u32,
    raw_size: u32,
}

fn u16_at(b: &[u8], o: usize) -> Result<u16> {
    b.get(o..o + 2)
        .map(|s| u16::from_le_bytes([s[0], s[1]]))
        .ok_or_else(|| anyhow!("truncated PE at {o}"))
}

fn u32_at(b: &[u8], o: usize) -> Result<u32> {
    b.get(o..o + 4)
        .map(|s| u32::from_le_bytes([s[0], s[1], s[2], s[3]]))
        .ok_or_else(|| anyhow!("truncated PE at {o}"))
}

fn rva_to_offset(sections: &[Section], rva: u32) -> Option<usize> {
    sections.iter().find_map(|s| {
        let span = s.virtual_size.max(s.raw_size);
        (rva >= s.virtual_address && rva < s.virtual_address + span)
            .then(|| (rva - s.virtual_address + s.raw_offset) as usize)
    })
}

fn c_string(b: &[u8], offset: usize) -> Result<String> {
    let tail = b
        .get(offset..)
        .ok_or_else(|| anyhow!("name offset out of range"))?;
    let end = tail
        .iter()
        .position(|c| *c == 0)
        .ok_or_else(|| anyhow!("unterminated import name"))?;
    Ok(String::from_utf8_lossy(&tail[..end]).into_owned())
}

/// Statically imported DLL names, in import-table order.
pub fn imports(bytes: &[u8]) -> Result<Vec<String>> {
    ensure!(
        bytes.get(0..2) == Some(b"MZ"),
        "not a PE image (no MZ header)"
    );
    let pe = u32_at(bytes, 0x3C)? as usize;
    ensure!(
        bytes.get(pe..pe + 4) == Some(b"PE\0\0"),
        "not a PE image (no PE signature)"
    );
    let coff = pe + 4;
    let section_count = u16_at(bytes, coff + 2)? as usize;
    let optional_size = u16_at(bytes, coff + 16)? as usize;
    let optional = coff + 20;
    let magic = u16_at(bytes, optional)?;
    let directories = match magic {
        0x20B => optional + 112, // PE32+
        0x10B => optional + 96,  // PE32
        other => bail!("unknown optional header magic {other:#x}"),
    };
    let import_rva = u32_at(bytes, directories + 8)?;
    let mut sections = Vec::with_capacity(section_count);
    let table = optional + optional_size;
    for i in 0..section_count {
        let s = table + i * 40;
        sections.push(Section {
            virtual_size: u32_at(bytes, s + 8)?,
            virtual_address: u32_at(bytes, s + 12)?,
            raw_size: u32_at(bytes, s + 16)?,
            raw_offset: u32_at(bytes, s + 20)?,
        });
    }
    if import_rva == 0 {
        return Ok(Vec::new());
    }
    let mut descriptor = rva_to_offset(&sections, import_rva)
        .ok_or_else(|| anyhow!("import table outside sections"))?;
    let mut names = Vec::new();
    loop {
        let name_rva = u32_at(bytes, descriptor + 12)?;
        let first_thunk = u32_at(bytes, descriptor + 16)?;
        if name_rva == 0 && first_thunk == 0 {
            break;
        }
        let offset = rva_to_offset(&sections, name_rva)
            .ok_or_else(|| anyhow!("import name outside sections"))?;
        names.push(c_string(bytes, offset)?);
        descriptor += 20;
        ensure!(names.len() < 4096, "implausible import table");
    }
    Ok(names)
}

pub fn imports_of(path: &Path) -> Result<Vec<String>> {
    imports(&std::fs::read(path)?)
}

/// Windows components present on every supported Windows 10/11 x64 install.
/// Anything imported that is neither shipped, listed here, nor part of the
/// MSVC redistributable is an unresolved dependency.
pub const WINDOWS_SYSTEM_DLLS: &[&str] = &[
    "kernel32.dll",
    "advapi32.dll",
    "ntdll.dll",
    "rpcrt4.dll",
    "secur32.dll",
    "sspicli.dll",
    "userenv.dll",
    "powrprof.dll",
    "cfgmgr32.dll",
    "setupapi.dll",
    "normaliz.dll",
    "ucrtbase.dll",
    "msvcrt.dll",
    "user32.dll",
    "gdi32.dll",
    "shell32.dll",
    "shlwapi.dll",
    "comdlg32.dll",
    "comctl32.dll",
    "uxtheme.dll",
    "imm32.dll",
    "uiautomationcore.dll",
    "dwmapi.dll",
    "ole32.dll",
    "oleaut32.dll",
    "crypt32.dll",
    "bcrypt.dll",
    "ncrypt.dll",
    "authz.dll",
    "dbghelp.dll",
    "ws2_32.dll",
    "netapi32.dll",
    "wtsapi32.dll",
    "dnsapi.dll",
    "iphlpapi.dll",
    "winhttp.dll",
    "wininet.dll",
    "urlmon.dll",
    "wldap32.dll",
    "mpr.dll",
    "d3d11.dll",
    "d3d9.dll",
    "d3d12.dll",
    "dxgi.dll",
    "dxva2.dll",
    "dcomp.dll",
    "dwrite.dll",
    "d2d1.dll",
    "windowscodecs.dll",
    // Every D3D11 shader is compiled from source at runtime. Windows services
    // this compiler in System32; a bundled copy would pin it to the build date.
    "d3dcompiler_47.dll",
    "mf.dll",
    "mfplat.dll",
    "mfreadwrite.dll",
    "mfcore.dll",
    "propsys.dll",
    "avrt.dll",
    "ksuser.dll",
    "audioses.dll",
    "mmdevapi.dll",
    "tdh.dll",
    "coremessaging.dll",
    // Qt uses the ICU that Windows ships in System32.
    "icuuc.dll",
    "winmm.dll",
    "version.dll",
];

/// The dynamic MSVC runtime, satisfied by the Visual C++ 2015-2022
/// redistributable (a declared package dependency), never bundled.
pub const MSVC_RUNTIME_DLLS: &[&str] = &[
    "vcruntime140.dll",
    "vcruntime140_1.dll",
    "msvcp140.dll",
    "msvcp140_1.dll",
    "msvcp140_2.dll",
    "msvcp140_atomic_wait.dll",
    "msvcp140_codecvt_ids.dll",
    "concrt140.dll",
    "vcomp140.dll",
];

#[derive(Debug, Default)]
pub struct Audit {
    pub binaries: usize,
    pub shipped: usize,
    pub system: usize,
    pub msvc: usize,
    pub unresolved: Vec<String>,
}

/// Classifies every static import of every `.exe`/`.dll` under `root`.
pub fn audit_tree(root: &Path) -> Result<Audit> {
    let mut binaries = Vec::new();
    let mut stack = vec![root.to_path_buf()];
    while let Some(dir) = stack.pop() {
        for entry in std::fs::read_dir(&dir)?.flatten() {
            let path = entry.path();
            if path.is_dir() {
                stack.push(path);
            } else if path
                .extension()
                .and_then(|e| e.to_str())
                .is_some_and(|e| e.eq_ignore_ascii_case("exe") || e.eq_ignore_ascii_case("dll"))
            {
                binaries.push(path);
            }
        }
    }
    binaries.sort();
    let shipped: std::collections::HashSet<String> = binaries
        .iter()
        .filter_map(|p| {
            p.file_name()
                .map(|n| n.to_string_lossy().to_ascii_lowercase())
        })
        .collect();
    let mut audit = Audit {
        binaries: binaries.len(),
        ..Default::default()
    };
    for binary in &binaries {
        for import in imports_of(binary)? {
            let lower = import.to_ascii_lowercase();
            if shipped.contains(&lower) {
                audit.shipped += 1;
            } else if MSVC_RUNTIME_DLLS.contains(&lower.as_str()) {
                audit.msvc += 1;
            } else if WINDOWS_SYSTEM_DLLS.contains(&lower.as_str())
                || lower.starts_with("api-ms-win-")
                || lower.starts_with("ext-ms-")
            {
                audit.system += 1;
            } else {
                let rel = binary
                    .strip_prefix(root)
                    .unwrap_or(binary)
                    .display()
                    .to_string();
                audit.unresolved.push(format!("{rel} -> {import}"));
            }
        }
    }
    Ok(audit)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_the_imports_of_this_test_binary() {
        let exe = std::env::current_exe().unwrap();
        let names = imports_of(&exe).unwrap();
        assert!(
            names.iter().any(|n| n.eq_ignore_ascii_case("kernel32.dll")),
            "{names:?}"
        );
    }

    #[test]
    fn rejects_non_pe_bytes() {
        assert!(imports(b"not a pe").is_err());
        assert!(imports(b"MZ").is_err());
    }

    /// A minimal PE32+ image with one section holding an import table.
    pub fn synthetic_pe(imports: &[&str]) -> Vec<u8> {
        let mut b = vec![0u8; 0x600];
        b[0..2].copy_from_slice(b"MZ");
        b[0x3C..0x40].copy_from_slice(&0x40u32.to_le_bytes());
        b[0x40..0x44].copy_from_slice(b"PE\0\0");
        let coff = 0x44;
        b[coff..coff + 2].copy_from_slice(&0x8664u16.to_le_bytes());
        b[coff + 2..coff + 4].copy_from_slice(&1u16.to_le_bytes());
        b[coff + 16..coff + 18].copy_from_slice(&240u16.to_le_bytes());
        let opt = coff + 20;
        b[opt..opt + 2].copy_from_slice(&0x20Bu16.to_le_bytes());
        let dirs = opt + 112;
        b[dirs + 8..dirs + 12].copy_from_slice(&0x1000u32.to_le_bytes());
        let section = opt + 240;
        b[section..section + 6].copy_from_slice(b".idata");
        b[section + 8..section + 12].copy_from_slice(&0x400u32.to_le_bytes());
        b[section + 12..section + 16].copy_from_slice(&0x1000u32.to_le_bytes());
        b[section + 16..section + 20].copy_from_slice(&0x400u32.to_le_bytes());
        b[section + 20..section + 24].copy_from_slice(&0x200u32.to_le_bytes());
        let mut name_rva = 0x1100u32;
        for (i, name) in imports.iter().enumerate() {
            let d = 0x200 + i * 20;
            b[d + 12..d + 16].copy_from_slice(&name_rva.to_le_bytes());
            b[d + 16..d + 20].copy_from_slice(&0x1200u32.to_le_bytes());
            let at = (name_rva - 0x1000 + 0x200) as usize;
            b[at..at + name.len()].copy_from_slice(name.as_bytes());
            name_rva += name.len() as u32 + 1;
        }
        b
    }

    #[test]
    fn synthetic_imports_are_read_in_order() {
        assert_eq!(
            imports(&synthetic_pe(&["KERNEL32.dll", "Qt6Core.dll"])).unwrap(),
            vec!["KERNEL32.dll", "Qt6Core.dll"]
        );
    }

    #[test]
    fn an_import_nobody_provides_is_unresolved() {
        let dir = tempfile::tempdir().unwrap();
        std::fs::write(
            dir.path().join("exosnap.exe"),
            synthetic_pe(&[
                "kernel32.dll",
                "Qt6Core.dll",
                "vcruntime140.dll",
                "missing-codec.dll",
            ]),
        )
        .unwrap();
        std::fs::write(
            dir.path().join("Qt6Core.dll"),
            synthetic_pe(&["api-ms-win-crt-runtime-l1-1-0.dll"]),
        )
        .unwrap();
        let audit = audit_tree(dir.path()).unwrap();
        assert_eq!(audit.binaries, 2);
        assert_eq!((audit.shipped, audit.system, audit.msvc), (1, 2, 1));
        assert_eq!(
            audit.unresolved,
            vec!["exosnap.exe -> missing-codec.dll".to_string()]
        );
    }
}
