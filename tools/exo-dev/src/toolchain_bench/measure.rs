//! One timed child process: wall clock, peak committed memory of its whole
//! process tree, and a log file that holds the complete combined output.

use std::fs::File;
use std::path::Path;
use std::process::{Command, Stdio};
use std::time::Instant;

use anyhow::{Context, Result};
use serde::Serialize;

#[derive(Debug, Clone, Serialize)]
pub struct Timing {
    pub wall_seconds: f64,
    /// Peak committed memory summed over every process the command started.
    /// Absent when the platform cannot report it.
    pub peak_commit_bytes: Option<u64>,
    pub exit_code: i32,
}

/// Runs `command` to completion with stdout and stderr appended to `log`.
///
/// The child joins a job object right after spawn. A grandchild started in the
/// microseconds before the assignment would escape the memory accounting, which
/// is acceptable for cmake and ninja because they spend their first
/// milliseconds on argument parsing.
pub fn timed(mut command: Command, log: &Path) -> Result<Timing> {
    let out = File::options()
        .create(true)
        .append(true)
        .open(log)
        .with_context(|| format!("open {}", log.display()))?;
    command
        .stdin(Stdio::null())
        .stdout(out.try_clone()?)
        .stderr(out);
    let job = job::Job::new();
    let start = Instant::now();
    let mut child = command.spawn().context("spawn")?;
    if let Some(job) = &job {
        job.adopt(&child);
    }
    let status = child.wait()?;
    let wall_seconds = start.elapsed().as_secs_f64();
    Ok(Timing {
        wall_seconds,
        peak_commit_bytes: job.as_ref().and_then(job::Job::peak_commit),
        exit_code: status.code().unwrap_or(-1),
    })
}

#[cfg(windows)]
mod job {
    use std::os::windows::io::AsRawHandle;
    use std::process::Child;

    use windows::Win32::Foundation::{CloseHandle, HANDLE};
    use windows::Win32::System::JobObjects::{
        AssignProcessToJobObject, CreateJobObjectW, JOBOBJECT_EXTENDED_LIMIT_INFORMATION,
        JobObjectExtendedLimitInformation, QueryInformationJobObject,
    };

    pub struct Job(HANDLE);

    impl Job {
        pub fn new() -> Option<Job> {
            unsafe { CreateJobObjectW(None, None).ok().map(Job) }
        }

        pub fn adopt(&self, child: &Child) {
            unsafe {
                let _ = AssignProcessToJobObject(self.0, HANDLE(child.as_raw_handle() as _));
            }
        }

        pub fn peak_commit(&self) -> Option<u64> {
            let mut info = JOBOBJECT_EXTENDED_LIMIT_INFORMATION::default();
            unsafe {
                QueryInformationJobObject(
                    Some(self.0),
                    JobObjectExtendedLimitInformation,
                    &mut info as *mut _ as *mut _,
                    std::mem::size_of::<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>() as u32,
                    None,
                )
                .ok()?;
            }
            Some(info.PeakJobMemoryUsed as u64)
        }
    }

    impl Drop for Job {
        fn drop(&mut self) {
            unsafe {
                let _ = CloseHandle(self.0);
            }
        }
    }
}

#[cfg(not(windows))]
mod job {
    use std::process::Child;

    pub struct Job;

    impl Job {
        pub fn new() -> Option<Job> {
            None
        }
        pub fn adopt(&self, _child: &Child) {}
        pub fn peak_commit(&self) -> Option<u64> {
            None
        }
    }
}

pub fn median(values: &[f64]) -> Option<f64> {
    if values.is_empty() {
        return None;
    }
    let mut sorted = values.to_vec();
    sorted.sort_by(|a, b| a.total_cmp(b));
    let mid = sorted.len() / 2;
    Some(if sorted.len() % 2 == 1 {
        sorted[mid]
    } else {
        (sorted[mid - 1] + sorted[mid]) / 2.0
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn median_of_odd_and_even_sets() {
        assert_eq!(median(&[3.0, 1.0, 2.0]), Some(2.0));
        assert_eq!(median(&[4.0, 1.0, 2.0, 3.0]), Some(2.5));
        assert_eq!(median(&[]), None);
    }
}
