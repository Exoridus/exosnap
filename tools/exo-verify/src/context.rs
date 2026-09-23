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
        self.artifacts.push(path.display().to_string());
    }

    /// Asks the operator a yes/no question. Only scenarios that require the
    /// `operator` capability may call this.
    pub fn ask(&self, question: &str) -> Step<bool> {
        self.require(Capability::Operator)?;
        use std::io::Write;
        print!("\n[operator] {question} [y/n] ");
        std::io::stdout().flush()?;
        let mut line = String::new();
        std::io::stdin().read_line(&mut line)?;
        Ok(matches!(line.trim(), "y" | "Y" | "yes"))
    }

    pub fn announce(&self, text: &str) {
        println!("  [{}] {text}", self.lane.name());
    }
}

/// A running product instance driven over its control endpoint.
pub struct App {
    pub client: Client,
    pub child: Child,
    pub output: PathBuf,
    pub config: PathBuf,
    pub run_id: String,
}

impl App {
    pub fn call(&mut self, command: &str, params: Value) -> Result<Value> {
        self.client.call(command, params)
    }

    /// Ends the instance. The job kills anything left; this is only the
    /// polite first attempt so the product can flush its logs.
    pub fn close(mut self) -> Result<Option<i32>> {
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

    pub fn identity(&self) -> &Value {
        &self.client.identity
    }
}

pub fn json_path<'a>(value: &'a Value, path: &str) -> Option<&'a Value> {
    path.split('.').try_fold(value, |v, key| v.get(key))
}

pub fn obj(pairs: &[(&str, Value)]) -> Value {
    let mut map = serde_json::Map::new();
    for (k, v) in pairs {
        map.insert((*k).to_string(), v.clone());
    }
    Value::Object(map)
}

pub fn empty() -> Value {
    json!({})
}
