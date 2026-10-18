//! Wall-clock costs of the blocking analysis pipeline.

pub(super) fn progress(event: serde_json::Value) {
    if std::env::var_os("EXO_DEV_CLANG_TIDY_PROGRESS").as_deref() == Some(std::ffi::OsStr::new("1"))
    {
        use std::io::Write as _;
        let _ = writeln!(
            std::io::stderr().lock(),
            "EXO_DEV_CLANG_TIDY_PROGRESS {event}"
        );
    }
}

pub(super) fn progress_phase(phase: &str, started: std::time::Instant) {
    progress(
        serde_json::json!({"event": "phase-start", "phase": phase, "elapsed_seconds": started.elapsed().as_secs_f64()}),
    );
}

#[derive(Clone, Debug, Default, serde::Serialize)]
pub struct TidyTiming {
    pub wall_seconds: f64,
    pub compile_database_seconds: f64,
    pub tool_discovery_seconds: f64,
    pub tool_version_seconds: f64,
    pub configuration_identity_seconds: f64,
    pub toolchain_identity_seconds: f64,
    pub scope_seconds: f64,
    pub git_scope_seconds: f64,
    pub header_expansion_seconds: f64,
    pub dependency_parse_seconds: f64,
    pub dependency_objects: usize,
    pub dependency_inputs: usize,
    pub dependency_sources: usize,
    pub freshness_seconds: f64,
    pub ninja_extraction_seconds: f64,
    pub dependencies_seconds: f64,
    pub hashing_seconds: f64,
    pub cache_lookup_seconds: f64,
    pub analysis_wall_seconds: f64,
    pub input_revalidation_seconds: f64,
    pub validation_and_storage_seconds: f64,
    pub reporting_seconds: f64,
    pub workers: usize,
    pub units: Vec<UnitTiming>,
}

#[derive(Clone, Debug, serde::Serialize)]
pub struct UnitTiming {
    #[serde(skip)]
    pub(super) unit: Option<usize>,
    pub source: String,
    pub wall_seconds: f64,
    pub cpu_seconds: Option<f64>,
    pub peak_working_set_bytes: Option<usize>,
}

pub(super) fn invoke(
    command: &mut std::process::Command,
    source: &str,
) -> (String, i32, UnitTiming) {
    use std::process::Stdio;
    let started = std::time::Instant::now();
    command.stdout(Stdio::piped()).stderr(Stdio::piped());
    let mut timing = UnitTiming {
        unit: None,
        source: source.to_string(),
        wall_seconds: 0.0,
        cpu_seconds: None,
        peak_working_set_bytes: None,
    };
    let result = match command.spawn() {
        Ok(child) => {
            #[cfg(windows)]
            let handle = {
                use std::os::windows::io::{AsRawHandle, BorrowedHandle};
                // wait_with_output consumes Child and closes its original handle.
                // Keep a duplicate to query the completed process rather than a reused PID.
                unsafe { BorrowedHandle::borrow_raw(child.as_raw_handle()) }
                    .try_clone_to_owned()
                    .ok()
            };
            let output = child.wait_with_output();
            #[cfg(windows)]
            if let Some(handle) = handle {
                let (cpu, memory) = resources(&handle);
                timing.cpu_seconds = cpu;
                timing.peak_working_set_bytes = memory;
            }
            match output {
                Ok(output) => {
                    let mut combined = String::from_utf8_lossy(&output.stdout).into_owned();
                    combined.push_str(&String::from_utf8_lossy(&output.stderr));
                    (combined, output.status.code().unwrap_or(-1))
                }
                Err(_) => (String::new(), -1),
            }
        }
        Err(_) => (String::new(), -1),
    };
    timing.wall_seconds = started.elapsed().as_secs_f64();
    (result.0, result.1, timing)
}

#[cfg(windows)]
fn resources(handle: &std::os::windows::io::OwnedHandle) -> (Option<f64>, Option<usize>) {
    use std::os::windows::io::AsRawHandle;
    use windows::Win32::Foundation::{FILETIME, HANDLE};
    use windows::Win32::System::ProcessStatus::{GetProcessMemoryInfo, PROCESS_MEMORY_COUNTERS};
    use windows::Win32::System::Threading::GetProcessTimes;

    let handle = HANDLE(handle.as_raw_handle());
    let mut created = FILETIME::default();
    let mut exited = FILETIME::default();
    let mut kernel = FILETIME::default();
    let mut user = FILETIME::default();
    let mut memory = PROCESS_MEMORY_COUNTERS::default();
    let cpu = unsafe { GetProcessTimes(handle, &mut created, &mut exited, &mut kernel, &mut user) }
        .ok()
        .map(|()| {
            let ticks = |time: FILETIME| {
                (u64::from(time.dwHighDateTime) << 32) | u64::from(time.dwLowDateTime)
            };
            (ticks(kernel) + ticks(user)) as f64 / 10_000_000.0
        });
    let peak = unsafe {
        GetProcessMemoryInfo(
            handle,
            &mut memory,
            std::mem::size_of::<PROCESS_MEMORY_COUNTERS>() as u32,
        )
    }
    .ok()
    .map(|()| memory.PeakWorkingSetSize);
    (cpu, peak)
}

impl super::ClangTidyReport {
    pub fn write_profile(
        &self,
        path: &std::path::Path,
        head: Option<&str>,
        dirty: bool,
    ) -> anyhow::Result<()> {
        use anyhow::Context as _;
        let public = std::env::var_os("EXO_DEV_CLANG_TIDY_PROFILE").as_deref()
            == Some(std::ffi::OsStr::new("public"));
        let identity = self.profile_identity(head, dirty, public)?;
        std::fs::write(path, serde_json::to_vec_pretty(&identity)?)
            .with_context(|| format!("could not write {}", path.display()))
    }

    fn profile_identity(
        &self,
        head: Option<&str>,
        dirty: bool,
        public: bool,
    ) -> anyhow::Result<serde_json::Value> {
        use sha2::Digest as _;
        let compile_database_sha256 = if self.compile_db.as_os_str().is_empty() {
            None
        } else {
            Some(hex::encode(sha2::Sha256::digest(std::fs::read(
                &self.compile_db,
            )?)))
        };
        let mut identity = serde_json::json!({
            "profile_schema": 1,
            "head": head,
            "dirty": dirty,
            "tool": self.tool_path,
            "tool_version": self.tool_version,
            "compile_database": self.compile_db,
            "compile_database_sha256": compile_database_sha256,
            "scope": self.scope,
            "selected": self.analyzed,
            "cache_enabled": self.cache_enabled,
            "cache_hits": self.replayed,
            "cache_misses": self.analyzed.saturating_sub(self.replayed),
            "compiler_policy": self.compiler_policy,
            "timing": (!self.tool_path.as_os_str().is_empty()).then_some(&self.timing),
        });
        if public {
            let object = identity.as_object_mut().expect("profile object");
            object.remove("tool");
            object.remove("compile_database");
            object.insert("profile_mode".into(), serde_json::json!("public"));
            object.insert(
                "scope".into(),
                serde_json::json!(if self.analyzed == 0 {
                    "skipped"
                } else if self.scope.starts_with("full tree") {
                    "full-tree"
                } else {
                    "scoped"
                }),
            );
            if let Some(units) = identity
                .get_mut("timing")
                .and_then(|timing| timing.get_mut("units"))
                .and_then(serde_json::Value::as_array_mut)
            {
                for (serialized, measured) in units.iter_mut().zip(&self.timing.units) {
                    let object = serialized.as_object_mut().expect("unit timing object");
                    object.remove("source");
                    object.insert("unit".into(), serde_json::json!(measured.unit));
                }
            }
        }
        Ok(identity)
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn public_profile_omits_paths_without_changing_local_profile_or_resource_evidence() {
        let dir = tempfile::tempdir().unwrap();
        let mut report =
            super::super::ClangTidyReport::skipped("scoped C:/private/source.cpp", dir.path());
        report.compile_db = dir.path().join("compile_commands.json");
        std::fs::write(&report.compile_db, "[]").unwrap();
        report.tool_path = std::path::PathBuf::from("C:/private/clang-tidy.exe");
        report.tool_version = "22.1.3".into();
        report.analyzed = 2;
        report.replayed = 1;
        report.timing.units.push(super::UnitTiming {
            unit: Some(2),
            source: "C:/private/source.cpp".into(),
            wall_seconds: 1.0,
            cpu_seconds: Some(0.5),
            peak_working_set_bytes: Some(4096),
        });
        let rich = report
            .profile_identity(Some("abcdef"), true, false)
            .unwrap();
        assert_eq!(rich["tool"], "C:/private/clang-tidy.exe");
        assert_eq!(
            rich["timing"]["units"][0]["source"],
            "C:/private/source.cpp"
        );
        assert!(rich["timing"]["units"][0].get("unit").is_none());
        assert!(rich.get("profile_mode").is_none());
        let public = report.profile_identity(Some("abcdef"), true, true).unwrap();
        assert!(public.get("tool").is_none());
        assert!(public.get("compile_database").is_none());
        assert_eq!(public["profile_mode"], "public");
        assert_eq!(public["scope"], "scoped");
        assert!(public["timing"]["units"][0].get("source").is_none());
        assert_eq!(public["timing"]["units"][0]["unit"], 2);
        for field in [
            "head",
            "dirty",
            "tool_version",
            "compile_database_sha256",
            "selected",
            "cache_enabled",
            "cache_hits",
            "cache_misses",
            "compiler_policy",
        ] {
            assert_eq!(public[field], rich[field], "{field}");
        }
        for field in ["wall_seconds", "cpu_seconds", "peak_working_set_bytes"] {
            assert_eq!(
                public["timing"]["units"][0][field], rich["timing"]["units"][0][field],
                "{field}"
            );
        }
        let serialized = public.to_string();
        assert!(!serialized.contains("private"));
        assert!(!serialized.contains(&report.compile_db.to_string_lossy().to_string()));
        let skipped = super::super::ClangTidyReport::skipped("private skip details", dir.path())
            .profile_identity(None, false, true)
            .unwrap();
        assert_eq!(skipped["scope"], "skipped");
        assert!(skipped["timing"].is_null());
    }
    #[cfg(windows)]
    #[test]
    fn measures_completed_child_without_changing_output_or_exit() {
        let mut command = crate::process::command("cmd");
        command.args(["/d", "/c", "echo timing-output"]);
        let (output, code, timing) = super::invoke(&mut command, "fixture");
        assert_eq!(code, 0);
        assert_eq!(output.trim(), "timing-output");
        assert!(timing.cpu_seconds.is_some());
        assert!(timing.peak_working_set_bytes.is_some_and(|bytes| bytes > 0));
    }
}

#[derive(Clone, Debug, Default, serde::Serialize)]
pub struct CompilerPolicySummary {
    pub project_cxx_commands: usize,
    pub warning_policy_commands: usize,
    pub warning_policy_with_sdl: usize,
    pub warning_policy_with_utf8: usize,
    pub explicit_stl_hardening_commands: usize,
    pub dependency_commands_with_sdl: usize,
}

pub(super) fn compiler_policy(
    entries: &[super::CompileEntry],
    repo_root: &str,
    build_dir: &str,
) -> CompilerPolicySummary {
    let mut summary = CompilerPolicySummary::default();
    for entry in entries {
        let command = entry
            .command_line
            .split("\nidentity=")
            .next()
            .unwrap_or_default();
        let has = |flag: &str| {
            command
                .split_whitespace()
                .any(|word| word.strip_prefix('/').or_else(|| word.strip_prefix('-')) == Some(flag))
        };
        let sdl = has("sdl");
        let dependency = entry.file.contains("/_deps/") || entry.file.contains("/third_party/");
        if dependency && sdl {
            summary.dependency_commands_with_sdl += 1;
        }
        let cxx = std::path::Path::new(&entry.file)
            .extension()
            .and_then(|value| value.to_str())
            .is_some_and(|extension| matches!(extension, "cpp" | "cxx" | "cc"));
        if !cxx || !super::is_project_source(&entry.file, repo_root, build_dir) {
            continue;
        }
        summary.project_cxx_commands += 1;
        if has("D_MSVC_STL_HARDENING=1") {
            summary.explicit_stl_hardening_commands += 1;
        }
        if has("W4") && has("WX") {
            summary.warning_policy_commands += 1;
            summary.warning_policy_with_sdl += usize::from(sdl);
            summary.warning_policy_with_utf8 += usize::from(has("utf-8"));
        }
    }
    summary
}
