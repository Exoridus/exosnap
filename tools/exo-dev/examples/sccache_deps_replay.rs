//! Opt-in Windows experiment: measure cross-checkout sccache/MSVC dependency
//! replay without touching repository worktrees, the developer's sccache
//! server or its cache. Two scenarios run in separate checkouts so neither can
//! explain the other's result:
//!
//! * header invalidation: a consumer hits the producer's cache entry while the
//!   producer still exists, then its included header changes. The rebuild must
//!   happen on its own account.
//! * no-op cost: a third checkout reuses the same entry, the producer is then
//!   retired, and the checkout's pending build edges and missing dependency
//!   paths are counted.
//!
//! Run from an MSVC Developer PowerShell:
//! cargo run --manifest-path tools/Cargo.toml -p exo-dev --example sccache_deps_replay -- --keep-evidence

use anyhow::{Context, Result, bail};
use serde_json::Value;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

/// The build directory layout a repository worktree uses, so the relative
/// paths replayed from a cache entry are spelled the way a real tree spells
/// them.
const BUILD_DIR: &str = "build/windows-x64-ninja-debug";

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
        // Reserve an ephemeral port only long enough to learn its number. This
        // keeps the experiment's sccache server separate from the user's server.
        let socket = std::net::TcpListener::bind("127.0.0.1:0")?;
        let port = socket.local_addr()?.port();
        drop(socket);
        // After the fallible port lookup, so a failure here cannot leak the
        // directory. `create_dir` also guarantees the root was never in use.
        fs::create_dir(&root).context("could not create the isolated scratch directory")?;
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
            // A developer's cache backend or basedir configuration must not
            // decide what this probe measures.
            .env_remove("SCCACHE_CONF")
            .env_remove("SCCACHE_GHA_CACHE_URL")
            .env_remove("SCCACHE_GHA_RUNTIME_TOKEN")
            .env_remove("SCCACHE_GHA_ENABLED");
        cmd
    }

    /// Runs a command and fails the probe when it fails. A measurement that
    /// silently observed nothing is worse than an aborted run.
    fn run_in(&self, cwd: &Path, program: &str, arguments: &[String]) -> Result<String> {
        let output = self
            .command(program)
            .current_dir(cwd)
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

    fn run(&self, program: &str, arguments: &[String]) -> Result<String> {
        let cwd = std::env::current_dir()?;
        self.run_in(&cwd, program, arguments)
    }

    /// Whitespace-delimited single-line version of a tool's banner. Used only
    /// for the environment record, so a non-zero exit is not a failure.
    fn banner(&self, program: &str, arguments: &[&str]) -> String {
        let output = self.command(program).args(arguments).output();
        match output {
            Ok(output) => {
                let mut text = String::from_utf8_lossy(&output.stdout).into_owned();
                text.push_str(&String::from_utf8_lossy(&output.stderr));
                let lines: Vec<&str> = text
                    .lines()
                    .map(str::trim)
                    .filter(|line| !line.is_empty())
                    .collect();
                // A version line is what makes a report attributable; a usage
                // banner is only the fallback.
                lines
                    .iter()
                    .find(|line| line.contains("ersion"))
                    .or_else(|| lines.first())
                    .copied()
                    .unwrap_or("(no banner)")
                    .to_string()
            }
            Err(_) => "missing".to_string(),
        }
    }

    fn cache_hits(&self) -> Result<u64> {
        let text = self.run(
            "sccache",
            &["--show-stats".into(), "--stats-format=json".into()],
        )?;
        let stats: Value = serde_json::from_str(&text)?;
        // The client talks to whatever server answers on the probe port. A
        // stale or foreign server would silently report another cache's
        // counters and make every later number meaningless.
        if let Some(location) = stats["cache_location"].as_str() {
            let marker = self
                .root
                .file_name()
                .unwrap_or_default()
                .to_string_lossy()
                .to_ascii_lowercase();
            if !location.to_ascii_lowercase().contains(&marker) {
                bail!(
                    "the sccache server on port {} reports cache {location:?} instead of the probe cache; stop it and rerun",
                    self.port
                );
            }
        }
        let counts = stats
            .pointer("/stats/cache_hits/counts")
            .and_then(Value::as_object)
            .context("sccache did not return cache_hits.counts")?;
        Ok(counts.values().filter_map(Value::as_u64).sum())
    }

    /// Fails before any measurement when a required tool is missing, so a
    /// toolchain problem is never reported as a product result.
    fn preflight(&self) -> Result<()> {
        for program in ["cmake", "ninja", "sccache"] {
            if self.banner(program, &["--version"]) == "missing" {
                bail!("{program} is not on PATH; run this from an MSVC Developer PowerShell");
            }
        }
        if self.banner("cl", &[]) == "missing" {
            bail!("cl.exe is not on PATH; run this from an MSVC Developer PowerShell");
        }
        Ok(())
    }

    fn describe_environment(&self) {
        println!(
            "Tools: {} | {} | {} | {}",
            self.banner("cl", &[]),
            self.banner("sccache", &["--version"]),
            self.banner("ninja", &["--version"]),
            self.banner("cmake", &["--version"])
        );
        println!(
            "SCCACHE_BASEDIRS (inherited): {:?}",
            std::env::var_os("SCCACHE_BASEDIRS")
        );
    }
}

impl Drop for Probe {
    fn drop(&mut self) {
        let _ = self.command("sccache").arg("--stop-server").output();
        if self.keep {
            eprintln!("Evidence retained: {}", self.root.display());
        } else if let Err(error) = fs::remove_dir_all(&self.root) {
            // A locked cache file is the usual cause. Say so instead of
            // leaving the directory behind as an unexplained leftover.
            eprintln!(
                "warning: could not remove the scratch directory {}: {error}",
                self.root.display()
            );
        }
    }
}

/// One disposable checkout shaped like a repository worktree: sources at the
/// root, Ninja tree under `build/<preset>`.
struct Worktree {
    root: PathBuf,
}

impl Worktree {
    fn create(base: &Path, name: &str) -> Result<Self> {
        let root = base.join(name);
        fs::create_dir_all(&root)?;
        fs::write(
            root.join("CMakeLists.txt"),
            "cmake_minimum_required(VERSION 3.27)\n\
             project(exosnap_sccache_replay LANGUAGES CXX)\n\
             set(CMAKE_CXX_STANDARD 20)\n\
             add_executable(exosnap_probe main.cpp)\n",
        )?;
        fs::write(
            root.join("main.cpp"),
            "#include \"marker.h\"\n\
             #include <cstdio>\n\
             int main() { std::printf(\"%d\\n\", marker()); return 0; }\n",
        )?;
        fs::write(root.join("marker.h"), MARKER_41)?;
        Ok(Self { root })
    }

    fn build_dir(&self) -> PathBuf {
        self.root.join(BUILD_DIR)
    }

    fn configure(&self, probe: &Probe) -> Result<()> {
        probe.run_in(
            &self.root,
            "cmake",
            &[
                "-S".into(),
                self.root.display().to_string(),
                "-B".into(),
                self.build_dir().display().to_string(),
                "-G".into(),
                "Ninja".into(),
                "-DCMAKE_BUILD_TYPE=Debug".into(),
                "-DCMAKE_CXX_COMPILER_LAUNCHER=sccache".into(),
                // sccache does not cache MSVC compiles that write a compiler
                // PDB (`/Zi`) and passes them through instead. The repository
                // embeds the debug information (`/Z7`), so the fixture must
                // match that or it measures nothing.
                "-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded".into(),
            ],
        )?;
        Ok(())
    }

    fn build(&self, probe: &Probe) -> Result<()> {
        probe.run_in(
            &self.root,
            "cmake",
            &["--build".into(), self.build_dir().display().to_string()],
        )?;
        Ok(())
    }

    fn pending_edges(&self, probe: &Probe) -> Result<usize> {
        let output = probe.run_in(
            &self.root,
            "ninja",
            &[
                "-C".into(),
                self.build_dir().display().to_string(),
                "-n".into(),
            ],
        )?;
        Ok(count_ninja_edges(&output))
    }

    fn dependencies(&self, probe: &Probe) -> Result<String> {
        probe.run_in(
            &self.root,
            "ninja",
            &[
                "-C".into(),
                self.build_dir().display().to_string(),
                "-t".into(),
                "deps".into(),
            ],
        )
    }

    fn run_probe_binary(&self, probe: &Probe) -> Result<String> {
        let binary = self.build_dir().join("exosnap_probe.exe");
        probe
            .run(&binary.to_string_lossy(), &[])
            .context("cannot execute the fixture's executable")
    }

    /// Deletes this checkout, which is created by the probe and never a
    /// repository worktree.
    fn retire(self) -> Result<()> {
        fs::remove_dir_all(&self.root)
            .with_context(|| format!("could not retire {}", self.root.display()))
    }
}

const MARKER_41: &str = "inline int marker() { return 41; }\n";
const MARKER_42: &str = "inline int marker() { return 42; }\n";

/// Dependency entries in `ninja -t deps` naming files that do not exist.
/// After the populating checkout is retired, every such entry is a replayed
/// path from the cache entry rather than a header this build can consult.
fn missing_deps(output: &str, build: &Path) -> Vec<String> {
    output
        .lines()
        .filter(|line| line.starts_with(char::is_whitespace))
        .map(str::trim)
        .filter(|dependency| !dependency.is_empty() && !build.join(dependency).exists())
        .map(str::to_string)
        .collect()
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

fn foreign_deps(output: &str, root: &Path) -> Vec<String> {
    // sccache replays the include lines recorded by the compile that populated
    // the entry. Depending on how the producer's include directories were
    // spelled, that is the absolute source root or a path relative to the
    // consuming build directory, so both spellings have to be recognized.
    let absolute = root
        .to_string_lossy()
        .replace('\\', "/")
        .to_ascii_lowercase();
    let relative = format!(
        "/{}/",
        root.file_name()
            .unwrap_or_default()
            .to_string_lossy()
            .to_ascii_lowercase()
    );
    output
        .lines()
        .filter(|line| {
            let normalized = line.replace('\\', "/").to_ascii_lowercase();
            normalized.contains(&absolute) || normalized.contains(&relative)
        })
        .map(str::to_string)
        .collect()
}

#[derive(Clone, Debug, PartialEq, Eq)]
enum Class {
    Reproduced,
    NotReproducible,
    Inconclusive,
    Regression,
}

impl Class {
    fn label(&self) -> &'static str {
        match self {
            Class::Reproduced => "REPRODUCED",
            Class::NotReproducible => "NOT_REPRODUCIBLE",
            Class::Inconclusive => "INCONCLUSIVE",
            Class::Regression => "REGRESSION",
        }
    }
}

/// What the two scenarios observed. Every field is a measurement; the verdict
/// is derived from them so a partial run cannot read as a healthy one.
#[derive(Clone, Debug)]
struct Observation {
    /// Cache hits from rebuilding the producer in place. Zero means the
    /// fixture itself is not cacheable here, so nothing else is measurable.
    control_hits: u64,
    /// Cache hits of the consumer that keeps a live producer.
    consumer_hits: u64,
    /// Pending edges in the consumer before its header changed.
    header_pending_before: usize,
    /// Pending edges in the consumer after its header changed.
    header_pending_after: usize,
    /// What the consumer's executable printed after the header change.
    header_output: String,
    /// Cache hits of the second consumer, which the no-op scenario measures.
    observer_hits: u64,
    /// Pending edges in the observer after the producer was retired.
    observer_pending: usize,
    /// Dependency paths in the observer naming a file that no longer exists.
    observer_missing: usize,
    /// Replayed include lines that name the producer.
    foreign: usize,
}

/// The replay finding on its own, independent of the header check. Both are
/// reported so a healthy dependency graph cannot hide a replayed path and a
/// vanished replay cannot read as a healthy graph.
fn replay_verdict(observation: &Observation) -> &'static str {
    if observation.control_hits == 0 {
        "INCONCLUSIVE: the fixture itself was not served from the cache"
    } else if observation.consumer_hits == 0 || observation.observer_hits == 0 {
        "NOT_REPRODUCIBLE: the cache works but was not reused across checkouts"
    } else if observation.foreign > 0
        || observation.observer_missing > 0
        || observation.observer_pending > 0
    {
        "REPRODUCED: replayed paths outlive the checkout that produced them"
    } else {
        "NOT_REPRODUCIBLE: cross-checkout reuse without a dependency replay"
    }
}

fn classify(observation: &Observation) -> (Class, &'static str) {
    if observation.header_output.trim() != "42" {
        return (
            Class::Regression,
            "the consumer did not pick up its own header change",
        );
    }
    if observation.control_hits == 0 {
        return (
            Class::Inconclusive,
            "sccache did not serve the same-checkout control, so the fixture proves nothing",
        );
    }
    if observation.consumer_hits == 0 || observation.observer_hits == 0 {
        return (
            Class::NotReproducible,
            "the cache works for this fixture but was not reused across checkouts",
        );
    }
    if observation.foreign > 0
        || observation.observer_missing > 0
        || observation.observer_pending > 0
    {
        return (
            Class::Reproduced,
            "cross-checkout reuse left replayed paths that outlive the producing checkout",
        );
    }
    (
        Class::NotReproducible,
        "cross-checkout reuse happened without a dependency replay",
    )
}

fn main() -> Result<()> {
    if !cfg!(windows) {
        bail!("this MSVC/Ninja experiment requires a Windows Developer PowerShell");
    }
    let keep = std::env::args().any(|arg| arg == "--keep-evidence");
    let probe = Probe::new(keep)?;
    probe.preflight()?;

    println!("Scratch: {}", probe.root.display());
    probe.describe_environment();

    let producer = Worktree::create(&probe.root, "producer")?;
    let consumer = Worktree::create(&probe.root, "consumer")?;

    // The producer populates the isolated cache.
    println!("\n== populate: producer ==");
    producer.configure(&probe)?;
    producer.build(&probe)?;

    // Control: rebuilding the same checkout from scratch must hit. Without it,
    // a zero cross-checkout hit count cannot distinguish a root-sensitive key
    // from a fixture that sccache never caches.
    probe.run_in(
        &producer.root,
        "ninja",
        &[
            "-C".into(),
            producer.build_dir().display().to_string(),
            "-t".into(),
            "clean".into(),
        ],
    )?;
    let before_control = probe.cache_hits()?;
    producer.build(&probe)?;
    let control_hits = probe.cache_hits()?.saturating_sub(before_control);
    println!("Same-checkout cache hits (control): {control_hits}");

    // Scenario 1: header invalidation, with the producer still present.
    // Sampled after configuring, so a hit from CMake's compiler checks cannot
    // be mistaken for reuse of the fixture's object.
    println!("\n== scenario 1: header invalidation ==");
    consumer.configure(&probe)?;
    let before_consumer = probe.cache_hits()?;
    consumer.build(&probe)?;
    let consumer_hits = probe.cache_hits()?.saturating_sub(before_consumer);
    println!("Consumer cache hits: {consumer_hits}");

    let foreign = foreign_deps(&consumer.dependencies(&probe)?, &producer.root);
    println!(
        "Replayed include lines naming the producer: {}",
        foreign.len()
    );
    for line in foreign.iter().take(8) {
        println!("  {line}");
    }

    let header_pending_before = consumer.pending_edges(&probe)?;
    println!("Pending edges before the header change: {header_pending_before}");

    // The header changes while the producer exists, so only the consumer's own
    // dependency record can trigger the rebuild.
    std::thread::sleep(Duration::from_millis(1100));
    fs::write(consumer.root.join("marker.h"), MARKER_42)?;
    let header_pending_after = consumer.pending_edges(&probe)?;
    println!("Pending edges after the header change: {header_pending_after}");

    consumer.build(&probe)?;
    let header_output = consumer.run_probe_binary(&probe)?;
    println!("Executable reports after the header change: {header_output:?}");
    if header_output.trim() != "42" {
        println!(
            "HEADER DEPENDENCY FAILURE: the consumer still reports {:?} after its own header \
             changed. Ninja did not treat that header as a dependency of the object.",
            header_output.trim()
        );
    }

    // Scenario 2: no-op cost, in a second consumer. This checkout is built
    // before the producer is retired, so its cache reuse is a real cross
    // checkout reuse; afterwards nothing in it changes.
    println!("\n== scenario 2: no-op cost ==");
    let observer = Worktree::create(&probe.root, "observer")?;
    observer.configure(&probe)?;
    let before_observer = probe.cache_hits()?;
    observer.build(&probe)?;
    let observer_hits = probe.cache_hits()?.saturating_sub(before_observer);
    println!("Observer cache hits: {observer_hits}");

    producer.retire()?;
    let observer_pending = observer.pending_edges(&probe)?;
    let missing = missing_deps(&observer.dependencies(&probe)?, &observer.build_dir());
    println!("Pending edges after retiring the producer: {observer_pending}");
    println!("Dependencies naming a missing file: {}", missing.len());
    for line in missing.iter().take(8) {
        println!("  {line}");
    }

    let observation = Observation {
        control_hits,
        consumer_hits,
        header_pending_before,
        header_pending_after,
        header_output,
        observer_hits,
        observer_pending,
        observer_missing: missing.len(),
        foreign: foreign.len(),
    };
    let (class, summary) = classify(&observation);
    println!(
        "\nheader invalidation: {}",
        if observation.header_output.trim() == "42" {
            "OK".to_string()
        } else {
            format!(
                "REGRESSION: the consumer still reports {:?} after its own header changed",
                observation.header_output.trim()
            )
        }
    );
    println!("dependency replay: {}", replay_verdict(&observation));
    println!("verdict: {} ({summary})", class.label());
    println!(
        "  control_hits={} consumer_hits={} observer_hits={} foreign={} \
         header_pending={}->{} observer_pending={} observer_missing={}",
        observation.control_hits,
        observation.consumer_hits,
        observation.observer_hits,
        observation.foreign,
        observation.header_pending_before,
        observation.header_pending_after,
        observation.observer_pending,
        observation.observer_missing
    );
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn observation() -> Observation {
        Observation {
            control_hits: 1,
            consumer_hits: 1,
            header_pending_before: 0,
            header_pending_after: 1,
            header_output: "42".into(),
            observer_hits: 1,
            observer_pending: 0,
            observer_missing: 0,
            foreign: 0,
        }
    }

    #[test]
    fn a_missed_header_change_is_a_regression_even_without_other_evidence() {
        let mut stale = observation();
        stale.header_output = "41".into();
        stale.header_pending_after = 0;
        stale.consumer_hits = 0;
        assert_eq!(classify(&stale).0, Class::Regression);
    }

    #[test]
    fn an_uncacheable_fixture_is_inconclusive() {
        let mut uncached = observation();
        uncached.control_hits = 0;
        uncached.consumer_hits = 0;
        uncached.observer_hits = 0;
        assert_eq!(classify(&uncached).0, Class::Inconclusive);
    }

    #[test]
    fn a_cache_without_cross_checkout_reuse_is_not_reproducing() {
        let mut no_reuse = observation();
        no_reuse.consumer_hits = 0;
        assert_eq!(classify(&no_reuse).0, Class::NotReproducible);
    }

    #[test]
    fn replayed_paths_that_outlive_the_producer_reproduce_the_defect() {
        let mut replay = observation();
        replay.observer_missing = 1;
        assert_eq!(classify(&replay).0, Class::Reproduced);
        let mut pending = observation();
        pending.observer_pending = 3;
        assert_eq!(classify(&pending).0, Class::Reproduced);
    }

    #[test]
    fn recognizes_foreign_msvc_paths() {
        let text = "build/test.obj: #deps 2\n    C:\\temp\\other\\marker.h\n    C:\\sdk\\stdio.h\n";
        let matches = foreign_deps(text, Path::new("C:\\temp\\other"));
        assert_eq!(matches.len(), 1);
        assert!(matches[0].contains("marker.h"));
    }

    #[test]
    fn recognizes_foreign_paths_relative_to_the_consuming_build() {
        let text = "CMakeFiles/p.dir/main.cpp.obj: #deps 1\n    ../../../producer/marker.h\n";
        let matches = foreign_deps(text, Path::new("C:\\temp\\probe\\producer"));
        assert_eq!(matches.len(), 1);
    }

    #[test]
    fn reports_dependencies_that_a_retired_checkout_left_missing() {
        let dir = tempfile::tempdir().unwrap();
        let present = dir.path().join("marker.h");
        std::fs::write(&present, "").unwrap();
        let text = format!(
            "a.obj: #deps 2\n    {}\n    {}\n",
            dir.path().join("retired/marker.h").display(),
            present.display()
        );
        let missing = missing_deps(&text, dir.path());
        assert_eq!(missing.len(), 1, "{missing:?}");
        assert!(missing[0].contains("retired"));
        assert!(missing_deps("a.obj: #deps 0\n", dir.path()).is_empty());
    }

    #[test]
    fn counts_dry_run_commands_without_counting_status() {
        let text = "[1/2] Compiling CXX\n[2/2] Linking CXX\nninja: no work to do.\n";
        assert_eq!(count_ninja_edges(text), 2);
        assert_eq!(count_ninja_edges("ninja: no work to do."), 0);
    }
}
