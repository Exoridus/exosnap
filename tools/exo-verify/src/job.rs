//! Every process a scenario starts lives in one kill-on-close job, so nothing a
//! scenario launched (including grandchildren such as the updater) survives
//! the scenario, whatever way it ended.

use anyhow::{Result, ensure};
use std::process::Child;
#[cfg(windows)]
use std::time::{Duration, Instant};

#[cfg(windows)]
pub struct Job {
    handle: windows::Win32::Foundation::HANDLE,
    children: std::sync::Mutex<Vec<windows::Win32::Foundation::HANDLE>>,
}

#[cfg(windows)]
unsafe impl Send for Job {}

#[cfg(windows)]
impl Job {
    pub fn new() -> Result<Job> {
        use windows::Win32::System::JobObjects::{
            CreateJobObjectW, JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE,
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION, JobObjectExtendedLimitInformation,
            SetInformationJobObject,
        };
        unsafe {
            let handle = CreateJobObjectW(None, None)?;
            let mut info = JOBOBJECT_EXTENDED_LIMIT_INFORMATION::default();
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(
                handle,
                JobObjectExtendedLimitInformation,
                &info as *const _ as *const _,
                std::mem::size_of::<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>() as u32,
            )?;
            Ok(Job {
                handle,
                children: std::sync::Mutex::new(Vec::new()),
            })
        }
    }

    pub fn adopt(&self, child: &Child) -> Result<()> {
        use std::os::windows::io::AsRawHandle;
        use windows::Win32::Foundation::{DUPLICATE_SAME_ACCESS, DuplicateHandle, HANDLE};
        use windows::Win32::System::JobObjects::AssignProcessToJobObject;
        use windows::Win32::System::Threading::GetCurrentProcess;
        unsafe { AssignProcessToJobObject(self.handle, HANDLE(child.as_raw_handle() as _))? };
        let mut wait_handle = HANDLE::default();
        unsafe {
            let process = GetCurrentProcess();
            DuplicateHandle(
                process,
                HANDLE(child.as_raw_handle() as _),
                process,
                &mut wait_handle,
                0,
                false,
                DUPLICATE_SAME_ACCESS,
            )?;
        }
        self.children.lock().unwrap().push(wait_handle);
        Ok(())
    }

    pub fn terminate(&self) -> Result<()> {
        use windows::Win32::System::JobObjects::{
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION, JobObjectBasicAccountingInformation,
            QueryInformationJobObject, TerminateJobObject,
        };
        unsafe {
            TerminateJobObject(self.handle, 1)?;
        }
        let deadline = Instant::now() + Duration::from_secs(10);
        for child in self.children.lock().unwrap().iter() {
            use windows::Win32::Foundation::WAIT_OBJECT_0;
            use windows::Win32::System::Threading::WaitForSingleObject;
            let remaining = deadline.saturating_duration_since(Instant::now());
            let wait = unsafe {
                WaitForSingleObject(*child, remaining.as_millis().min(u32::MAX as u128) as u32)
            };
            ensure!(
                wait == WAIT_OBJECT_0,
                "an adopted process did not exit after job termination"
            );
        }
        loop {
            let mut info = JOBOBJECT_BASIC_ACCOUNTING_INFORMATION::default();
            unsafe {
                QueryInformationJobObject(
                    Some(self.handle),
                    JobObjectBasicAccountingInformation,
                    &mut info as *mut _ as *mut _,
                    std::mem::size_of::<JOBOBJECT_BASIC_ACCOUNTING_INFORMATION>() as u32,
                    None,
                )?;
            }
            if info.ActiveProcesses == 0 {
                return Ok(());
            }
            ensure!(
                Instant::now() < deadline,
                "job still has {} active processes after termination",
                info.ActiveProcesses
            );
            std::thread::sleep(Duration::from_millis(20));
        }
    }
}

#[cfg(windows)]
impl Drop for Job {
    fn drop(&mut self) {
        let _ = self.terminate();
        unsafe {
            for child in self.children.get_mut().unwrap().drain(..) {
                let _ = windows::Win32::Foundation::CloseHandle(child);
            }
            let _ = windows::Win32::Foundation::CloseHandle(self.handle);
        }
    }
}

#[cfg(not(windows))]
pub struct Job;

#[cfg(not(windows))]
impl Job {
    pub fn new() -> Result<Job> {
        Ok(Job)
    }
    pub fn adopt(&self, _: &Child) -> Result<()> {
        Ok(())
    }
    pub fn terminate(&self) -> Result<()> {
        Ok(())
    }
}

#[cfg(all(test, windows))]
mod tests {
    use super::*;
    use std::process::Command;

    #[test]
    fn terminate_waits_for_adopted_processes_to_exit() {
        let job = Job::new().unwrap();
        let mut child = Command::new("pwsh")
            .args([
                "-NoProfile",
                "-NonInteractive",
                "-Command",
                "Start-Sleep -Seconds 30",
            ])
            .spawn()
            .unwrap();
        job.adopt(&child).unwrap();
        job.terminate().unwrap();
        assert!(child.try_wait().unwrap().is_some());
    }
}
