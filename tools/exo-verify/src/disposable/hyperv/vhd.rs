//! Per-run differencing disks over the sealed base image (Virtual Disk API).

use anyhow::{Result, bail};
use std::path::Path;
use windows::Win32::Foundation::{CloseHandle, HANDLE, WIN32_ERROR};
use windows::Win32::Storage::Vhd::{
    CREATE_VIRTUAL_DISK_FLAG_NONE, CREATE_VIRTUAL_DISK_PARAMETERS, CREATE_VIRTUAL_DISK_VERSION_2,
    CreateVirtualDisk, VIRTUAL_DISK_ACCESS_NONE, VIRTUAL_STORAGE_TYPE,
    VIRTUAL_STORAGE_TYPE_DEVICE_VHDX, VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT,
};
use windows::core::{HSTRING, PCWSTR};

/// Creates `child` as a differencing VHDX whose parent is `parent`. Every
/// write of the run lands in the child; the parent is never opened for write.
pub fn create_differencing(parent: &Path, child: &Path) -> Result<()> {
    if child.exists() {
        bail!("{} already exists", child.display());
    }
    let storage = VIRTUAL_STORAGE_TYPE {
        DeviceId: VIRTUAL_STORAGE_TYPE_DEVICE_VHDX,
        VendorId: VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT,
    };
    let parent_path = HSTRING::from(parent.as_os_str());
    let child_path = HSTRING::from(child.as_os_str());
    let mut parameters = CREATE_VIRTUAL_DISK_PARAMETERS {
        Version: CREATE_VIRTUAL_DISK_VERSION_2,
        ..Default::default()
    };
    parameters.Anonymous.Version2.ParentPath = PCWSTR(parent_path.as_ptr());
    parameters.Anonymous.Version2.ParentVirtualStorageType = storage;
    let mut handle = HANDLE::default();
    let status: WIN32_ERROR = unsafe {
        CreateVirtualDisk(
            &storage,
            &child_path,
            VIRTUAL_DISK_ACCESS_NONE,
            None,
            CREATE_VIRTUAL_DISK_FLAG_NONE,
            0,
            &parameters,
            None,
            &mut handle,
        )
    };
    if status.0 != 0 {
        bail!(
            "create differencing disk {} over {}: {}",
            child.display(),
            parent.display(),
            windows::core::Error::from(status.to_hresult()).message()
        );
    }
    unsafe {
        let _ = CloseHandle(handle);
    }
    Ok(())
}
