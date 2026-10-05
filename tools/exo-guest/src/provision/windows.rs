//! Native guest identity, console and certificate operations.

use std::path::{Path, PathBuf};

use anyhow::{Context as _, Result, bail, ensure};
use serde::Deserialize;
use windows::Win32::Foundation::HWND;
use windows::Win32::Security::Cryptography::*;
use windows::Win32::Security::WinTrust::*;
use windows::Win32::System::Registry::{
    HKEY_LOCAL_MACHINE, RRF_RT_REG_DWORD, RRF_RT_REG_SZ, RegGetValueW,
};
use windows::core::{PCWSTR, w};

fn wide(value: &std::ffi::OsStr) -> Vec<u16> {
    use std::os::windows::ffi::OsStrExt;
    value.encode_wide().chain(Some(0)).collect()
}

fn registry_string(key: &str, name: &str) -> Result<String> {
    let key = wide(key.as_ref());
    let name = wide(name.as_ref());
    let mut value = vec![0u16; 2048];
    let mut size = (value.len() * 2) as u32;
    unsafe {
        RegGetValueW(
            HKEY_LOCAL_MACHINE,
            PCWSTR(key.as_ptr()),
            PCWSTR(name.as_ptr()),
            RRF_RT_REG_SZ,
            None,
            Some(value.as_mut_ptr().cast()),
            Some(&mut size),
        )
        .ok()?;
    }
    let end = value
        .iter()
        .position(|value| *value == 0)
        .unwrap_or(value.len());
    Ok(String::from_utf16(&value[..end])?)
}

pub fn assert_guest(expected: &str) -> Result<()> {
    let actual = registry_string(
        r"SOFTWARE\Microsoft\Virtual Machine\Guest\Parameters",
        "VirtualMachineId",
    )
    .context("Hyper-V guest identity unavailable; refusing to provision this machine")?;
    ensure!(
        actual
            .trim_matches(['{', '}'])
            .eq_ignore_ascii_case(expected.trim_matches(['{', '}'])),
        "disposable VM identity mismatch"
    );
    Ok(())
}

pub fn boot_identity() -> Result<String> {
    #[derive(Deserialize)]
    struct Boot {
        #[serde(rename = "LastBootUpTime")]
        last_boot: String,
    }
    let connection = wmi::WMIConnection::new()?;
    let rows: Vec<Boot> =
        connection.raw_query("SELECT LastBootUpTime FROM Win32_OperatingSystem")?;
    Ok(rows
        .into_iter()
        .next()
        .context("guest boot identity unavailable")?
        .last_boot)
}

pub fn replace_file(source: &Path, target: &Path) -> Result<()> {
    use windows::Win32::Storage::FileSystem::{
        MOVEFILE_REPLACE_EXISTING, MOVEFILE_WRITE_THROUGH, MoveFileExW,
    };
    let source = wide(source.as_os_str());
    let target = wide(target.as_os_str());
    unsafe {
        MoveFileExW(
            PCWSTR(source.as_ptr()),
            PCWSTR(target.as_ptr()),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH,
        )?;
    }
    Ok(())
}

pub fn uac_setting() -> Result<u32> {
    let mut value = 0u32;
    let mut size = 4u32;
    unsafe {
        RegGetValueW(
            HKEY_LOCAL_MACHINE,
            w!("SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System"),
            w!("ConsentPromptBehaviorAdmin"),
            RRF_RT_REG_DWORD,
            None,
            Some((&mut value as *mut u32).cast()),
            Some(&mut size),
        )
        .ok()?;
    }
    Ok(value)
}

pub fn winget() -> Result<PathBuf> {
    let mut candidates: Vec<PathBuf> =
        std::env::split_paths(&std::env::var_os("PATH").unwrap_or_default())
            .map(|path| path.join("winget.exe"))
            .collect();
    if let Some(local) = std::env::var_os("LOCALAPPDATA") {
        candidates.push(PathBuf::from(local).join("Microsoft/WindowsApps/winget.exe"));
    }
    if let Some(program_files) = std::env::var_os("ProgramFiles") {
        let root = PathBuf::from(program_files).join("WindowsApps");
        if let Ok(entries) = std::fs::read_dir(root) {
            let mut packages = entries
                .filter_map(|entry| entry.ok())
                .filter(|entry| {
                    entry
                        .file_name()
                        .to_string_lossy()
                        .starts_with("Microsoft.DesktopAppInstaller_")
                })
                .map(|entry| entry.path())
                .collect::<Vec<_>>();
            packages.sort_by_key(|path| std::cmp::Reverse(package_version(path)));
            candidates.extend(packages.into_iter().map(|path| path.join("winget.exe")));
        }
    }
    for path in &candidates {
        if path.is_file()
            && std::process::Command::new(path)
                .arg("--version")
                .output()
                .is_ok_and(|output| output.status.success())
        {
            return Ok(path.clone());
        }
    }
    register_inbox_app_installer()?;
    for path in candidates {
        if path.is_file()
            && std::process::Command::new(&path)
                .arg("--version")
                .output()
                .is_ok_and(|output| output.status.success())
        {
            return Ok(path);
        }
    }
    bail!(
        "a usable inbox App Installer/winget is required; use Windows 11 media containing App Installer"
    )
}

fn register_inbox_app_installer() -> Result<()> {
    use windows::Management::Deployment::{DeploymentOptions, PackageManager};
    use windows::Win32::System::WinRT::{RO_INIT_MULTITHREADED, RoInitialize, RoUninitialize};
    unsafe {
        RoInitialize(RO_INIT_MULTITHREADED)?;
    }
    let result = (|| {
        let manager = PackageManager::new()?;
        let family = windows::core::HSTRING::from("Microsoft.DesktopAppInstaller_8wekyb3d8bbwe");
        let packages = manager.FindPackagesByPackageFamilyName(&family)?;
        ensure!(
            packages.First()?.HasCurrent()?,
            "Windows image contains no inbox App Installer package; refusing an unpinned download"
        );
        let result = manager
            .RegisterPackageByFamilyNameAndOptionalPackagesAsync(
                &family,
                None,
                DeploymentOptions::None,
                None,
                None,
            )?
            .join()?;
        result.ExtendedErrorCode()?.ok().with_context(|| {
            result
                .ErrorText()
                .map(|text| text.to_string())
                .unwrap_or_default()
        })?;
        Ok(())
    })();
    unsafe {
        RoUninitialize();
    }
    result
}

fn package_version(path: &Path) -> Vec<u32> {
    path.file_name()
        .unwrap_or_default()
        .to_string_lossy()
        .split('_')
        .nth(1)
        .unwrap_or_default()
        .split('.')
        .map(|part| part.parse().unwrap_or(0))
        .collect()
}

pub fn display_installed(hardware_id: &str) -> Result<bool> {
    #[derive(Deserialize)]
    struct Device {
        #[serde(rename = "HardwareID")]
        hardware_ids: Option<Vec<String>>,
    }
    let connection = wmi::WMIConnection::new()?;
    let devices: Vec<Device> = connection
        .raw_query("SELECT HardwareID FROM Win32_PnPEntity WHERE PNPClass = 'Display'")?;
    Ok(devices
        .iter()
        .filter_map(|device| device.hardware_ids.as_ref())
        .flatten()
        .any(|id| id.eq_ignore_ascii_case(hardware_id)))
}

pub fn attach_console() -> Result<()> {
    use windows::Win32::System::RemoteDesktop::{
        WTSActive, WTSDisconnected, WTSEnumerateSessionsW, WTSFreeMemory,
        WTSGetActiveConsoleSessionId,
    };
    let console = unsafe { WTSGetActiveConsoleSessionId() };
    let mut sessions = std::ptr::null_mut();
    let mut count = 0;
    unsafe {
        WTSEnumerateSessionsW(None, 0, 1, &mut sessions, &mut count)?;
    }
    let result = (|| {
        let sessions = if count == 0 {
            &[][..]
        } else {
            unsafe { std::slice::from_raw_parts(sessions, count as usize) }
        };
        let interactive: Vec<_> = sessions
            .iter()
            .filter(|session| {
                session.SessionId != 0
                    && (session.State == WTSActive || session.State == WTSDisconnected)
            })
            .collect();
        if interactive
            .iter()
            .any(|session| session.SessionId == console)
        {
            return Ok(());
        }
        ensure!(
            interactive.len() == 1,
            "guest console attachment needs exactly one interactive session, found {}",
            interactive.len()
        );
        let session = interactive[0].SessionId;
        // tscon can return an error when another actor completed the same attach.
        // Windows' resulting console identity is the authoritative postcondition.
        let _ = std::process::Command::new("tscon.exe")
            .args([session.to_string(), "/dest:console".into()])
            .output()?;
        ensure!(
            unsafe { WTSGetActiveConsoleSessionId() } == session,
            "guest session {session} did not attach to the console"
        );
        Ok(())
    })();
    if !sessions.is_null() {
        unsafe {
            WTSFreeMemory(sessions.cast());
        }
    }
    result
}

pub fn trust_catalog(catalog: &Path, expected_thumbprint: &str) -> Result<()> {
    let path = wide(catalog.as_os_str());
    let mut file = WINTRUST_FILE_INFO {
        cbStruct: std::mem::size_of::<WINTRUST_FILE_INFO>() as u32,
        pcwszFilePath: PCWSTR(path.as_ptr()),
        ..Default::default()
    };
    let mut data = WINTRUST_DATA {
        cbStruct: std::mem::size_of::<WINTRUST_DATA>() as u32,
        dwUIChoice: WTD_UI_NONE,
        fdwRevocationChecks: WTD_REVOKE_WHOLECHAIN,
        dwUnionChoice: WTD_CHOICE_FILE,
        Anonymous: WINTRUST_DATA_0 { pFile: &mut file },
        dwStateAction: WTD_STATEACTION_VERIFY,
        ..Default::default()
    };
    let mut policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    let status = unsafe {
        WinVerifyTrust(
            HWND::default(),
            &mut policy,
            (&mut data as *mut WINTRUST_DATA).cast(),
        )
    };
    let result = (|| {
        ensure!(
            status == 0,
            "driver catalog signature verification failed: 0x{:08x}",
            status as u32
        );
        let provider = unsafe { WTHelperProvDataFromStateData(data.hWVTStateData) };
        ensure!(
            !provider.is_null(),
            "driver catalog has no trust provider state"
        );
        let signer = unsafe { WTHelperGetProvSignerFromChain(provider, 0, false, 0) };
        ensure!(!signer.is_null(), "driver catalog has no signer");
        let signer = unsafe { &*signer };
        ensure!(
            signer.csCertChain > 0 && !signer.pasCertChain.is_null(),
            "driver signer has no certificate chain"
        );
        let certificate = unsafe { (*signer.pasCertChain).pCert };
        ensure!(!certificate.is_null(), "driver signer has no certificate");
        let mut thumbprint = [0u8; 20];
        let mut size = thumbprint.len() as u32;
        unsafe {
            CertGetCertificateContextProperty(
                certificate,
                CERT_SHA1_HASH_PROP_ID,
                Some(thumbprint.as_mut_ptr().cast()),
                &mut size,
            )?;
        }
        ensure!(
            size == 20 && hex::encode(thumbprint).eq_ignore_ascii_case(expected_thumbprint),
            "driver catalog signer does not match the manifest's publisher pin"
        );
        let store = unsafe {
            CertOpenStore(
                CERT_STORE_PROV_SYSTEM_W,
                CERT_QUERY_ENCODING_TYPE(0),
                None,
                CERT_OPEN_STORE_FLAGS(CERT_SYSTEM_STORE_LOCAL_MACHINE)
                    | CERT_STORE_OPEN_EXISTING_FLAG,
                Some(w!("TrustedPublisher").as_ptr().cast()),
            )?
        };
        let imported = unsafe {
            CertAddCertificateContextToStore(
                Some(store),
                certificate,
                CERT_STORE_ADD_REPLACE_EXISTING,
                None,
            )
        };
        unsafe {
            CertCloseStore(Some(store), 0)?;
        }
        imported?;
        Ok(())
    })();
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    unsafe {
        WinVerifyTrust(
            HWND::default(),
            &mut policy,
            (&mut data as *mut WINTRUST_DATA).cast(),
        );
    }
    result
}
