//! Reads installer identity out of a built MSI.
//!
//! WinGet keys its uninstall and upgrade behavior on the MSI's real
//! `ProductCode` and its permanent `UpgradeCode`, and neither can be derived
//! from a filename or a packaging template. The Windows Installer API reads
//! the `Property` table of the database without installing anything.

use std::path::Path;

/// The `ProductCode` and `UpgradeCode` declared inside `msi`.
#[derive(Debug, Clone, PartialEq, serde::Serialize, serde::Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct MsiIdentity {
    pub product_code: String,
    pub upgrade_code: String,
}

#[cfg(windows)]
pub fn read_msi_identity(msi: &Path) -> anyhow::Result<MsiIdentity> {
    use anyhow::{Context as _, ensure};
    use windows::Win32::System::ApplicationInstallationAndServicing::{
        MSIDBOPEN_READONLY, MSIHANDLE, MsiCloseHandle, MsiDatabaseOpenViewW, MsiOpenDatabaseW,
        MsiRecordGetStringW, MsiViewExecute, MsiViewFetch,
    };
    use windows::core::PCWSTR;

    fn wide(text: &str) -> Vec<u16> {
        text.encode_utf16().chain(std::iter::once(0)).collect()
    }

    unsafe {
        let path: Vec<u16> = wide(&msi.to_string_lossy());
        let mut database = MSIHANDLE(0);
        let code = MsiOpenDatabaseW(PCWSTR(path.as_ptr()), MSIDBOPEN_READONLY, &mut database);
        ensure!(code == 0, "MsiOpenDatabaseW failed with {code}");
        let read = |property: &str| -> anyhow::Result<String> {
            let query = wide(&format!(
                "SELECT `Value` FROM `Property` WHERE `Property`='{property}'"
            ));
            let mut view = MSIHANDLE(0);
            let code = MsiDatabaseOpenViewW(database, PCWSTR(query.as_ptr()), &mut view);
            ensure!(code == 0, "MsiDatabaseOpenViewW failed with {code}");
            let mut record = MSIHANDLE(0);
            let result: anyhow::Result<String> = (|| {
                let code = MsiViewExecute(view, MSIHANDLE(0));
                ensure!(code == 0, "MsiViewExecute failed with {code}");
                let code = MsiViewFetch(view, &mut record);
                ensure!(code == 0, "the MSI declares no {property}");
                // The size queried with a null buffer includes the null
                // terminator, and the second call takes the buffer capacity
                // as its input length. Both conventions have to be honored or
                // the call reports ERROR_MORE_DATA for a buffer that fits.
                let mut required = 0u32;
                let code = MsiRecordGetStringW(record, 1, None, Some(&mut required));
                ensure!(
                    code == 0 || code == 234,
                    "MsiRecordGetStringW failed with {code}"
                );
                if required == 0 {
                    return Ok(String::new());
                }
                let mut buffer = vec![0u16; required as usize + 1];
                let mut capacity = required + 1;
                let code = MsiRecordGetStringW(
                    record,
                    1,
                    Some(windows::core::PWSTR(buffer.as_mut_ptr())),
                    Some(&mut capacity),
                );
                ensure!(code == 0, "MsiRecordGetStringW failed with {code}");
                buffer.truncate(capacity.min(required) as usize);
                Ok(String::from_utf16_lossy(&buffer))
            })();
            if record.0 != 0 {
                MsiCloseHandle(record);
            }
            if view.0 != 0 {
                MsiCloseHandle(view);
            }
            result
        };
        let identity: anyhow::Result<MsiIdentity> = (|| {
            Ok(MsiIdentity {
                product_code: read("ProductCode")?,
                upgrade_code: read("UpgradeCode")?,
            })
        })();
        MsiCloseHandle(database);
        identity
    }
    .with_context(|| format!("could not read installer identity from {}", msi.display()))
}

#[cfg(not(windows))]
pub fn read_msi_identity(msi: &Path) -> anyhow::Result<MsiIdentity> {
    anyhow::bail!(
        "reading installer identity is a Windows contract; {} cannot be inspected here",
        msi.display()
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[cfg(windows)]
    #[test]
    fn a_missing_file_is_an_error_not_an_empty_identity() {
        let dir = tempfile::tempdir().unwrap();
        let missing = dir.path().join("absent.msi");
        assert!(read_msi_identity(&missing).is_err());
    }

    #[test]
    fn the_identity_carries_braced_guids_in_the_published_shape() {
        let identity = MsiIdentity {
            product_code: "{11111111-2222-3333-4444-555555555555}".into(),
            upgrade_code: "{8988DAFC-3AE4-4788-BA6D-62E3F73C7A7D}".into(),
        };
        let text = serde_json::to_string(&identity).unwrap();
        assert!(text.contains("productCode"));
        assert!(text.contains("8988DAFC"));
    }
}
