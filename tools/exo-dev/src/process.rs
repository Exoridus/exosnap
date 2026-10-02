//! Child processes: environment hygiene, combined output capture, log files.

use std::ffi::OsString;
use std::fs::File;
use std::io::{self, BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

/// The variables a git hook sets for the repository being committed. Every child
/// inherits them, and GIT_DIR beats discovery, so `git -C <elsewhere>` would still
/// act on this repository. A script test that built a fixture repository under a
/// pre-commit hook once replaced the real index and set core.bare on the shared
/// config. Nothing started from here needs the hook's view: every git call names
/// its repository.
pub const HOOK_GIT_VARIABLES: [&str; 10] = [
    "GIT_DIR",
    "GIT_WORK_TREE",
    "GIT_INDEX_FILE",
    "GIT_OBJECT_DIRECTORY",
    "GIT_ALTERNATE_OBJECT_DIRECTORIES",
    "GIT_COMMON_DIR",
    "GIT_PREFIX",
    "GIT_CEILING_DIRECTORIES",
    "GIT_NAMESPACE",
    "GIT_QUARANTINE_PATH",
];

/// A command with the hook's git environment removed.
pub fn command(program: &str) -> Command {
    let mut command = Command::new(program);
    for name in HOOK_GIT_VARIABLES {
        command.env_remove(name);
    }
    command
}

/// Runs a command to completion and returns its exit code and stdout. stderr is
/// discarded. For short queries, not for steps.
pub fn query(mut command: Command) -> io::Result<(i32, String)> {
    let output = command
        .stdin(Stdio::null())
        .stderr(Stdio::null())
        .output()?;
    Ok((
        output.status.code().unwrap_or(-1),
        String::from_utf8_lossy(&output.stdout).into_owned(),
    ))
}

pub enum StepExit {
    Code(i32),
    /// The program itself could not be found.
    NotFound,
}

/// Runs verification steps and keeps a log per step.
pub struct StepRunner {
    pub log_dir: PathBuf,
    /// Echo output live instead of only on failure. CI keeps the whole log.
    pub stream: bool,
    pub failure_tail_lines: usize,
    /// Applied to every step, e.g. the imported MSVC environment.
    pub env: Vec<(OsString, OsString)>,
}

impl StepRunner {
    pub fn log_path(&self, name: &str) -> PathBuf {
        self.log_dir.join(format!("{name}.log"))
    }

    /// Runs `program args` with stdout and stderr merged into the step log.
    pub fn run(
        &self,
        name: &str,
        program: &str,
        args: &[String],
        cwd: &Path,
        extra_env: &[(String, String)],
    ) -> io::Result<(StepExit, PathBuf)> {
        std::fs::create_dir_all(&self.log_dir)?;
        let log_path = self.log_path(name);
        let mut log = File::create(&log_path)?;

        let (reader, writer) = io::pipe()?;
        let mut child = {
            let mut cmd = command(program);
            cmd.args(args)
                .current_dir(cwd)
                .stdin(Stdio::null())
                .stdout(writer.try_clone()?)
                .stderr(writer);
            for (key, value) in &self.env {
                cmd.env(key, value);
            }
            for (key, value) in extra_env {
                cmd.env(key, value);
            }
            match cmd.spawn() {
                Ok(child) => child,
                Err(error) if error.kind() == io::ErrorKind::NotFound => {
                    writeln!(log, "{program}: not found on PATH")?;
                    return Ok((StepExit::NotFound, log_path));
                }
                Err(error) => return Err(error),
            }
            // `cmd` is dropped here, closing this process's copies of the write
            // end. Otherwise the read loop below would never see end of file.
        };

        if self.stream {
            println!("::group::{name}");
        }
        let mut lines = BufReader::new(reader);
        let mut line = Vec::new();
        let stdout = io::stdout();
        loop {
            line.clear();
            if lines.read_until(b'\n', &mut line)? == 0 {
                break;
            }
            log.write_all(&line)?;
            if self.stream {
                let mut out = stdout.lock();
                out.write_all(&line)?;
                out.flush()?;
            }
        }
        let status = child.wait()?;
        if self.stream {
            println!("::endgroup::");
        }
        Ok((StepExit::Code(status.code().unwrap_or(-1)), log_path))
    }

    /// Prints the last lines of a failed step's log.
    pub fn print_tail(&self, name: &str, log_path: &Path) {
        println!();
        println!(
            "---- {name} output (last {} lines) ----",
            self.failure_tail_lines
        );
        for line in tail(log_path, self.failure_tail_lines) {
            println!("{line}");
        }
        println!("Full log: {}", log_path.display());
    }
}

pub fn read_lines(path: &Path) -> Vec<String> {
    match std::fs::read(path) {
        Ok(bytes) => String::from_utf8_lossy(&bytes)
            .lines()
            .map(str::to_string)
            .collect(),
        Err(_) => Vec::new(),
    }
}

pub fn tail(path: &Path, count: usize) -> Vec<String> {
    let lines = read_lines(path);
    let start = lines.len().saturating_sub(count);
    lines[start..].to_vec()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn runner(dir: &Path) -> StepRunner {
        StepRunner {
            log_dir: dir.to_path_buf(),
            stream: false,
            failure_tail_lines: 5,
            env: Vec::new(),
        }
    }

    #[test]
    fn stdout_and_stderr_both_reach_the_log_and_the_exit_code_is_kept() {
        let dir = tempfile::tempdir().unwrap();
        let (program, args): (&str, Vec<String>) = if cfg!(windows) {
            (
                "cmd",
                vec!["/c".into(), "echo out& echo err 1>&2& exit /b 7".into()],
            )
        } else {
            (
                "sh",
                vec!["-c".into(), "echo out; echo err 1>&2; exit 7".into()],
            )
        };
        let (exit, log) = runner(dir.path())
            .run("probe", program, &args, dir.path(), &[])
            .unwrap();
        assert!(matches!(exit, StepExit::Code(7)));
        let text = read_lines(&log).join("\n");
        assert!(text.contains("out") && text.contains("err"), "{text}");
    }

    #[test]
    fn a_missing_program_is_reported_as_not_found() {
        let dir = tempfile::tempdir().unwrap();
        let (exit, _) = runner(dir.path())
            .run("probe", "exo-dev-no-such-program", &[], dir.path(), &[])
            .unwrap();
        assert!(matches!(exit, StepExit::NotFound));
    }

    #[test]
    fn every_hook_git_variable_is_removed_from_a_child() {
        let cmd = command("git");
        let removed: Vec<String> = cmd
            .get_envs()
            .filter(|(_, value)| value.is_none())
            .map(|(key, _)| key.to_string_lossy().to_uppercase())
            .collect();
        for name in HOOK_GIT_VARIABLES {
            assert!(
                removed.contains(&name.to_string()),
                "{name} would leak into a child"
            );
        }
    }

    #[test]
    fn tail_returns_the_last_lines() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("x.log");
        std::fs::write(&path, "1\n2\n3\n4\n").unwrap();
        assert_eq!(tail(&path, 2), vec!["3", "4"]);
        assert!(tail(&dir.path().join("missing"), 2).is_empty());
    }
}
