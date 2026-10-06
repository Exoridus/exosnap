//! Child processes: environment hygiene, combined output capture, log files.

use std::ffi::OsString;
use std::fs::File;
use std::io::{self, IsTerminal, Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::mpsc::{self, RecvTimeoutError};
use std::time::{Duration, Instant};

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

#[derive(Debug, PartialEq, Eq)]
pub enum StepExit {
    Code(i32),
    /// The program itself could not be found.
    NotFound,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, clap::ValueEnum)]
pub enum OutputMode {
    #[default]
    Auto,
    Compact,
    Normal,
    Verbose,
    Silent,
}

impl OutputMode {
    pub fn resolve(self, ci: bool, interactive: bool) -> Self {
        match self {
            Self::Auto if ci || !interactive => Self::Compact,
            Self::Auto => Self::Normal,
            explicit => explicit,
        }
    }

    pub fn resolve_environment(self) -> Self {
        self.resolve(
            environment_flag("CI") || environment_flag("GITHUB_ACTIONS"),
            io::stdout().is_terminal(),
        )
    }

    pub fn streams(self) -> bool {
        matches!(self.resolve_environment(), Self::Normal | Self::Verbose)
    }

    pub fn is_silent(self) -> bool {
        self == Self::Silent
    }
}

fn environment_flag(name: &str) -> bool {
    std::env::var_os(name).is_some_and(|value| enabled_flag(&value.to_string_lossy()))
}

fn enabled_flag(value: &str) -> bool {
    let value = value.trim();
    !value.is_empty() && !value.eq_ignore_ascii_case("false") && value != "0"
}

const MAX_TAIL_LINES: usize = 120;
const MAX_TAIL_BYTES: usize = 64 * 1024;
const HEARTBEAT_INTERVAL: Duration = Duration::from_secs(90);

fn heartbeat(
    mode: OutputMode,
    name: &str,
    elapsed: Duration,
    last: &mut Duration,
    output: &mut dyn Write,
) -> io::Result<()> {
    if elapsed.saturating_sub(*last) >= HEARTBEAT_INTERVAL {
        if mode == OutputMode::Compact {
            writeln!(
                output,
                "{name}: still running ({:.0}s)",
                elapsed.as_secs_f64()
            )?;
            output.flush()?;
        }
        *last = elapsed;
    }
    Ok(())
}

struct ConsoleWriter<'a> {
    output: &'a mut dyn Write,
    disconnected: bool,
}

impl Write for ConsoleWriter<'_> {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        // A closed observer must not interrupt log capture or abandon the child.
        if !self.disconnected && self.output.write_all(bytes).is_err() {
            self.disconnected = true;
        }
        Ok(bytes.len())
    }

    fn flush(&mut self) -> io::Result<()> {
        if !self.disconnected && self.output.flush().is_err() {
            self.disconnected = true;
        }
        Ok(())
    }
}

/// Runs verification steps and keeps a log per step.
pub struct StepRunner {
    pub log_dir: PathBuf,
    /// Controls console output only. Every mode retains the complete step log.
    pub output: OutputMode,
    pub failure_tail_lines: usize,
    /// Applied to every step, e.g. the imported MSVC environment.
    pub env: Vec<(OsString, OsString)>,
}

impl StepRunner {
    pub fn output_mode(&self) -> OutputMode {
        self.output.resolve_environment()
    }

    pub fn streams(&self) -> bool {
        self.output_mode().streams()
    }

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
        self.run_with_output(
            name,
            program,
            args,
            cwd,
            extra_env,
            &mut io::stdout().lock(),
        )
    }

    fn run_with_output(
        &self,
        name: &str,
        program: &str,
        args: &[String],
        cwd: &Path,
        extra_env: &[(String, String)],
        output: &mut dyn Write,
    ) -> io::Result<(StepExit, PathBuf)> {
        let started = Instant::now();
        let mode = self.output_mode();
        let mut console = ConsoleWriter {
            output,
            disconnected: false,
        };
        let output: &mut dyn Write = &mut console;
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
                    if !mode.is_silent() {
                        writeln!(
                            output,
                            "{name}: TOOL-MISSING ({:.2}s): {program} not found",
                            started.elapsed().as_secs_f64()
                        )?;
                        writeln!(output, "Full log: {}", log_path.display())?;
                    }
                    return Ok((StepExit::NotFound, log_path));
                }
                Err(error) => {
                    writeln!(log, "could not start {program}: {error}")?;
                    if !mode.is_silent() {
                        writeln!(
                            output,
                            "{name}: FAILED ({:.2}s): {error}",
                            started.elapsed().as_secs_f64()
                        )?;
                        writeln!(output, "Full log: {}", log_path.display())?;
                    }
                    return Err(error);
                }
            }
            // `cmd` is dropped here, closing this process's copies of the write
            // end. Otherwise the read loop below would never see end of file.
        };

        let grouped = mode.streams() && environment_flag("GITHUB_ACTIONS");
        if grouped {
            writeln!(output, "::group::{name}")?;
        }
        let (sender, receiver) = mpsc::sync_channel(8);
        let reader_thread = std::thread::spawn(move || {
            let mut reader = reader;
            let mut chunk = [0_u8; 8192];
            loop {
                match reader.read(&mut chunk) {
                    Ok(0) => break,
                    Ok(length) => {
                        if sender.send(Ok(chunk[..length].to_vec())).is_err() {
                            break;
                        }
                    }
                    Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
                    Err(error) => {
                        let _ = sender.send(Err(error));
                        break;
                    }
                }
            }
        });
        let mut last_heartbeat = Duration::ZERO;
        let capture = (|| -> io::Result<()> {
            loop {
                let timeout = HEARTBEAT_INTERVAL
                    .saturating_sub(started.elapsed().saturating_sub(last_heartbeat));
                match receiver.recv_timeout(timeout) {
                    Ok(chunk) => {
                        let chunk = chunk?;
                        log.write_all(&chunk)?;
                        if mode.streams() {
                            output.write_all(&chunk)?;
                            output.flush()?;
                        }
                    }
                    Err(RecvTimeoutError::Disconnected) => break,
                    Err(RecvTimeoutError::Timeout) => {}
                }
                heartbeat(mode, name, started.elapsed(), &mut last_heartbeat, output)?;
            }
            Ok(())
        })();
        if capture.is_err() {
            let _ = child.kill();
        }
        drop(receiver);
        let reader_result = reader_thread.join();
        let status = loop {
            if let Some(status) = child.try_wait()? {
                break status;
            }
            heartbeat(mode, name, started.elapsed(), &mut last_heartbeat, output)?;
            std::thread::sleep(Duration::from_millis(100));
        };
        capture?;
        reader_result.map_err(|_| io::Error::other("child output reader panicked"))?;
        log.flush()?;
        if grouped {
            writeln!(output, "::endgroup::")?;
        }
        let code = status.code().unwrap_or(-1);
        if !status.success() && !mode.is_silent() {
            self.write_failure(
                name,
                &format!("exit {code}, {:.2}s", started.elapsed().as_secs_f64()),
                &log_path,
                output,
            )?;
        }
        Ok((StepExit::Code(code), log_path))
    }

    /// Prints the last lines of a failed step's log.
    pub fn print_tail(&self, name: &str, log_path: &Path) {
        if self.output_mode().is_silent() || self.streams() {
            return;
        }
        let _ = self.write_tail(name, log_path, &mut io::stdout().lock());
    }

    pub fn print_failure(&self, name: &str, status: &str, log_path: &Path) {
        let _ = self.write_failure(name, status, log_path, &mut io::stdout().lock());
    }

    fn write_failure(
        &self,
        name: &str,
        status: &str,
        log_path: &Path,
        output: &mut dyn Write,
    ) -> io::Result<()> {
        if self.output_mode().is_silent() {
            return Ok(());
        }
        writeln!(output, "{name}: FAILED ({})", text_prefix(status, 4096))?;
        if !self.streams() {
            let lines = tail(log_path, self.failure_tail_lines);
            if let Some(first) = first_error_line(log_path).ok().flatten()
                && !lines.iter().any(|line| line.trim() == first)
            {
                writeln!(output, "First error: {first}")?;
            }
            self.write_tail_lines(name, log_path, &lines, output)
        } else {
            writeln!(output, "Full log: {}", log_path.display())
        }
    }

    fn write_tail(&self, name: &str, log_path: &Path, output: &mut dyn Write) -> io::Result<()> {
        self.write_tail_lines(
            name,
            log_path,
            &tail(log_path, self.failure_tail_lines),
            output,
        )
    }

    fn write_tail_lines(
        &self,
        name: &str,
        log_path: &Path,
        lines: &[String],
        output: &mut dyn Write,
    ) -> io::Result<()> {
        writeln!(
            output,
            "---- {name} output (last {} lines, at most 64 KiB) ----",
            self.failure_tail_lines.min(MAX_TAIL_LINES)
        )?;
        for line in lines {
            writeln!(output, "{line}")?;
        }
        writeln!(output, "Full log: {}", log_path.display())
    }
}

fn text_prefix(text: &str, limit: usize) -> &str {
    let mut end = text.len().min(limit);
    while !text.is_char_boundary(end) {
        end -= 1;
    }
    &text[..end]
}

pub(crate) fn first_error_line(path: &Path) -> io::Result<Option<String>> {
    let mut file = File::open(path)?;
    let mut chunk = [0_u8; 8192];
    let mut line = Vec::with_capacity(4096);
    loop {
        let length = file.read(&mut chunk)?;
        for &byte in &chunk[..length] {
            if byte == b'\n' {
                let text = String::from_utf8_lossy(&line);
                if crate::evidence::is_build_error(&text) {
                    return Ok(Some(text_prefix(text.trim(), 4096).to_owned()));
                }
                line.clear();
            } else if line.len() < 4096 {
                line.push(byte);
            }
        }
        if length == 0 {
            let text = String::from_utf8_lossy(&line);
            return Ok(crate::evidence::is_build_error(&text)
                .then(|| text_prefix(text.trim(), 4096).to_owned()));
        }
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
    bounded_tail(path, count).unwrap_or_default()
}

fn bounded_tail(path: &Path, count: usize) -> io::Result<Vec<String>> {
    let count = count.min(MAX_TAIL_LINES);
    if count == 0 {
        return Ok(Vec::new());
    }
    let mut file = File::open(path)?;
    let byte_limit = (MAX_TAIL_BYTES - 1) as u64;
    file.seek(SeekFrom::Start(
        file.metadata()?.len().saturating_sub(byte_limit),
    ))?;
    let mut bytes = Vec::new();
    file.take(byte_limit).read_to_end(&mut bytes)?;
    let text = String::from_utf8_lossy(&bytes);
    let mut start = text.len().saturating_sub(byte_limit as usize);
    while !text.is_char_boundary(start) {
        start += 1;
    }
    let mut lines: Vec<_> = text[start..]
        .lines()
        .rev()
        .take(count)
        .map(str::to_owned)
        .collect();
    lines.reverse();
    Ok(lines)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn probe(exit_code: i32) -> (&'static str, Vec<String>) {
        if cfg!(windows) {
            (
                "cmd",
                vec![
                    "/d".into(),
                    "/c".into(),
                    format!("echo child-stdout& echo child-stderr 1>&2& exit /b {exit_code}"),
                ],
            )
        } else {
            (
                "sh",
                vec![
                    "-c".into(),
                    format!("echo child-stdout; echo child-stderr >&2; exit {exit_code}"),
                ],
            )
        }
    }

    fn runner(dir: &Path) -> StepRunner {
        StepRunner {
            log_dir: dir.to_path_buf(),
            output: OutputMode::Compact,
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

    #[test]
    fn auto_resolution_respects_ci_and_terminal_without_overriding_explicit_modes() {
        assert_eq!(OutputMode::Auto.resolve(false, true), OutputMode::Normal);
        for (ci, interactive) in [(true, true), (true, false), (false, false)] {
            assert_eq!(
                OutputMode::Auto.resolve(ci, interactive),
                OutputMode::Compact
            );
        }
        for mode in [
            OutputMode::Compact,
            OutputMode::Normal,
            OutputMode::Verbose,
            OutputMode::Silent,
        ] {
            for ci in [false, true] {
                for interactive in [false, true] {
                    assert_eq!(mode.resolve(ci, interactive), mode);
                }
            }
        }
        for value in ["", " ", "0", "false", " FALSE "] {
            assert!(!enabled_flag(value));
        }
        for value in ["true", "1", "yes"] {
            assert!(enabled_flag(value));
        }
    }

    #[test]
    fn compact_heartbeat_is_sparse_and_mode_scoped_even_with_continuous_output() {
        let mut output = Vec::new();
        let mut last = Duration::ZERO;
        for second in 0..=179 {
            heartbeat(
                OutputMode::Compact,
                "build",
                Duration::from_secs(second),
                &mut last,
                &mut output,
            )
            .unwrap();
        }
        assert_eq!(
            String::from_utf8_lossy(&output)
                .matches("still running")
                .count(),
            1
        );
        assert_eq!(last, Duration::from_secs(90));
        heartbeat(
            OutputMode::Compact,
            "build",
            Duration::from_secs(180),
            &mut last,
            &mut output,
        )
        .unwrap();
        assert_eq!(
            String::from_utf8_lossy(&output)
                .matches("still running")
                .count(),
            2
        );
        for mode in [OutputMode::Normal, OutputMode::Verbose, OutputMode::Silent] {
            let mut output = Vec::new();
            let mut last = Duration::ZERO;
            heartbeat(
                mode,
                "build",
                Duration::from_secs(180),
                &mut last,
                &mut output,
            )
            .unwrap();
            assert!(output.is_empty());
        }
    }

    #[test]
    fn compact_success_saves_both_streams_without_echo_and_stderr_is_not_failure() {
        let dir = tempfile::tempdir().unwrap();
        let (program, args) = probe(0);
        let mut console = Vec::new();
        let (exit, log) = runner(dir.path())
            .run_with_output("success", program, &args, dir.path(), &[], &mut console)
            .unwrap();
        assert_eq!(exit, StepExit::Code(0));
        assert!(console.is_empty());
        let text = std::fs::read_to_string(log).unwrap();
        assert!(text.contains("child-stdout") && text.contains("child-stderr"));
    }

    #[test]
    fn explicit_stream_modes_echo_both_streams_and_keep_the_log() {
        for mode in [OutputMode::Normal, OutputMode::Verbose] {
            let dir = tempfile::tempdir().unwrap();
            let mut step = runner(dir.path());
            step.output = mode;
            let (program, args) = probe(0);
            let mut console = Vec::new();
            let (exit, log) = step
                .run_with_output("stream", program, &args, dir.path(), &[], &mut console)
                .unwrap();
            assert_eq!(exit, StepExit::Code(0));
            assert!(String::from_utf8_lossy(&console).contains("child-stdout"));
            assert!(String::from_utf8_lossy(&console).contains("child-stderr"));
            assert!(!std::fs::read(log).unwrap().is_empty());
        }
    }

    #[test]
    fn silent_failure_retains_real_exit_and_full_log_without_console_output() {
        let dir = tempfile::tempdir().unwrap();
        let mut step = runner(dir.path());
        step.output = OutputMode::Silent;
        let (program, args) = probe(23);
        let mut console = Vec::new();
        let (exit, log) = step
            .run_with_output("silent", program, &args, dir.path(), &[], &mut console)
            .unwrap();
        assert_eq!(exit, StepExit::Code(23));
        assert!(console.is_empty());
        assert!(
            std::fs::read_to_string(log)
                .unwrap()
                .contains("child-stderr")
        );
    }

    #[test]
    fn missing_tool_is_distinct_and_silent_respects_the_same_policy() {
        for mode in [OutputMode::Compact, OutputMode::Silent] {
            let dir = tempfile::tempdir().unwrap();
            let mut step = runner(dir.path());
            step.output = mode;
            let mut console = Vec::new();
            let (exit, log) = step
                .run_with_output(
                    "missing",
                    "exo-dev-no-such-output-fixture",
                    &[],
                    dir.path(),
                    &[],
                    &mut console,
                )
                .unwrap();
            assert_eq!(exit, StepExit::NotFound);
            assert!(std::fs::read_to_string(&log).unwrap().contains("not found"));
            if mode == OutputMode::Silent {
                assert!(console.is_empty());
            } else {
                let text = String::from_utf8(console).unwrap();
                assert!(text.contains("TOOL-MISSING"));
                assert!(text.contains(&log.display().to_string()));
            }
        }
    }

    #[test]
    fn failure_tail_is_bounded_for_many_lines_unterminated_bytes_and_invalid_utf8() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("large.log");
        std::fs::write(
            &path,
            (0..1000).map(|n| format!("line-{n}\n")).collect::<String>(),
        )
        .unwrap();
        let lines = tail(&path, usize::MAX);
        assert_eq!(lines.len(), MAX_TAIL_LINES);
        assert_eq!(lines.last().unwrap(), "line-999");
        for byte in [b'x', 0xff] {
            std::fs::write(&path, vec![byte; 2 * 1024 * 1024]).unwrap();
            let lines = tail(&path, usize::MAX);
            assert_eq!(lines.len(), 1);
            assert!(lines[0].len() < MAX_TAIL_BYTES);
        }
    }

    #[test]
    fn compact_failed_child_keeps_huge_unterminated_output_but_only_prints_a_bounded_tail() {
        let dir = tempfile::tempdir().unwrap();
        let input = dir.path().join("unterminated.txt");
        let mut bytes = b"begin-marker".to_vec();
        bytes.resize(2 * 1024 * 1024, b'x');
        bytes.extend_from_slice(b"end-marker");
        std::fs::write(&input, &bytes).unwrap();
        let (program, args) = if cfg!(windows) {
            (
                "cmd",
                vec![
                    "/d".into(),
                    "/c".into(),
                    "type unterminated.txt& echo child-stderr 1>&2& exit /b 23".into(),
                ],
            )
        } else {
            (
                "sh",
                vec![
                    "-c".into(),
                    "cat \"$1\"; echo child-stderr >&2; exit 23".into(),
                    "fixture".into(),
                    input.to_string_lossy().into_owned(),
                ],
            )
        };
        let mut console = Vec::new();
        let (exit, log) = runner(dir.path())
            .run_with_output("huge", program, &args, dir.path(), &[], &mut console)
            .unwrap();
        assert_eq!(exit, StepExit::Code(23));
        let saved = std::fs::read(&log).unwrap();
        assert!(
            saved.starts_with(&bytes),
            "{}",
            String::from_utf8_lossy(&saved[..saved.len().min(512)])
        );
        assert!(String::from_utf8_lossy(&saved).contains("child-stderr"));
        let text = String::from_utf8(console).unwrap();
        assert!(text.contains("FAILED (exit 23,"));
        assert!(text.contains("end-marker") && text.contains("child-stderr"));
        assert!(!text.contains("begin-marker"));
        assert!(text.contains(&log.display().to_string()));
        assert!(text.len() < MAX_TAIL_BYTES + 1024);
    }

    #[test]
    fn broken_console_does_not_abandon_capture_or_replace_the_child_exit() {
        struct Broken;
        impl Write for Broken {
            fn write(&mut self, _: &[u8]) -> io::Result<usize> {
                Err(io::Error::new(io::ErrorKind::BrokenPipe, "observer closed"))
            }
            fn flush(&mut self) -> io::Result<()> {
                Ok(())
            }
        }
        let dir = tempfile::tempdir().unwrap();
        let mut step = runner(dir.path());
        step.output = OutputMode::Verbose;
        let (program, args) = probe(23);
        let (exit, log) = step
            .run_with_output(
                "broken-console",
                program,
                &args,
                dir.path(),
                &[],
                &mut Broken,
            )
            .unwrap();
        assert_eq!(exit, StepExit::Code(23));
        let text = std::fs::read_to_string(log).unwrap();
        assert!(text.contains("child-stdout") && text.contains("child-stderr"));
    }

    #[test]
    fn compact_failure_keeps_the_first_cause_above_the_tail_without_repeating_it() {
        let dir = tempfile::tempdir().unwrap();
        let log = dir.path().join("build.log");
        let first = "source.cpp(4): error C2065: missing_name";
        std::fs::write(
            &log,
            format!("progress\n{first}\n{}", "cascade\n".repeat(200)),
        )
        .unwrap();
        assert_eq!(first_error_line(&log).unwrap().as_deref(), Some(first));
        let mut console = Vec::new();
        runner(dir.path())
            .write_failure("build", "exit 1", &log, &mut console)
            .unwrap();
        let text = String::from_utf8(console).unwrap();
        assert!(text.contains(&format!("First error: {first}")));
        assert_eq!(text.matches(first).count(), 1);
        std::fs::write(&log, format!("{first}\n")).unwrap();
        let mut console = Vec::new();
        runner(dir.path())
            .write_failure("build", "exit 1", &log, &mut console)
            .unwrap();
        assert_eq!(
            String::from_utf8(console).unwrap().matches(first).count(),
            1
        );
    }
}
