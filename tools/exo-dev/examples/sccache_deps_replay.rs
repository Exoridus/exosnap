//! Opt-in Windows experiment: verify cross-checkout sccache/MSVC /showIncludes
//! replay without touching the repository's worktrees or persistent cache.
//! Run from an MSVC Developer PowerShell:
//! cargo run --manifest-path tools/Cargo.toml -p exo-dev --example sccache_deps_replay -- --keep-evidence

use anyhow::{Context, Result, bail};
use serde_json::Value;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

struct Probe {
    root: PathBuf,
    cache: PathBuf,
    port: u16,
    keep: bool,
}

impl Probe {
    fn new(keep: bool) -> Result<Self> {
        let stamp = SystemTime::now().duration_since(UNIX_EPOCH)?.as_nanos();
        let root = std::env::temp_dir().join(format!(
            "exosnap-sccache-replay-{}-{stamp}",
            std::process::id()
        ));
        fs::create_dir(&root).context("could not create the isolated scratch directory")?;
        // Reserve an ephemeral port only long enough to learn its number. This
        // keeps the experiment's sccache server separate from the user's server.
        let socket = std::net::TcpListener::bind("127.0.0.1:0")?;
        let port = socket.local_addr()?.port();
        drop(socket);
        Ok(Self {
            cache: root.join("cache"),
            root,
            port,
            keep,
        })
    }

    fn command(&self, program: &str) -> Command {
        let mut cmd = Command::new(program);
        cmd.env("SCCACHE_DIR", &self.cache)
            .env("SCCACHE_SERVER_PORT", self.port.to_string())
            .env_remove("SCCACHE_GHA_ENABLED");
        // Inherited SCCACHE_BASEDIRS is deliberately not modified.
        cmd
    }

    fn run(&self, program: &str, arguments: &[String]) -> Result<String> {
        let output = self
            .command(program)
            .args(arguments)
            .output()
            .with_context(|| format!("cannot start {program}; check MSVC Developer PowerShell"))?;
        if !output.status.success() {
            bail!(
                "{program} {:?} exited {}\nstdout:\n{}\nstderr:\n{}",
                arguments,
                output.status,
                String::from_utf8_lossy(&output.stdout),
                String::from_utf8_lossy(&output.stderr)
            );
        }
        Ok(String::from_utf8_lossy(&output.stdout).into_owned())
    }

    fn cache_hits(&self) -> Result<u64> {
        let text = self.run(
            "sccache",
            &["--show-stats".into(), "--stats-format=json".into()],
        )?;
        let stats: Value = serde_json::from_str(&text)?;
        let counts = stats
            .pointer("/stats/cache_hits/counts")
            .and_then(Value::as_object)
            .context("sccache did not return cache_hits.counts")?;
        Ok(counts.values().filter_map(Value::as_u64).sum())
    }
}

impl Drop for Probe {
    fn drop(&mut self) {
        let _ = self
            .command("sccache")
            .arg("--stop-server")
            .output();
        if self.keep {
            eprintln!("Evidence retained: {}", self.root.display());
        } else {
            // Only this exact, newly created GUID-like scratch directory.
            let _ = fs::remove_dir_all(&self.root);
        }
    }
}

fn write_fixture(source: &Path) -> Result<()> {
    fs::create_dir_all(source)?;
    fs::write(
        source.join("CMakeLists.txt"),
        "cmake_minimum_required(VERSION 3.27)\n\
         project(exosnap_sccache_replay LANGUAGES CXX)\n\
         set(CMAKE_CXX_STANDARD 20)\n\
         add_executable(exosnap_probe main.cpp)\n",
    )?;
    fs::write(
        source.join("main.cpp"),
        "#include \"marker.h\"\n\
         #include <cstdio>\n\
         int main() { std::printf(\"%d\\n\", marker()); return 0; }\n",
    )?;
    fs::write(source.join("marker.h"), "inline int marker() { return 41; }\n")?;
    Ok(())
}

fn configure_and_build(probe: &Probe, source: &Path, build: &Path) -> Result<()> {
    probe.run(
        "cmake",
        &[
            "-S".into(),
            source.display().to_string(),
            "-B".into(),
            build.display().to_string(),
            "-G".into(),
            "Ninja".into(),
            "-DCMAKE_CXX_COMPILER_LAUNCHER=sccache".into(),
        ],
    )?;
    probe.run(
        "cmake",
        &["--build".into(), build.display().to_string()],
    )?;
    Ok(())
}

fn count_ninja_edges(output: &str) -> usize {
    output
        .lines()
        .filter(|line| {
            let line = line.trim_start();
            line.starts_with('[') && line.contains('/')
        })
        .count()
}

fn foreign_deps<'a>(output: &'a str, root: &Path) -> Vec<&'a str> {
    let needle = root.to_string_lossy().replace('\\', "/").to_ascii_lowercase();
    output
        .lines()
        .filter(|line| {
            line.replace('\\', "/")
                .to_ascii_lowercase()
                .contains(&needle)
        })
        .collect()
}

fn main() -> Result<()> {
    if !cfg!(windows) {
        bail!("this MSVC/Ninja experiment requires a Windows Developer PowerShell");
    }
    let keep = std::env::args().any(|arg| arg == "--keep-evidence");
    let probe = Probe::new(keep)?;
    let source_a = probe.root.join("source-a");
    let source_b = probe.root.join("source-b");
    let build_a = probe.root.join("build-a");
    let build_b = probe.root.join("build-b");
    write_fixture(&source_a)?;
    write_fixture(&source_b)?;

    println!("Scratch: {}", probe.root.display());
    println!(
        "SCCACHE_BASEDIRS (inherited): {:?}",
        std::env::var_os("SCCACHE_BASEDIRS")
    );
    println!("Build A, populating isolated sccache...");
    configure_and_build(&probe, &source_a, &build_a)?;
    let before = probe.cache_hits()?;

    println!("Build B from a different checkout...");
    configure_and_build(&probe, &source_b, &build_b)?;
    let hits = probe.cache_hits()?.saturating_sub(before);
    let deps = probe.run(
        "ninja",
        &[
            "-C".into(),
            build_b.display().to_string(),
            "-t".into(),
            "deps".into(),
        ],
    )?;
    let foreign = foreign_deps(&deps, &source_a);
    println!("Cross-checkout cache hits: {hits}");
    println!("Foreign dependencies: {}", foreign.len());
    for line in foreign.iter().take(8) {
        println!("  {line}");
    }

    // Remove only the scratch source A, never a user worktree. This exposes
    // whether replayed foreign includes invalidate a no-op build in B.
    fs::remove_dir_all(&source_a)?;
    let no_op = probe.run(
        "ninja",
        &["-C".into(), build_b.display().to_string(), "-n".into()],
    )?;
    let pending = count_ninja_edges(&no_op);
    println!("Pending no-op edges after retiring A: {pending}");

    // A missed dependency here would be a correctness failure, not just
    // a performance problem. Always check it, even if cache reuse was absent.
    std::thread::sleep(Duration::from_millis(1100));
    fs::write(
        source_b.join("marker.h"),
        "inline int marker() { return 42; }\n",
    )?;
    probe.run(
        "cmake",
        &["--build".into(), build_b.display().to_string()],
    )?;
    let binary = build_b.join("exosnap_probe.exe");
    let result = probe
        .run(&binary.to_string_lossy(), &[])
        .context("cannot execute probe after changing its included header")?;
    if result.trim() != "42" {
        bail!(
            "HEADER DEPENDENCY FAILURE: expected 42 after changing marker.h, observed {:?}",
            result.trim()
        );
    }
    println!("PASS: current header edit rebuilt the executable correctly.");

    if hits == 0 {
        println!("INCONCLUSIVE: no cross-checkout cache hit. No claim about dependency replay.");
    } else if !foreign.is_empty() {
        println!("REPRODUCED: sccache replay referenced the previous source root.");
    } else {
        println!("PASS: cross-checkout cache hit without foreign dependency paths.");
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn recognizes_foreign_msvc_paths() {
        let text = "build/test.obj: #deps 2\n    C:\\temp\\other\\marker.h\n    C:\\sdk\\stdio.h\n";
        let matches = foreign_deps(text, Path::new("C:\\temp\\other"));
        assert_eq!(matches.len(), 1);
        assert!(matches[0].contains("marker.h"));
    }

    #[test]
    fn counts_dry_run_commands_without_counting_status() {
        let text = "[1/2] Compiling CXX\n[2/2] Linking CXX\nninja: no work to do.\n";
        assert_eq!(count_ninja_edges(text), 2);
        assert_eq!(count_ninja_edges("ninja: no work to do."), 0);
    }
}
