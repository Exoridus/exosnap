//! exo-verify: ExoSnap's release, E2E and documentation-structure verifier.
//!
//! Tests measure facts. A per-candidate plan says which facts matter. A
//! maintainer may accept a known risk explicitly. Nothing here publishes.

mod bundle;
mod capability;
mod context;
mod control;
mod docs;
mod feed;
#[cfg(windows)]
mod holders;
mod job;
mod manifest;
mod media;
mod model;
mod package;
mod pattern;
mod pe;
mod plan;
mod report;
mod runner;
mod scenario;
mod scenarios;
mod stimulus;
mod tools;
#[cfg(windows)]
mod win;

use anyhow::{Context as _, Result, bail, ensure};
use clap::{Args, Parser, Subcommand};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use crate::capability::Capability;
use crate::model::LaneResult;
use crate::plan::{Decision, DecisionKind, Decisions, PLAN_SCHEMA, PlannedScenario, ReleasePlan};
use crate::scenario::Lane;

#[derive(Parser)]
#[command(
    name = "exo-verify",
    version,
    about = "ExoSnap release, E2E and documentation verification"
)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Build, inspect or transport the immutable candidate bundle.
    #[command(subcommand)]
    Bundle(BundleCommand),
    /// Freeze the release plan for a candidate bundle.
    #[command(subcommand)]
    Plan(PlanCommand),
    /// Run a profile or lane and write its result document.
    Run(RunArgs),
    /// Merge lane results and decisions into the release report.
    #[command(subcommand)]
    Report(ReportCommand),
    /// Recompute and print readiness from bound evidence; exit 0 only when ready for approval.
    Status {
        #[arg(long)]
        bundle: PathBuf,
        #[arg(long)]
        plan: PathBuf,
        #[arg(long)]
        report: PathBuf,
        #[arg(long)]
        decisions: Option<PathBuf>,
        #[arg(long = "results", required = true)]
        results: Vec<PathBuf>,
    },
    /// Record an explicit maintainer decision for one scenario.
    Accept(AcceptArgs),
    /// Build, sign and verify the update manifest.
    #[command(subcommand)]
    Manifest(ManifestCommand),
    /// Stage, validate and package a built Release tree (portable ZIP and MSI).
    Package(package::PackageArgs),
    /// Structural documentation checks.
    #[command(subcommand)]
    Docs(DocsCommand),
    /// Show what this machine can do.
    Capabilities {
        #[arg(long = "attest")]
        attest: Vec<String>,
    },
    /// List scenarios, optionally for one lane.
    List {
        #[arg(long)]
        lane: Option<String>,
    },
    /// Show the deterministic verification stimulus window (used by GPU scenarios).
    Stimulus(stimulus::StimulusArgs),
    /// Serve a local HTTPS update feed (used by the update lane).
    #[command(hide = true)]
    Feed(feed::FeedArgs),
}

#[derive(Subcommand)]
enum BundleCommand {
    Create {
        #[arg(long)]
        out: PathBuf,
        #[arg(long)]
        version: String,
        #[arg(long)]
        commit: String,
        #[arg(long)]
        candidate_id: String,
        #[arg(long)]
        installer: PathBuf,
        #[arg(long)]
        portable: PathBuf,
        #[arg(long = "runtime")]
        runtimes: Vec<PathBuf>,
        #[arg(long = "metadata")]
        metadata: Vec<PathBuf>,
        /// key=value toolchain facts, repeatable.
        #[arg(long = "toolchain")]
        toolchain: Vec<String>,
        /// key=value release inputs, repeatable.
        #[arg(long = "input")]
        inputs: Vec<String>,
    },
    /// Rehash every file and print the bundle identity.
    Verify { path: PathBuf },
    Pack {
        path: PathBuf,
        #[arg(long)]
        out: PathBuf,
    },
    Unpack {
        archive: PathBuf,
        #[arg(long)]
        out: PathBuf,
    },
}

#[derive(Subcommand)]
enum PlanCommand {
    Create {
        #[arg(long)]
        bundle: PathBuf,
        #[arg(long)]
        out: PathBuf,
    },
    Verify {
        #[arg(long)]
        bundle: PathBuf,
        #[arg(long)]
        plan: PathBuf,
    },
}

#[derive(Args)]
struct RunArgs {
    /// quick, preflight, release-ci, release-gpu, release-hardware or nightly.
    #[arg(long)]
    profile: String,
    /// One lane of a multi-lane profile (release-ci-core, release-ci-install, release-ci-update).
    #[arg(long)]
    lane: Option<String>,
    /// Candidate bundle directory or packed archive (release profiles).
    #[arg(long)]
    bundle: Option<PathBuf>,
    /// Local installed-layout product tree (development profiles).
    #[arg(long)]
    product: Option<PathBuf>,
    #[arg(long)]
    out: PathBuf,
    #[arg(long = "only")]
    only: Vec<String>,
    #[arg(long = "skip")]
    skip: Vec<String>,
    /// Declare an operator-provided capability (operator, physical-audio-disconnect).
    #[arg(long = "attest")]
    attest: Vec<String>,
    /// Keep recordings of passing scenarios.
    #[arg(long)]
    keep_media: bool,
}

#[derive(Subcommand)]
enum ReportCommand {
    Merge {
        #[arg(long)]
        bundle: PathBuf,
        #[arg(long)]
        plan: PathBuf,
        #[arg(long)]
        decisions: Option<PathBuf>,
        /// Lane result files or directories containing them.
        #[arg(long = "results", required = true)]
        results: Vec<PathBuf>,
        #[arg(long)]
        out: PathBuf,
    },
    Verify {
        #[arg(long)]
        bundle: PathBuf,
        #[arg(long)]
        plan: PathBuf,
        #[arg(long)]
        report: PathBuf,
        #[arg(long)]
        decisions: Option<PathBuf>,
        #[arg(long = "results", required = true)]
        results: Vec<PathBuf>,
    },
}

#[derive(Args)]
struct AcceptArgs {
    scenario: String,
    #[arg(long)]
    reason: String,
    /// The decision file for this candidate; created when absent.
    #[arg(long)]
    decisions: PathBuf,
    /// Bundle SHA-256 the decision applies to (from the plan or report).
    #[arg(long)]
    bundle_sha256: String,
    /// Accept a FAIL as a known shipped risk (ACCEPTED_RISK).
    #[arg(long)]
    risk: bool,
    #[arg(long)]
    by: Option<String>,
}

#[derive(Subcommand)]
enum ManifestCommand {
    /// Write update-manifest.json and its detached signature for the bundle's bytes.
    Sign {
        #[arg(long)]
        bundle: PathBuf,
        #[arg(long)]
        installer_url: String,
        #[arg(long)]
        portable_url: String,
        #[arg(long)]
        out: PathBuf,
        /// Environment variable holding the base64 Ed25519 seed.
        #[arg(long, default_value = "EXOSNAP_UPDATE_SIGNING_KEY")]
        key_env: String,
        /// Public key the product embeds, hex.
        #[arg(long)]
        public_key_hex: String,
    },
    /// Prove a signed manifest describes exactly these package files.
    Verify {
        #[arg(long)]
        manifest: PathBuf,
        #[arg(long)]
        signature: PathBuf,
        #[arg(long)]
        public_key_hex: String,
        #[arg(long)]
        version: String,
        #[arg(long)]
        installer: PathBuf,
        #[arg(long)]
        portable: PathBuf,
    },
}

#[derive(Subcommand)]
enum DocsCommand {
    Check {
        #[arg(long, default_value = ".")]
        repo_root: PathBuf,
    },
}

fn key_values(items: &[String]) -> Result<BTreeMap<String, String>> {
    items
        .iter()
        .map(|item| {
            item.split_once('=')
                .map(|(k, v)| (k.trim().to_string(), v.trim().to_string()))
                .with_context(|| format!("'{item}' is not key=value"))
        })
        .collect()
}

fn write_json(path: &Path, value: &impl serde::Serialize) -> Result<()> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let mut bytes = serde_json::to_vec_pretty(value)?;
    bytes.push(b'\n');
    std::fs::write(path, bytes).with_context(|| format!("write {}", path.display()))
}

fn profile_lanes(profile: &str) -> Result<Vec<Lane>> {
    Ok(match profile {
        "quick" => vec![Lane::Quick],
        "preflight" => vec![Lane::Preflight],
        "release-ci" => vec![Lane::CiCore, Lane::CiInstall, Lane::CiUpdate],
        "release-gpu" => vec![Lane::Gpu],
        "release-hardware" => vec![Lane::Hardware],
        "nightly" => vec![Lane::Nightly],
        other => bail!("unknown profile '{other}'"),
    })
}

fn run(args: RunArgs) -> Result<ExitCode> {
    let mut lanes = profile_lanes(&args.profile)?;
    if let Some(name) = &args.lane {
        let lane = Lane::parse(name).with_context(|| format!("unknown lane '{name}'"))?;
        ensure!(
            lanes.contains(&lane),
            "lane {name} is not part of profile {}",
            args.profile
        );
        lanes = vec![lane];
    }
    let release = lanes.iter().any(|l| l.is_release());
    let bundle = match &args.bundle {
        Some(path) => Some(bundle::open_any(path)?),
        None if release => bail!(
            "profile {} judges a CI-built candidate: pass --bundle",
            args.profile
        ),
        None => None,
    };
    if release && args.product.is_some() {
        bail!("release lanes judge the bundle's own bytes; --product is for development profiles");
    }
    let attested = args
        .attest
        .iter()
        .map(|a| {
            Capability::parse(a)
                .filter(|c| c.is_attested())
                .with_context(|| format!("'{a}' cannot be attested"))
        })
        .collect::<Result<Vec<_>>>()?;
    let caps = capability::probe(&attested);
    std::fs::create_dir_all(&args.out)?;
    let registry = scenarios::registry();
    let mut any_blocking = false;
    for lane in lanes {
        let mut ctx = context::Context::new(
            lane,
            caps.clone(),
            bundle.clone(),
            args.product.clone(),
            args.out.join(lane.name()),
        )?;
        ctx.keep_media = args.keep_media;
        println!(
            "== {} ({} scenarios) ==",
            lane.name(),
            runner::selected(
                &registry,
                &runner::Selection {
                    lane,
                    only: &args.only,
                    skip: &args.skip
                }
            )
            .len()
        );
        let result = runner::run_lane(
            &registry,
            &runner::Selection {
                lane,
                only: &args.only,
                skip: &args.skip,
            },
            &mut ctx,
            &args.profile,
        );
        let file = args.out.join(format!("{}.result.json", lane.name()));
        write_json(&file, &result)?;
        println!(
            "{}: PASS {} FAIL {} UNAVAILABLE {} INFRA_ERROR {} SKIPPED {} -> {}",
            lane.name(),
            result.count(model::Verdict::Pass),
            result.count(model::Verdict::Fail),
            result.count(model::Verdict::Unavailable),
            result.count(model::Verdict::InfraError),
            result.count(model::Verdict::Skipped),
            file.display()
        );
        any_blocking |=
            result.count(model::Verdict::Fail) > 0 || result.count(model::Verdict::InfraError) > 0;
    }
    // A lane's exit code is a convenience for local loops. Release readiness is
    // decided only by the merged report against the frozen plan.
    Ok(if any_blocking {
        ExitCode::from(1)
    } else {
        ExitCode::SUCCESS
    })
}

fn collect_results(inputs: &[PathBuf]) -> Result<Vec<LaneResult>> {
    let mut files = Vec::new();
    for input in inputs {
        if input.is_dir() {
            let mut stack = vec![input.clone()];
            while let Some(dir) = stack.pop() {
                for entry in std::fs::read_dir(&dir)?.flatten() {
                    let path = entry.path();
                    if path.is_dir() {
                        stack.push(path);
                    } else if path.to_string_lossy().ends_with(".result.json") {
                        files.push(path);
                    }
                }
            }
        } else {
            files.push(input.clone());
        }
    }
    files.sort();
    files
        .iter()
        .map(|f| {
            let result: LaneResult = serde_json::from_slice(&std::fs::read(f)?)
                .with_context(|| format!("parse {}", f.display()))?;
            ensure!(
                result.schema_version == model::RESULT_SCHEMA_VERSION,
                "{} has result schema {}, expected {}",
                f.display(),
                result.schema_version,
                model::RESULT_SCHEMA_VERSION
            );
            Ok(result)
        })
        .collect()
}

fn main() -> ExitCode {
    match real_main() {
        Ok(code) => code,
        Err(error) => {
            eprintln!("exo-verify: {error:#}");
            ExitCode::from(2)
        }
    }
}

fn real_main() -> Result<ExitCode> {
    let cli = Cli::parse();
    match cli.command {
        Command::Bundle(BundleCommand::Create {
            out,
            version,
            commit,
            candidate_id,
            installer,
            portable,
            runtimes,
            metadata,
            toolchain,
            inputs,
        }) => {
            let b = bundle::create(&bundle::CreateRequest {
                out_dir: out,
                product_version: version,
                source_commit: commit,
                candidate_id,
                installer,
                portable,
                runtimes,
                metadata,
                toolchain: key_values(&toolchain)?,
                inputs: key_values(&inputs)?,
            })?;
            println!("{}", b.sha256);
        }
        Command::Bundle(BundleCommand::Verify { path }) => {
            let b = bundle::open_any(&path)?;
            println!("{}", b.sha256);
        }
        Command::Bundle(BundleCommand::Pack { path, out }) => {
            let b = bundle::open(&path)?;
            bundle::pack(&b, &out)?;
            println!("{}", b.sha256);
        }
        Command::Bundle(BundleCommand::Unpack { archive, out }) => {
            let b = bundle::unpack(&archive, &out)?;
            println!("{}", b.sha256);
        }
        Command::Plan(PlanCommand::Create { bundle: path, out }) => {
            let b = bundle::open_any(&path)?;
            let registry = scenarios::registry();
            let scenarios = registry
                .iter()
                .filter(|s| s.lane.is_release())
                .map(|s| PlannedScenario {
                    id: s.id.into(),
                    scenario_revision: s.revision,
                    lane: s.lane.name().into(),
                    tier: s.tier,
                    title: s.title.into(),
                })
                .collect();
            let plan = ReleasePlan {
                schema: PLAN_SCHEMA.into(),
                product_version: b.inventory.product_version.clone(),
                bundle_sha256: b.sha256.clone(),
                candidate_id: b.inventory.candidate_id.clone(),
                source_commit: b.inventory.source_commit.clone(),
                created_at: model::now_rfc3339(),
                scenarios,
            };
            write_json(&out, &plan)?;
            println!(
                "{} scenarios planned for {}",
                plan.scenarios.len(),
                b.sha256
            );
        }
        Command::Plan(PlanCommand::Verify { bundle: path, plan }) => {
            let bundle = bundle::open_any(&path)?;
            let plan = ReleasePlan::load(&plan)?;
            plan.verify_against(&bundle, &scenarios::registry())?;
            println!("plan matches {} and the source registry", bundle.sha256);
        }
        Command::Run(args) => return run(args),
        Command::Report(ReportCommand::Merge {
            bundle,
            plan,
            decisions,
            results,
            out,
        }) => {
            let bundle = bundle::open_any(&bundle)?;
            let plan = ReleasePlan::load(&plan)?;
            plan.verify_against(&bundle, &scenarios::registry())?;
            let decisions = decisions
                .filter(|d| d.is_file())
                .map(|d| Decisions::load(&d))
                .transpose()?;
            let results = collect_results(&results)?;
            let report = report::merge(&plan, &results, decisions.as_ref())?;
            std::fs::create_dir_all(&out)?;
            write_json(&out.join("release-report.json"), &report)?;
            std::fs::write(out.join("release-report.md"), report::markdown(&report))?;
            std::fs::write(out.join("junit.xml"), report::junit(&report))?;
            println!(
                "{}",
                if report.ready_for_approval {
                    "READY FOR APPROVAL"
                } else {
                    "NOT READY"
                }
            );
            for item in &report.blocking {
                println!("  blocking: {item}");
            }
        }
        Command::Report(ReportCommand::Verify {
            bundle,
            plan,
            report,
            decisions,
            results,
        }) => {
            let bundle = bundle::open_any(&bundle)?;
            let plan = ReleasePlan::load(&plan)?;
            plan.verify_against(&bundle, &scenarios::registry())?;
            let decisions = decisions.map(|path| Decisions::load(&path)).transpose()?;
            let results = collect_results(&results)?;
            let stored: report::Report = serde_json::from_slice(&std::fs::read(&report)?)?;
            report::verify(&stored, &plan, &results, decisions.as_ref())?;
            println!("report matches candidate, plan, decisions and lane results");
            return Ok(if stored.ready_for_approval {
                ExitCode::SUCCESS
            } else {
                ExitCode::from(1)
            });
        }
        Command::Status {
            bundle,
            plan,
            report,
            decisions,
            results,
        } => {
            let bundle = bundle::open_any(&bundle)?;
            let plan = ReleasePlan::load(&plan)?;
            plan.verify_against(&bundle, &scenarios::registry())?;
            let decisions = decisions.map(|path| Decisions::load(&path)).transpose()?;
            let results = collect_results(&results)?;
            let stored: report::Report = serde_json::from_slice(&std::fs::read(&report)?)?;
            report::verify(&stored, &plan, &results, decisions.as_ref())?;
            print!("{}", report::markdown(&stored));
            return Ok(if stored.ready_for_approval {
                ExitCode::SUCCESS
            } else {
                ExitCode::from(1)
            });
        }
        Command::Accept(args) => {
            ensure!(
                bundle::is_sha256(&args.bundle_sha256),
                "--bundle-sha256 must be a lowercase SHA-256"
            );
            let mut decisions = if args.decisions.is_file() {
                Decisions::load(&args.decisions)?
            } else {
                Decisions::new(&args.bundle_sha256)
            };
            ensure!(
                decisions.bundle_sha256 == args.bundle_sha256,
                "{} holds decisions for bundle {}, not {}",
                args.decisions.display(),
                decisions.bundle_sha256,
                args.bundle_sha256
            );
            let known = scenarios::registry().iter().any(|s| s.id == args.scenario);
            ensure!(known, "unknown scenario '{}'", args.scenario);
            let by = args
                .by
                .or_else(|| std::env::var("GITHUB_ACTOR").ok())
                .or_else(|| std::env::var("USERNAME").ok())
                .unwrap_or_else(|| "unknown".into());
            decisions.record(Decision {
                scenario: args.scenario.clone(),
                decision: if args.risk {
                    DecisionKind::AcceptedRisk
                } else {
                    DecisionKind::Accepted
                },
                reason: args.reason,
                decided_by: by,
                decided_at: model::now_rfc3339(),
            })?;
            decisions.save(&args.decisions)?;
            println!(
                "{} recorded for {}. The original verdict stays in the report.",
                if args.risk {
                    "ACCEPTED_RISK"
                } else {
                    "ACCEPTED"
                },
                args.scenario
            );
        }
        Command::Manifest(ManifestCommand::Sign {
            bundle: path,
            installer_url,
            portable_url,
            out,
            key_env,
            public_key_hex,
        }) => {
            let b = bundle::open_any(&path)?;
            let key = manifest::signing_key_from_env(&key_env)?;
            manifest::check_key_pair(&key, &public_key_hex)?;
            let m = manifest::for_bundle(&b, &installer_url, &portable_url)?;
            let bytes = manifest::serialize(&m)?;
            let signature = manifest::sign(&bytes, &key);
            manifest::verify(
                &bytes,
                &signature,
                &manifest::public_key_from_hex(&public_key_hex)?,
            )?;
            std::fs::create_dir_all(&out)?;
            std::fs::write(out.join(manifest::MANIFEST_NAME), &bytes)?;
            std::fs::write(out.join(manifest::SIGNATURE_NAME), &signature)?;
            println!(
                "signed {} for {} with the embedded key's private half",
                manifest::MANIFEST_NAME,
                m.version
            );
        }
        Command::Manifest(ManifestCommand::Verify {
            manifest: m,
            signature,
            public_key_hex,
            version,
            installer,
            portable,
        }) => {
            let bytes = std::fs::read(&m)?;
            let sig = std::fs::read_to_string(&signature)?;
            manifest::verify_against_files(
                &bytes,
                &sig,
                &manifest::public_key_from_hex(&public_key_hex)?,
                &version,
                &installer,
                &portable,
            )?;
            println!("manifest signature and package hashes verified");
        }
        Command::Docs(DocsCommand::Check { repo_root }) => {
            let root = std::fs::canonicalize(&repo_root)?;
            let paths = docs::repository_paths(&root)?;
            let failures = docs::check(&root, &paths);
            for f in &failures {
                println!("{f}");
            }
            println!(
                "documentation: {} error(s), {} paths inspected",
                failures.len(),
                paths.len()
            );
            return Ok(if failures.is_empty() {
                ExitCode::SUCCESS
            } else {
                ExitCode::from(1)
            });
        }
        Command::Capabilities { attest } => {
            let attested: Vec<Capability> =
                attest.iter().filter_map(|a| Capability::parse(a)).collect();
            let caps = capability::probe(&attested);
            for c in Capability::ALL {
                println!(
                    "{:<28} {}",
                    c.name(),
                    if caps.has(c) { "yes" } else { "no" }
                );
            }
            println!("{}", serde_json::to_string_pretty(&caps.facts)?);
        }
        Command::List { lane } => {
            let lane = lane
                .map(|l| Lane::parse(&l).with_context(|| format!("unknown lane '{l}'")))
                .transpose()?;
            for s in scenarios::registry()
                .iter()
                .filter(|s| lane.is_none_or(|l| s.runs_in(l)))
            {
                let caps: Vec<&str> = s.requires.iter().map(|c| c.name()).collect();
                println!(
                    "{:<40} r{} {:<20} {:<12} {:?} [{}]",
                    s.id,
                    s.revision,
                    s.lane.name(),
                    format!("{:?}", s.tier),
                    s.also.iter().map(|l| l.name()).collect::<Vec<_>>(),
                    caps.join(",")
                );
            }
        }
        Command::Package(args) => {
            let result = package::run(&args)?;
            println!("{}", serde_json::to_string_pretty(&result)?);
        }
        Command::Stimulus(args) => stimulus::run(args)?,
        Command::Feed(args) => feed::serve_forever(args)?,
    }
    Ok(ExitCode::SUCCESS)
}
