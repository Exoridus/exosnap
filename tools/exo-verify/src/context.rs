//! What a scenario body sees: the bytes under test, the machine, a private
//! scratch directory, evidence collection and a process job.

use anyhow::{Context as _, Result, anyhow};
use serde_json::{Value, json};
use std::path::{Path, PathBuf};
use std::process::{Child, Command};
use std::time::Duration;

use crate::bundle::{Bundle, FileRole};
use crate::capability::{Capability, CapabilitySet};
use crate::control::{self, Client};
use crate::job::Job;
use crate::scenario::{Evidence, Lane, Step, Stop};

/// An installed-layout product tree: `exosnap.exe` next to its runtime and
/// the updater. Either the candidate's portable package, extracted once per
/// run, or a local tree named on the command line for development lanes.
#[derive(Debug, Clone)]
pub struct Product {
    pub root: PathBuf,
    pub exe: PathBuf,
    pub updater: PathBuf,
    #[allow(dead_code, reason = "Reserved for bundle-specific scenario checks")]
    pub from_bundle: bool,
}

impl Product {
    pub fn from_dir(root: &Path, from_bundle: bool) -> Result<Product> {
        let exe = find_file(root, "exosnap.exe")
            .ok_or_else(|| anyhow!("{} contains no exosnap.exe", root.display()))?;
        let dir = exe.parent().unwrap().to_path_buf();
        let updater = dir.join("exosnap-updater.exe");
        Ok(Product {
            root: dir,
            exe,
            updater,
            from_bundle,
        })
    }
}

fn find_file(root: &Path, name: &str) -> Option<PathBuf> {
    let direct = root.join(name);
    if direct.is_file() {
        return Some(direct);
    }
    let mut stack = vec![(root.to_path_buf(), 0)];
    while let Some((dir, depth)) = stack.pop() {
        for entry in std::fs::read_dir(&dir).ok()?.flatten() {
            let path = entry.path();
            if path.is_dir() && depth < 3 {
                stack.push((path, depth + 1));
            } else if path
                .file_name()
                .is_some_and(|n| n.eq_ignore_ascii_case(name))
            {
                return Some(path);
            }
        }
    }
    None
}

pub fn extract_zip(archive: &Path, out: &Path) -> Result<()> {
    let mut zip = zip::ZipArchive::new(std::fs::File::open(archive)?)?;
    zip.extract(out)
        .with_context(|| format!("extract {}", archive.display()))
}

pub struct Context {
    #[allow(dead_code, reason = "Reserved for lane-specific scenario behavior")]
    pub lane: Lane,
    pub caps: CapabilitySet,
    pub bundle: Option<Bundle>,
    product: Option<Product>,
    local_product: Option<PathBuf>,
    pub run_dir: PathBuf,
    pub scenario_dir: PathBuf,
    pub evidence: Evidence,
    pub artifacts: Vec<String>,
    pub job: Job,
    pub keep_media: bool,
    pub cleanup_failed: bool,
}

impl Context {
    pub fn new(
        lane: Lane,
        caps: CapabilitySet,
        bundle: Option<Bundle>,
        local_product: Option<PathBuf>,
        run_dir: PathBuf,
    ) -> Result<Self> {
        Ok(Context {
            lane,
            caps,
            bundle,
            product: None,
            local_product,
            scenario_dir: run_dir.clone(),
            run_dir,
            evidence: Evidence::default(),
            artifacts: Vec::new(),
            job: Job::new()?,
            keep_media: false,
            cleanup_failed: false,
        })
    }

    /// Resets the per-scenario state before a scenario runs.
    pub fn begin(&mut self, scenario_id: &str) -> Result<()> {
        self.scenario_dir = self.run_dir.join("scenarios").join(scenario_id);
        std::fs::create_dir_all(&self.scenario_dir)?;
        self.evidence = Evidence::default();
        self.artifacts.clear();
        self.job = Job::new()?;
        Ok(())
    }

    pub fn bundle(&self) -> Step<&Bundle> {
        self.bundle.as_ref().ok_or_else(|| {
            Stop::unavailable("this scenario judges a release bundle and none was given (--bundle)")
        })
    }

    pub fn package(&self, role: FileRole) -> Step<PathBuf> {
        Ok(self.bundle()?.require(role)?)
    }

    /// The product tree under test, extracted from the candidate's portable
    /// package on first use.
    pub fn product(&mut self) -> Step<Product> {
        if let Some(p) = &self.product {
            return Ok(p.clone());
        }
        let product = if let Some(bundle) = &self.bundle {
            let target = self.run_dir.join("product");
            if !target.join(".extracted").is_file() {
                let _ = std::fs::remove_dir_all(&target);
                extract_zip(&bundle.require(FileRole::Portable)?, &target)?;
                std::fs::write(target.join(".extracted"), b"")?;
            }
            Product::from_dir(&target, true)?
        } else if let Some(local) = &self.local_product {
            Product::from_dir(local, false)?
        } else {
            return Err(Stop::unavailable(
                "no product under test: pass --bundle for a release lane or --product <install tree> for a development lane",
            ));
        };
        self.product = Some(product.clone());
        Ok(product)
    }

    pub fn has(&self, c: Capability) -> bool {
        self.caps.has(c)
    }

    #[allow(dead_code, reason = "Reserved for scenario capability checks")]
    pub fn require(&self, c: Capability) -> Step {
        if self.caps.has(c) {
            Ok(())
        } else {
            Err(Stop::unavailable(format!("requires {c}")))
        }
    }

    pub fn spawn(&self, command: &mut Command) -> Result<Child> {
        let child = command
            .spawn()
            .with_context(|| format!("start {command:?}"))?;
        self.job.adopt(&child)?;
        Ok(child)
    }

    /// Starts the product with its control endpoint armed and isolated
    /// configuration and output directories, and completes the handshake.
    pub fn launch(&mut self, extra_args: &[&str]) -> Step<App> {
        let product = self.product()?;
        let run_id = control::new_run_id("exov");
        let config = self.scenario_dir.join("config");
        let output = self.scenario_dir.join("output");
        std::fs::create_dir_all(&config)?;
        std::fs::create_dir_all(&output)?;
        let mut command = Command::new(&product.exe);
        command
            .arg("--live-verify-control")
            .arg(&run_id)
            .args(extra_args)
            .current_dir(&product.root)
            .env("EXOSNAP_CONFIG_DIR", &config)
            .env("EXOSNAP_OUTPUT_DIR", &output);
        let mut child = self.spawn(&mut command)?;
        let client = match Client::connect("LiveVerify", &run_id, Duration::from_secs(60)) {
            Ok(c) => c,
            Err(e) => {
                let exited = child.try_wait().ok().flatten();
                let _ = child.kill();
                return Err(match exited {
                    // The product process itself went away before its endpoint came up.
                    Some(status) => Stop::fail(format!(
                        "exosnap.exe exited during startup ({status}) before its control endpoint was reachable"
                    )),
                    None => Stop::Infra(e),
                });
            }
        };
        self.evidence.put("appIdentity", client.identity.clone());
        Ok(App {
            client,
            child,
            output,
            config,
            run_id,
        })
    }

    pub fn keep(&mut self, path: &Path) {
        let reference = path
            .strip_prefix(&self.run_dir)
            .unwrap_or(path)
            .to_string_lossy()
            .replace('\\', "/");
        self.artifacts.push(reference);
    }

    /// Asks the operator a yes/no question. Only scenarios that require the
    /// `operator` capability may call this.
    #[allow(dead_code, reason = "Reserved for operator-assisted scenarios")]
    pub fn ask(&self, question: &str) -> Step<bool> {
        self.require(Capability::Operator)?;
        use std::io::Write;
        print!("\n[operator] {question} [y/n] ");
        std::io::stdout().flush()?;
        let mut line = String::new();
        std::io::stdin().read_line(&mut line)?;
        Ok(matches!(line.trim(), "y" | "Y" | "yes"))
    }

    #[allow(dead_code, reason = "Reserved for operator-assisted scenarios")]
    pub fn announce(&self, text: &str) {
        println!("  [{}] {text}", self.lane.name());
    }
}

/// A running product instance driven over its control endpoint.
pub struct App {
    pub client: Client,
    pub child: Child,
    #[allow(dead_code, reason = "Reserved for recording output checks")]
    pub output: PathBuf,
    #[allow(dead_code, reason = "Reserved for configuration checks")]
    pub config: PathBuf,
    pub run_id: String,
}

impl App {
    pub fn call(&mut self, command: &str, params: Value) -> Result<Value> {
        self.client.call(command, params)
    }

    /// Ends the instance for harness cleanup: the control connection is
    /// dropped and whatever is still running a second later is killed. A
    /// killed product records an unclean exit, so nothing may afterwards be
    /// judged as a normal restart; use [`App::close_gracefully`] for that.
    pub fn kill_for_cleanup(mut self) -> Result<Option<i32>> {
        drop(self.client);
        match crate::tools::wait(&mut self.child, Duration::from_secs(1)) {
            Ok(status) => Ok(status.code()),
            Err(_) => {
                let _ = self.child.kill();
                let status = self.child.wait()?;
                Ok(status.code())
            }
        }
    }

    /// Quits through the product's own shutdown (`app.quit`, the tray Quit's
    /// close-guard chain) and proves that it completed: the process exits
    /// with code 0 within `timeout` and its crash session records a clean
    /// exit. Never falls back to killing the process. A refusal, hang or
    /// unclean exit is the product's verdict; anything left running is
    /// removed by the scenario's job afterwards.
    pub fn close_gracefully(mut self, timeout: Duration) -> Step<GracefulExit> {
        let crash_record = self.config.join("crashes").join("last_session.json");
        let mut instance = RunningInstance {
            client: Some(self.client),
            child: &mut self.child,
            crash_record,
        };
        shut_down(&mut instance, timeout)
    }

    pub fn identity(&self) -> &Value {
        &self.client.identity
    }
}

/// Proof that an instance ended through the product's own shutdown. Only a
/// completed graceful shutdown creates one, so a restart judged against it
/// cannot follow a forced termination.
#[derive(Debug)]
pub struct GracefulExit {
    pub code: i32,
    _proof: (),
}

/// What a graceful shutdown needs from a running instance. There is
/// deliberately no way to kill through it.
trait Shutdown {
    /// Asks the product to quit. The outer error is a transport failure, the
    /// inner one the product's refusal.
    fn request_quit(&mut self) -> Result<Result<(), String>>;
    /// Waits up to `timeout` for the process to end; `None` while it runs.
    fn wait_exit(&mut self, timeout: Duration) -> Result<Option<Option<i32>>>;
    /// The product's own clean-exit flag, `None` when it left no record.
    fn clean_exit_recorded(&self) -> Result<Option<bool>>;
}

struct RunningInstance<'a> {
    client: Option<Client>,
    child: &'a mut Child,
    crash_record: PathBuf,
}

impl Shutdown for RunningInstance<'_> {
    fn request_quit(&mut self) -> Result<Result<(), String>> {
        let client = self
            .client
            .as_mut()
            .ok_or_else(|| anyhow!("the control connection is already closed"))?;
        let answer = client.request("app.quit", json!({}), Duration::from_secs(30));
        // The product must not wait on a client that holds its pipe open.
        drop(self.client.take());
        Ok(answer?.map(|_| ()).map_err(|refusal| refusal.to_string()))
    }

    fn wait_exit(&mut self, timeout: Duration) -> Result<Option<Option<i32>>> {
        let deadline = std::time::Instant::now() + timeout;
        loop {
            if let Some(status) = self.child.try_wait()? {
                return Ok(Some(status.code()));
            }
            if std::time::Instant::now() >= deadline {
                return Ok(None);
            }
            std::thread::sleep(Duration::from_millis(50));
        }
    }

    fn clean_exit_recorded(&self) -> Result<Option<bool>> {
        if !self.crash_record.is_file() {
            return Ok(None);
        }
        let record: Value = serde_json::from_slice(&std::fs::read(&self.crash_record)?)
            .with_context(|| format!("parse {}", self.crash_record.display()))?;
        Ok(record.get("clean_exit").and_then(Value::as_bool))
    }
}

fn shut_down(instance: &mut impl Shutdown, timeout: Duration) -> Step<GracefulExit> {
    match instance.request_quit() {
        Ok(Ok(())) => {}
        Ok(Err(refusal)) => {
            return Err(Stop::fail(format!(
                "the product refused app.quit: {refusal}"
            )));
        }
        // The answer can be lost when the process exits right after sending
        // it. Only an actual exit, judged below, makes that a quit.
        Err(error) => {
            if instance.wait_exit(Duration::ZERO)?.is_none() {
                return Err(Stop::Infra(error.context("app.quit")));
            }
        }
    }
    let Some(code) = instance.wait_exit(timeout)? else {
        return Err(Stop::fail(format!(
            "the product accepted app.quit but was still running after {} s",
            timeout.as_secs()
        )));
    };
    if code != Some(0) {
        return Err(Stop::fail(format!(
            "the product exited with code {code:?} after app.quit"
        )));
    }
    match instance.clean_exit_recorded()? {
        Some(true) => Ok(GracefulExit {
            code: 0,
            _proof: (),
        }),
        Some(false) => Err(Stop::fail(
            "the product exited after app.quit without recording a clean exit",
        )),
        None => Err(Stop::infra(
            "the product left no clean-exit record; a clean shutdown cannot be established",
        )),
    }
}

#[allow(dead_code, reason = "Reserved for control-response assertions")]
pub fn json_path<'a>(value: &'a Value, path: &str) -> Option<&'a Value> {
    path.split('.').try_fold(value, |v, key| v.get(key))
}

#[allow(dead_code, reason = "Reserved for control requests")]
pub fn obj(pairs: &[(&str, Value)]) -> Value {
    let mut map = serde_json::Map::new();
    for (k, v) in pairs {
        map.insert((*k).to_string(), v.clone());
    }
    Value::Object(map)
}

#[allow(dead_code, reason = "Reserved for control requests")]
pub fn empty() -> Value {
    json!({})
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A product that runs until asked to quit and then behaves as configured.
    struct FakeProduct {
        answer: Option<Result<(), String>>,
        exit: Option<Option<i32>>,
        clean: Option<bool>,
        asked: bool,
    }

    impl FakeProduct {
        fn clean() -> Self {
            FakeProduct {
                answer: Some(Ok(())),
                exit: Some(Some(0)),
                clean: Some(true),
                asked: false,
            }
        }
    }

    impl Shutdown for FakeProduct {
        fn request_quit(&mut self) -> Result<Result<(), String>> {
            self.asked = true;
            self.answer
                .clone()
                .ok_or_else(|| anyhow!("control pipe closed"))
        }
        fn wait_exit(&mut self, _: Duration) -> Result<Option<Option<i32>>> {
            Ok(if self.asked { self.exit } else { None })
        }
        fn clean_exit_recorded(&self) -> Result<Option<bool>> {
            Ok(self.clean)
        }
    }

    fn outcome(mut product: FakeProduct) -> Step<GracefulExit> {
        shut_down(&mut product, Duration::from_secs(1))
    }

    #[test]
    fn accepted_quit_with_clean_exit_proves_a_graceful_shutdown() {
        assert_eq!(outcome(FakeProduct::clean()).unwrap().code, 0);
    }

    #[test]
    fn a_refused_quit_is_a_product_verdict() {
        let product = FakeProduct {
            answer: Some(Err("blocked: recording".into())),
            exit: None,
            ..FakeProduct::clean()
        };
        assert!(matches!(outcome(product), Err(Stop::Fail(_))));
    }

    #[test]
    fn a_process_that_outlives_an_accepted_quit_fails_instead_of_being_killed() {
        let product = FakeProduct {
            exit: None,
            ..FakeProduct::clean()
        };
        assert!(matches!(outcome(product), Err(Stop::Fail(_))));
    }

    #[test]
    fn an_exit_without_a_clean_record_is_not_graceful() {
        let unclean = FakeProduct {
            clean: Some(false),
            ..FakeProduct::clean()
        };
        assert!(matches!(outcome(unclean), Err(Stop::Fail(_))));
        let failed = FakeProduct {
            exit: Some(Some(1)),
            ..FakeProduct::clean()
        };
        assert!(matches!(outcome(failed), Err(Stop::Fail(_))));
        let killed = FakeProduct {
            exit: Some(None),
            ..FakeProduct::clean()
        };
        assert!(matches!(outcome(killed), Err(Stop::Fail(_))));
    }

    #[test]
    fn a_missing_clean_exit_record_is_unobserved_not_passed() {
        let product = FakeProduct {
            clean: None,
            ..FakeProduct::clean()
        };
        assert!(matches!(outcome(product), Err(Stop::Infra(_))));
    }

    #[test]
    fn a_lost_answer_counts_only_when_the_process_really_exits() {
        let exited = FakeProduct {
            answer: None,
            ..FakeProduct::clean()
        };
        assert_eq!(outcome(exited).unwrap().code, 0);
        let running = FakeProduct {
            answer: None,
            exit: None,
            ..FakeProduct::clean()
        };
        assert!(matches!(outcome(running), Err(Stop::Infra(_))));
    }
}
