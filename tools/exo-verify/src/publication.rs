//! Offline publication preconditions composed from the frozen candidate contracts.

use anyhow::{Context, Result, ensure};
use base64::Engine as _;
use clap::{Args, Subcommand};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::BTreeMap;
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};

use crate::bundle::{self, Bundle, FileRole};
use crate::model::LaneResult;
use crate::plan::{Decisions, ReleasePlan};
use crate::report::{self, Report};

const REPOSITORY: &str = "Exoridus/exosnap";

#[derive(Args)]
pub struct EvidenceArgs {
    #[arg(long)]
    bundle: PathBuf,
    #[arg(long)]
    plan: PathBuf,
    #[arg(long)]
    report: PathBuf,
    #[arg(long)]
    decisions: Option<PathBuf>,
    #[arg(long, required = true)]
    results: Vec<PathBuf>,
}

#[derive(Subcommand)]
pub enum PublicationCommand {
    /// Write an annotated-tag message binding qualified bytes to a frozen preparation run.
    TagMessage {
        #[command(flatten)]
        evidence: EvidenceArgs,
        #[arg(long)]
        run_metadata: PathBuf,
        #[arg(long)]
        preparation_run_metadata: PathBuf,
        #[arg(long)]
        out: PathBuf,
    },
    /// Read the admin-created annotated tag for workflow artifact selection.
    TagInput {
        #[arg(long)]
        tag_metadata: PathBuf,
        #[arg(long)]
        tag: String,
        #[arg(long)]
        source_commit: String,
        #[arg(long)]
        github_output: PathBuf,
    },
    /// Write the full evidence documents and the compact publication binding.
    Bundle {
        #[command(flatten)]
        evidence: EvidenceArgs,
        /// Successful official candidate run the evidence belongs to.
        #[arg(long)]
        candidate_run: u64,
        /// Directory receiving the evidence documents and the binding.
        #[arg(long)]
        out: PathBuf,
    },
    /// Decode the compact binding a workflow dispatch carried.
    Binding {
        /// Environment variable containing the base64 binding document.
        #[arg(long, default_value = "EXOSNAP_BINDING")]
        base64_env: String,
        #[arg(long)]
        out: PathBuf,
    },
    /// Fetch the retained evidence set named by a binding.
    Fetch {
        #[arg(long)]
        binding: PathBuf,
        /// Gist holding the retained evidence documents.
        #[arg(long)]
        gist: String,
        /// Gist revision the binding was frozen against.
        #[arg(long)]
        revision: String,
        #[arg(long)]
        out: PathBuf,
    },
    /// Verify the retained evidence set and write the qualification documents.
    Unpack {
        #[arg(long)]
        binding: PathBuf,
        /// Directory holding the fetched evidence documents.
        #[arg(long)]
        evidence: PathBuf,
        #[arg(long)]
        gist: Option<String>,
        #[arg(long)]
        revision: Option<String>,
        #[arg(long)]
        out: PathBuf,
    },
    /// Refuse unless the exact official candidate is qualified and publishable.
    Check {
        #[command(flatten)]
        evidence: EvidenceArgs,
        /// Compact publication binding the dispatched evidence belongs to.
        #[arg(long)]
        binding: PathBuf,
        /// GitHub Actions run metadata fetched by the workflow.
        #[arg(long)]
        run_metadata: PathBuf,
        #[arg(long)]
        candidate_id: String,
        #[arg(long)]
        source_commit: String,
        #[arg(long, requires_all = ["tag_metadata", "preparation_run_metadata"])]
        tag: Option<String>,
        #[arg(long, requires = "tag")]
        tag_metadata: Option<PathBuf>,
        #[arg(long, requires = "tag")]
        preparation_run_metadata: Option<PathBuf>,
        /// Fresh directory for exact package copies and hash sidecars.
        #[arg(long)]
        out: PathBuf,
        /// Optional GitHub step output file for the verified identity.
        #[arg(long)]
        github_output: Option<PathBuf>,
    },
    /// Rehash downloaded release assets and verify their manifest and tag.
    VerifyUploaded {
        #[arg(long)]
        bundle: PathBuf,
        #[arg(long)]
        assets: PathBuf,
        #[arg(long)]
        public_key_hex: String,
        #[arg(long)]
        tag_metadata: PathBuf,
        #[arg(long)]
        release_metadata: PathBuf,
        /// Require the release to be public rather than a staged draft.
        #[arg(long)]
        published: bool,
    },
}

/// One retained evidence document. `name` is the flat gist file name; the
/// digest is over the exact bytes that were frozen, so a gist revision or a
/// re-download cannot silently replace the qualification.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct EvidenceFile {
    name: String,
    sha256: String,
    size: u64,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    lane: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    attempt: Option<String>,
}

/// The compact binding a preparation dispatch carries. It names the candidate,
/// the bundle and the qualification documents by digest; the documents
/// themselves stay in the retained evidence set.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct PublicationBinding {
    schema: String,
    product_version: String,
    source_commit: String,
    candidate_run: u64,
    candidate_id: String,
    bundle_sha256: String,
    report_sha256: String,
    decisions_sha256: String,
    files: Vec<EvidenceFile>,
}

const BINDING_SCHEMA: &str = "exosnap.publication-binding/1";

/// The frozen qualification as loaded from local files: one report, the
/// decisions for the same bundle, and the evidence results the report covers.
struct Qualification {
    report: Report,
    decisions: Decisions,
    results: Vec<LaneResult>,
}

#[derive(Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct TagBinding {
    candidate_run: u64,
    preparation_run: u64,
    candidate_id: String,
    bundle_sha256: String,
}

fn validate_preparation(run: &Value, source: &str) -> Result<()> {
    ensure!(
        run["repository"]["full_name"] == REPOSITORY
            && run["path"] == ".github/workflows/publish-release.yml"
            && run["event"] == "workflow_dispatch"
            && run["head_branch"] == "next"
            && run["head_sha"] == source
            && run["status"] == "completed"
            && run["conclusion"] == "success",
        "qualification preparation provenance does not match a successful official run"
    );
    Ok(())
}

fn tag_binding(metadata: &Value, tag: &str, source: &str) -> Result<TagBinding> {
    ensure!(
        tag.strip_prefix('v').is_some_and(bundle::is_final_version),
        "release tag must be final vX.Y.Z"
    );
    ensure!(
        source.len() == 40 && source.bytes().all(|b| b.is_ascii_hexdigit()),
        "invalid tag source commit"
    );
    ensure!(
        metadata["tag"] == tag
            && metadata["object"]["type"] == "commit"
            && metadata["object"]["sha"] == source,
        "annotated release tag differs from qualified source/version"
    );
    let binding: TagBinding = serde_json::from_str(
        metadata["message"]
            .as_str()
            .context("tag lacks qualification binding")?,
    )?;
    ensure!(
        binding.candidate_run > 0
            && binding.preparation_run > 0
            && bundle::is_sha256(&binding.bundle_sha256),
        "invalid release tag binding"
    );
    ensure!(
        !binding.candidate_id.is_empty()
            && binding.candidate_id.len() <= 64
            && binding
                .candidate_id
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'.' | b'-')),
        "invalid candidate ID in release tag"
    );
    Ok(binding)
}

fn external_lane(lane: &str) -> bool {
    matches!(lane, "release-gpu" | "release-hardware")
}

fn load_binding(path: &Path) -> Result<PublicationBinding> {
    let binding: PublicationBinding = serde_json::from_slice(&fs::read(path)?)
        .with_context(|| format!("{} is not a publication binding", path.display()))?;
    ensure!(
        binding.schema == BINDING_SCHEMA,
        "unsupported publication binding schema '{}'",
        binding.schema
    );
    ensure!(
        !binding.product_version.is_empty() && !binding.candidate_id.is_empty(),
        "publication binding lacks a product version or candidate ID"
    );
    ensure!(
        binding.source_commit.len() == 40
            && binding.source_commit.bytes().all(|b| b.is_ascii_hexdigit()),
        "publication binding names no full source commit"
    );
    ensure!(
        binding.candidate_run > 0,
        "publication binding names no candidate run"
    );
    ensure!(
        bundle::is_sha256(&binding.bundle_sha256)
            && bundle::is_sha256(&binding.report_sha256)
            && bundle::is_sha256(&binding.decisions_sha256),
        "publication binding carries an invalid digest"
    );
    ensure!(
        !binding.files.is_empty(),
        "publication binding names no evidence documents"
    );
    let mut names = std::collections::BTreeSet::new();
    for file in &binding.files {
        ensure!(
            bundle::is_sha256(&file.sha256) && file.size > 0,
            "evidence document '{}' has no valid digest",
            file.name
        );
        ensure!(
            !file.name.is_empty()
                && !file.name.contains(['/', '\\'])
                && !file.name.contains("..")
                && file.name.bytes().all(|b| b.is_ascii_graphic()),
            "evidence document name '{}' is not a flat file name",
            file.name
        );
        ensure!(
            names.insert(&file.name),
            "evidence document '{}' is listed twice",
            file.name
        );
    }
    Ok(binding)
}

/// The flat gist name for one external lane result. Gists are flat, and the
/// name has to survive a round trip through a URL path segment.
fn evidence_result_name(index: usize, result: &LaneResult) -> String {
    let sanitize = |value: &str| {
        value
            .chars()
            .map(|c| {
                if c.is_ascii_alphanumeric() || matches!(c, '.' | '-') {
                    c
                } else {
                    '-'
                }
            })
            .collect::<String>()
    };
    format!(
        "result-{index:02}-{}-{}.result.json",
        sanitize(&result.lane),
        sanitize(&result.attempt)
    )
}

fn evidence_file(name: &str, bytes: &[u8]) -> EvidenceFile {
    EvidenceFile {
        name: name.to_string(),
        sha256: bundle::sha256_bytes(bytes),
        size: bytes.len() as u64,
        lane: None,
        attempt: None,
    }
}

fn validate_binding(
    binding: &PublicationBinding,
    bundle: &Bundle,
    source_commit: &str,
    candidate_id: &str,
) -> Result<()> {
    ensure!(
        binding.product_version == bundle.inventory.product_version
            && binding.source_commit == source_commit
            && binding.source_commit == bundle.inventory.source_commit
            && binding.candidate_id == candidate_id
            && binding.bundle_sha256 == bundle.sha256,
        "publication binding names a different candidate, source, version or bundle"
    );
    Ok(())
}

/// What one retained evidence download proved.
#[derive(Debug, Serialize)]
#[serde(rename_all = "camelCase")]
struct EvidenceLocator {
    gist: String,
    revision: String,
    files: Vec<EvidenceFile>,
}

/// Reads a URL and returns its bytes. Injected so the fetch path can be
/// exercised against scripted documents instead of the network.
pub trait EvidenceFetcher {
    fn get(&self, url: &str) -> Result<Vec<u8>>;
}

/// The production fetcher: `curl` follows the redirect a gist raw URL answers
/// with. Nothing is written anywhere until the bytes match the binding.
pub struct CurlEvidence;

impl EvidenceFetcher for CurlEvidence {
    fn get(&self, url: &str) -> Result<Vec<u8>> {
        let output = std::process::Command::new("curl")
            .args(["-sSfL", "--max-time", "60", url])
            .output()
            .context("could not run curl")?;
        ensure!(output.status.success(), "curl {url} failed");
        Ok(output.stdout)
    }
}

fn gist_raw_urls(index: &[u8], gist: &str, revision: &str) -> Result<BTreeMap<String, String>> {
    let document: Value = serde_json::from_slice(index)
        .context("the gist revision endpoint did not answer with JSON")?;
    ensure!(
        document["id"] == gist,
        "the gist revision belongs to a different gist"
    );
    let pinned = document["history"]
        .as_array()
        .and_then(|history| history.first())
        .and_then(|entry| entry["version"].as_str());
    ensure!(
        pinned == Some(revision),
        "the gist did not answer at the requested revision"
    );
    let files = document["files"]
        .as_object()
        .context("the gist revision lists no files")?;
    let mut urls = BTreeMap::new();
    for (name, entry) in files {
        let raw = entry["raw_url"]
            .as_str()
            .context("gist file has no raw URL")?;
        urls.insert(name.clone(), raw.to_string());
    }
    Ok(urls)
}

fn fetch_with_fetcher(
    binding: &PublicationBinding,
    gist: &str,
    revision: &str,
    out: &Path,
    fetcher: &dyn EvidenceFetcher,
) -> Result<()> {
    ensure!(
        !gist.is_empty() && !revision.is_empty(),
        "the retained evidence gist and revision are required"
    );
    ensure!(
        revision.len() == 40 && revision.bytes().all(|b| b.is_ascii_hexdigit()),
        "the retained evidence revision is not a full gist revision"
    );
    ensure!(!out.exists(), "evidence output directory must be fresh");
    let index = fetcher.get(&format!("https://api.github.com/gists/{gist}/{revision}"))?;
    let urls = gist_raw_urls(&index, gist, revision)?;
    fs::create_dir_all(out)?;
    for file in &binding.files {
        let url = urls.get(&file.name).with_context(|| {
            format!(
                "the retained evidence set is missing the frozen document '{}'",
                file.name
            )
        })?;
        let bytes = fetcher.get(url)?;
        ensure!(
            bytes.len() as u64 == file.size && bundle::sha256_bytes(&bytes) == file.sha256,
            "retained evidence document '{}' differs from the frozen reference",
            file.name
        );
        fs::write(out.join(&file.name), &bytes)?;
    }
    Ok(())
}

fn validate_run(run: &Value, source: &str) -> Result<()> {
    ensure!(
        run["repository"]["full_name"] == REPOSITORY,
        "wrong candidate repository"
    );
    ensure!(
        run["path"] == ".github/workflows/release-candidate-next.yml",
        "wrong candidate workflow"
    );
    ensure!(
        run["head_branch"] == "next",
        "candidate workflow was not run on next"
    );
    ensure!(
        run["head_sha"] == source,
        "candidate workflow/source revision mismatch"
    );
    ensure!(
        run["status"] == "completed" && run["conclusion"] == "success",
        "candidate workflow is not successful"
    );
    Ok(())
}

fn validate_qualification(
    qualification: &Qualification,
    plan: &ReleasePlan,
    results: &[LaneResult],
) -> Result<()> {
    report::verify(
        &qualification.report,
        plan,
        results,
        Some(&qualification.decisions),
    )?;
    ensure!(
        qualification.report.ready_for_approval,
        "candidate qualification is not ready"
    );
    Ok(())
}

fn load_evidence(args: &EvidenceArgs) -> Result<(Bundle, Qualification)> {
    let bundle = bundle::open_any(&args.bundle)?;
    let plan = ReleasePlan::load(&args.plan)?;
    plan.verify_against(&bundle, &crate::scenarios::registry())?;
    let results = crate::collect_results(&args.results)?;
    let qualification = Qualification {
        report: serde_json::from_slice(&fs::read(&args.report)?)?,
        decisions: args
            .decisions
            .as_ref()
            .map(|p| Decisions::load(p))
            .transpose()?
            .unwrap_or_else(|| Decisions::new(&bundle.sha256)),
        results,
    };
    validate_qualification(&qualification, &plan, &qualification.results)?;
    bundle.require(FileRole::Installer)?;
    bundle.require(FileRole::Portable)?;
    Ok((bundle, qualification))
}

/// The release package roles present in the bundle, in the order the release
/// page lists them. Setup is optional until the Burn toolchain produces one.
fn package_roles(bundle: &Bundle) -> Vec<FileRole> {
    [FileRole::Installer, FileRole::Setup, FileRole::Portable]
        .into_iter()
        .filter(|role| bundle.inventory.file(*role).is_some())
        .collect()
}

fn verify_packages(bundle: &Bundle, assets: &Path) -> Result<()> {
    for role in package_roles(bundle) {
        let file = bundle
            .inventory
            .file(role)
            .context("bundle lacks required release package")?;
        let original = bundle.require(role)?;
        let name = original.file_name().context("package has no filename")?;
        let (hash, size) = bundle::sha256_file(&assets.join(name))?;
        ensure!(
            hash == file.sha256 && size == file.size,
            "release asset differs from qualified bytes: {}",
            name.to_string_lossy()
        );
    }
    Ok(())
}

fn sidecar_name(filename: &str, role: FileRole) -> String {
    if role == FileRole::Portable {
        format!("{}.sha256", filename.trim_end_matches(".zip"))
    } else {
        format!("{filename}.sha256")
    }
}

fn verify_sidecars(bundle: &Bundle, assets: &Path) -> Result<()> {
    for role in package_roles(bundle) {
        let file = bundle
            .inventory
            .file(role)
            .context("missing release package")?;
        let package = bundle.require(role)?;
        let name = package
            .file_name()
            .and_then(|n| n.to_str())
            .context("invalid package name")?;
        let expected = format!("{}  {name}\n", file.sha256);
        ensure!(
            fs::read_to_string(assets.join(sidecar_name(name, role)))? == expected,
            "published hash sidecar differs from qualified package"
        );
    }
    Ok(())
}

pub fn run(command: PublicationCommand) -> Result<()> {
    match command {
        PublicationCommand::TagMessage {
            evidence,
            run_metadata,
            preparation_run_metadata,
            out,
        } => {
            let (bundle, _) = load_evidence(&evidence)?;
            let candidate: Value = serde_json::from_slice(&fs::read(run_metadata)?)?;
            let preparation: Value = serde_json::from_slice(&fs::read(preparation_run_metadata)?)?;
            validate_run(&candidate, &bundle.inventory.source_commit)?;
            validate_preparation(&preparation, &bundle.inventory.source_commit)?;
            crate::write_json(
                &out,
                &TagBinding {
                    candidate_run: candidate["id"]
                        .as_u64()
                        .filter(|id| *id > 0)
                        .context("candidate run has no ID")?,
                    preparation_run: preparation["id"]
                        .as_u64()
                        .filter(|id| *id > 0)
                        .context("preparation run has no ID")?,
                    candidate_id: bundle.inventory.candidate_id,
                    bundle_sha256: bundle.sha256,
                },
            )?;
        }
        PublicationCommand::TagInput {
            tag_metadata,
            tag,
            source_commit,
            github_output,
        } => {
            let metadata: Value = serde_json::from_slice(&fs::read(tag_metadata)?)?;
            let binding = tag_binding(&metadata, &tag, &source_commit)?;
            let mut output = fs::OpenOptions::new()
                .create(true)
                .append(true)
                .open(github_output)?;
            writeln!(
                output,
                "candidate_run={}\npreparation_run={}\ncandidate_id={}\nsource={}",
                binding.candidate_run, binding.preparation_run, binding.candidate_id, source_commit
            )?;
        }
        PublicationCommand::Bundle {
            evidence,
            candidate_run,
            out,
        } => {
            ensure!(candidate_run > 0, "candidate run must be a positive run ID");
            ensure!(!out.exists(), "bundle output directory must be fresh");
            let (bundle, mut qualification) = load_evidence(&evidence)?;
            qualification.results.retain(|r| external_lane(&r.lane));
            ensure!(
                !qualification.results.is_empty(),
                "no external lane evidence to retain: the qualification is incomplete"
            );
            let report_bytes = serde_json::to_vec_pretty(&qualification.report)?;
            let decisions_bytes = serde_json::to_vec_pretty(&qualification.decisions)?;
            fs::create_dir_all(&out)?;
            fs::write(out.join("release-report.json"), &report_bytes)?;
            fs::write(out.join("decisions.json"), &decisions_bytes)?;
            let mut files = vec![
                evidence_file("release-report.json", &report_bytes),
                evidence_file("decisions.json", &decisions_bytes),
            ];
            for (index, result) in qualification.results.iter().enumerate() {
                let name = evidence_result_name(index, result);
                let bytes = serde_json::to_vec_pretty(result)?;
                fs::write(out.join(&name), &bytes)?;
                files.push(EvidenceFile {
                    name,
                    sha256: bundle::sha256_bytes(&bytes),
                    size: bytes.len() as u64,
                    lane: Some(result.lane.clone()),
                    attempt: Some(result.attempt.clone()),
                });
            }
            let binding = PublicationBinding {
                schema: BINDING_SCHEMA.into(),
                product_version: bundle.inventory.product_version.clone(),
                source_commit: bundle.inventory.source_commit.clone(),
                candidate_run,
                candidate_id: bundle.inventory.candidate_id.clone(),
                bundle_sha256: bundle.sha256.clone(),
                report_sha256: bundle::sha256_bytes(&report_bytes),
                decisions_sha256: bundle::sha256_bytes(&decisions_bytes),
                files,
            };
            crate::write_json(&out.join("publication-binding.json"), &binding)?;
            let binding_bytes = serde_json::to_vec(&binding)?;
            let encoded = base64::engine::general_purpose::STANDARD.encode(&binding_bytes);
            ensure!(
                encoded.len() <= 60_000,
                "the compact publication binding does not fit a workflow dispatch input"
            );
            fs::write(out.join("publication-binding.base64"), &encoded)?;
            println!(
                "retained evidence set: {} document(s) for candidate {} ({}), binding {} bytes base64",
                binding.files.len(),
                binding.candidate_id,
                binding.product_version,
                encoded.len()
            );
            println!(
                "upload {} and every file next to it to a secret gist, then dispatch with that binding",
                out.join("publication-binding.json").display()
            );
        }
        PublicationCommand::Binding { base64_env, out } => {
            ensure!(!out.exists(), "binding output file must not exist");
            let encoded = std::env::var(&base64_env)
                .with_context(|| format!("{base64_env} is not set in the environment"))?;
            ensure!(
                encoded.len() <= 60_000,
                "the compact publication binding is larger than a dispatch input should carry"
            );
            let bytes = base64::engine::general_purpose::STANDARD.decode(encoded.trim())?;
            let binding: PublicationBinding = serde_json::from_slice(&bytes)
                .context("the dispatched binding is not a publication binding")?;
            fs::write(&out, &bytes)?;
            load_binding(&out)?;
            println!(
                "publication binding: {} {} ({} evidence documents)",
                binding.product_version,
                binding.candidate_id,
                binding.files.len()
            );
        }
        PublicationCommand::Fetch {
            binding,
            gist,
            revision,
            out,
        } => {
            let binding = load_binding(&binding)?;
            fetch_with_fetcher(&binding, &gist, &revision, &out, &CurlEvidence)?;
            println!(
                "retained evidence set at gist revision {} verified ({} documents)",
                revision,
                binding.files.len()
            );
        }
        PublicationCommand::Unpack {
            binding,
            evidence,
            gist,
            revision,
            out,
        } => {
            ensure!(!out.exists(), "qualification directory must be fresh");
            ensure!(
                gist.is_some() == revision.is_some(),
                "the evidence gist and revision are recorded together or not at all"
            );
            let binding = load_binding(&binding)?;
            let mut report_bytes = None;
            let mut decisions_bytes = None;
            let mut results = Vec::new();
            for file in &binding.files {
                let path = evidence.join(&file.name);
                let bytes = fs::read(&path).with_context(|| {
                    format!(
                        "the retained evidence set is missing the frozen document '{}'",
                        file.name
                    )
                })?;
                ensure!(
                    bytes.len() as u64 == file.size && bundle::sha256_bytes(&bytes) == file.sha256,
                    "retained evidence document '{}' differs from its frozen reference",
                    file.name
                );
                match file.name.as_str() {
                    "release-report.json" => report_bytes = Some(bytes),
                    "decisions.json" => decisions_bytes = Some(bytes),
                    _ => {
                        let result: LaneResult = serde_json::from_slice(&bytes)
                            .with_context(|| format!("'{}' is not a lane result", file.name))?;
                        ensure!(
                            external_lane(&result.lane),
                            "external qualification cannot replace hosted lanes"
                        );
                        results.push(result);
                    }
                }
            }
            let report_bytes =
                report_bytes.context("the evidence set carries no release report")?;
            ensure!(
                bundle::sha256_bytes(&report_bytes) == binding.report_sha256,
                "retained qualification report differs from the frozen qualification identity"
            );
            let decisions_bytes =
                decisions_bytes.context("the evidence set carries no decisions document")?;
            ensure!(
                bundle::sha256_bytes(&decisions_bytes) == binding.decisions_sha256,
                "retained decisions differ from the frozen qualification identity"
            );
            let stored: Report = serde_json::from_slice(&report_bytes)?;
            fs::create_dir_all(out.join("results"))?;
            fs::write(out.join("release-report.json"), &report_bytes)?;
            fs::write(out.join("release-report.md"), report::markdown(&stored))?;
            fs::write(out.join("decisions.json"), &decisions_bytes)?;
            Decisions::load(&out.join("decisions.json"))?;
            for (index, result) in results.iter().enumerate() {
                crate::write_json(
                    &out.join("results").join(format!("{index}.result.json")),
                    result,
                )?;
            }
            if let (Some(gist), Some(revision)) = (gist, revision) {
                crate::write_json(
                    &out.join("evidence-locator.json"),
                    &EvidenceLocator {
                        gist,
                        revision,
                        files: binding.files.clone(),
                    },
                )?;
            }
        }
        PublicationCommand::Check {
            evidence,
            binding,
            run_metadata,
            candidate_id,
            source_commit,
            tag,
            tag_metadata,
            preparation_run_metadata,
            out,
            github_output,
        } => {
            let (bundle, _) = load_evidence(&evidence)?;
            ensure!(
                bundle.inventory.source_commit == source_commit
                    && bundle.inventory.candidate_id == candidate_id,
                "bundle differs from selected source/candidate"
            );
            let binding = load_binding(&binding)?;
            validate_binding(&binding, &bundle, &source_commit, &candidate_id)?;
            let report_bytes = fs::read(&evidence.report)?;
            ensure!(
                bundle::sha256_bytes(&report_bytes) == binding.report_sha256,
                "the release report differs from the frozen qualification identity"
            );
            if let Some(decisions_path) = &evidence.decisions {
                let decisions_bytes = fs::read(decisions_path)?;
                ensure!(
                    bundle::sha256_bytes(&decisions_bytes) == binding.decisions_sha256,
                    "the decisions differ from the frozen qualification identity"
                );
            }
            let metadata: Value = serde_json::from_slice(&fs::read(run_metadata)?)?;
            validate_run(&metadata, &source_commit)?;
            ensure!(
                metadata["id"].as_u64() == Some(binding.candidate_run),
                "candidate run metadata differs from the publication binding"
            );
            if let Some(tag) = tag {
                ensure!(
                    tag == format!("v{}", bundle.inventory.product_version),
                    "tag version differs from qualified candidate"
                );
                let tag_record: Value = serde_json::from_slice(&fs::read(
                    tag_metadata.context("tag metadata is required")?,
                )?)?;
                let binding = tag_binding(&tag_record, &tag, &source_commit)?;
                let preparation: Value = serde_json::from_slice(&fs::read(
                    preparation_run_metadata.context("preparation provenance is required")?,
                )?)?;
                validate_preparation(&preparation, &source_commit)?;
                ensure!(
                    binding.bundle_sha256 == bundle.sha256
                        && binding.candidate_id == candidate_id
                        && metadata["id"].as_u64() == Some(binding.candidate_run)
                        && preparation["id"].as_u64() == Some(binding.preparation_run),
                    "tag names different candidate bytes or qualification runs"
                );
            }
            ensure!(!out.exists(), "publication output directory must be fresh");
            fs::create_dir_all(&out)?;
            for role in package_roles(&bundle) {
                let original = bundle.require(role)?;
                let name = original.file_name().context("package has no filename")?;
                fs::copy(&original, out.join(name))?;
                let filename = name.to_str().context("package filename is not UTF-8")?;
                let sidecar = sidecar_name(filename, role);
                fs::write(
                    out.join(sidecar),
                    format!("{}  {filename}\n", bundle::sha256_file(&original)?.0),
                )?;
            }
            verify_packages(&bundle, &out)?;
            verify_sidecars(&bundle, &out)?;
            if let Some(output) = github_output {
                let mut file = fs::OpenOptions::new()
                    .append(true)
                    .create(true)
                    .open(output)?;
                writeln!(
                    file,
                    "version={}\ntag=v{}\nsource={}\ninstaller={}\nportable={}",
                    bundle.inventory.product_version,
                    bundle.inventory.product_version,
                    source_commit,
                    bundle::installer_name(&bundle.inventory.product_version),
                    bundle::portable_name(&bundle.inventory.product_version)
                )?;
            }
            println!(
                "QUALIFIED FOR PUBLICATION: {} source {} bundle {}",
                bundle.inventory.product_version, source_commit, bundle.sha256
            );
        }
        PublicationCommand::VerifyUploaded {
            bundle: bundle_path,
            assets,
            public_key_hex,
            tag_metadata,
            release_metadata,
            published,
        } => {
            let bundle = bundle::open_any(&bundle_path)?;
            verify_packages(&bundle, &assets)?;
            verify_sidecars(&bundle, &assets)?;
            let tag = format!("v{}", bundle.inventory.product_version);
            let tag_record: Value = serde_json::from_slice(&fs::read(tag_metadata)?)?;
            let binding = tag_binding(&tag_record, &tag, &bundle.inventory.source_commit)?;
            ensure!(
                binding.bundle_sha256 == bundle.sha256
                    && binding.candidate_id == bundle.inventory.candidate_id,
                "release tag differs from qualified candidate"
            );
            let release: Value = serde_json::from_slice(&fs::read(release_metadata)?)?;
            ensure!(
                release["tag_name"] == tag && release["prerelease"] == false,
                "wrong release identity"
            );
            if published {
                ensure!(release["draft"] == false, "release is still a draft");
            }
            let manifest = crate::manifest::verify_against_files(
                &fs::read(assets.join(crate::manifest::MANIFEST_NAME))?,
                &fs::read_to_string(assets.join(crate::manifest::SIGNATURE_NAME))?,
                &crate::manifest::public_key_from_hex(&public_key_hex)?,
                &bundle.inventory.product_version,
                &assets.join(bundle::installer_name(&bundle.inventory.product_version)),
                &assets.join(bundle::portable_name(&bundle.inventory.product_version)),
            )?;
            let base = format!("https://github.com/{REPOSITORY}/releases/download/{tag}");
            ensure!(
                manifest
                    == crate::manifest::for_bundle(
                        &bundle,
                        &format!(
                            "{base}/{}",
                            bundle::installer_name(&bundle.inventory.product_version)
                        ),
                        &format!(
                            "{base}/{}",
                            bundle::portable_name(&bundle.inventory.product_version)
                        )
                    )?,
                "manifest URLs or package inventory differ from the official release"
            );
            println!("Release assets, signature and source match the qualified candidate");
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::plan::{PLAN_SCHEMA, PlannedScenario, Tier};
    use serde_json::json;

    #[test]
    fn admin_tag_binds_source_final_version_and_frozen_run() {
        let binding = TagBinding {
            candidate_run: 10,
            preparation_run: 20,
            candidate_id: "v010-final".into(),
            bundle_sha256: "b".repeat(64),
        };
        let metadata = json!({"tag": "v0.10.0", "object": {"type": "commit", "sha": "a".repeat(40)}, "message": serde_json::to_string(&binding).unwrap()});
        assert_eq!(
            tag_binding(&metadata, "v0.10.0", &"a".repeat(40))
                .unwrap()
                .preparation_run,
            20
        );
        assert!(tag_binding(&metadata, "v0.10.1", &"a".repeat(40)).is_err());
        assert!(tag_binding(&metadata, "v0.10.0", &"c".repeat(40)).is_err());
        assert!(tag_binding(&metadata, "v0.10.0-rc1", &"a".repeat(40)).is_err());
        assert!(
            tag_binding(
                &json!({"ref": "refs/tags/v0.10.0"}),
                "v0.10.0",
                &"a".repeat(40)
            )
            .is_err()
        );
        let mut other = metadata;
        other["message"] = json!("unbound release");
        assert!(tag_binding(&other, "v0.10.0", &"a".repeat(40)).is_err());
    }

    #[test]
    fn preparation_must_be_successful_and_from_the_same_next_source() {
        let original = json!({"path": ".github/workflows/publish-release.yml", "event": "workflow_dispatch", "head_branch": "next", "head_sha": "a".repeat(40), "status": "completed", "conclusion": "success", "repository": {"full_name": REPOSITORY}});
        validate_preparation(&original, &"a".repeat(40)).unwrap();
        assert!(validate_preparation(&original, &"b".repeat(40)).is_err());
        for (field, value) in [
            ("conclusion", "failure"),
            ("event", "push"),
            ("head_branch", "feature"),
            ("path", "other.yml"),
        ] {
            let mut other = original.clone();
            other[field] = json!(value);
            assert!(validate_preparation(&other, &"a".repeat(40)).is_err());
        }
    }

    #[test]
    fn binding_stays_a_dispatch_input_and_binds_every_document_digest() {
        let dir = tempfile::tempdir().unwrap();
        let fixture = evidence_fixture(dir.path());
        let binding = load_binding(&fixture.binding).unwrap();
        let encoded =
            base64::engine::general_purpose::STANDARD.encode(serde_json::to_vec(&binding).unwrap());
        assert!(
            encoded.len() <= 20_000,
            "the compact binding must stay small even with every evidence document listed"
        );
        assert!(
            binding.files.len() >= 3,
            "the report, decisions and external results are all retained"
        );
        assert!(
            binding
                .files
                .iter()
                .any(|f| f.lane.as_deref() == Some("release-gpu")),
            "the retained set carries the external lane documents"
        );
        let report = fs::read(&fixture.args.report).unwrap();
        assert_eq!(bundle::sha256_bytes(&report), binding.report_sha256);
    }

    #[test]
    fn a_binding_that_does_not_name_the_candidate_or_qualification_is_refused() {
        let dir = tempfile::tempdir().unwrap();
        let fixture = evidence_fixture(dir.path());
        let (bundle, _) = load_evidence(&fixture.args).unwrap();
        let run_path = dir.path().join("run.json");
        crate::write_json(&run_path, &json!({"id": 10, "path": ".github/workflows/release-candidate-next.yml", "head_branch": "next", "head_sha": bundle.inventory.source_commit, "status": "completed", "conclusion": "success", "repository": {"full_name": REPOSITORY}})).unwrap();
        let check = |binding: &Path, out: &str| {
            run(PublicationCommand::Check {
                evidence: EvidenceArgs {
                    bundle: fixture.args.bundle.clone(),
                    plan: fixture.args.plan.clone(),
                    report: fixture.args.report.clone(),
                    decisions: None,
                    results: fixture.args.results.clone(),
                },
                binding: binding.to_path_buf(),
                run_metadata: run_path.clone(),
                candidate_id: bundle.inventory.candidate_id.clone(),
                source_commit: bundle.inventory.source_commit.clone(),
                tag: None,
                tag_metadata: None,
                preparation_run_metadata: None,
                out: dir.path().join(out),
                github_output: None,
            })
        };
        check(&fixture.binding, "matching").unwrap();

        let original: Value = serde_json::from_slice(&fs::read(&fixture.binding).unwrap()).unwrap();
        let mutations = [
            ("candidateRun", json!(999), "candidate run metadata differs"),
            ("candidateId", json!("other"), "different candidate"),
            ("sourceCommit", json!("0".repeat(40)), "different candidate"),
            ("bundleSha256", json!("0".repeat(64)), "different candidate"),
            (
                "reportSha256",
                json!("0".repeat(64)),
                "frozen qualification identity",
            ),
        ];
        for (index, (field, value, expected)) in mutations.into_iter().enumerate() {
            let mut forged = original.clone();
            forged[field] = value;
            let path = dir.path().join(format!("forged-{index}.json"));
            crate::write_json(&path, &forged).unwrap();
            let error = check(&path, &format!("forged-out-{index}"))
                .unwrap_err()
                .to_string();
            assert!(error.contains(expected), "{field}: {error}");
        }
    }

    #[test]
    fn unpack_refuses_missing_altered_and_misreferenced_evidence() {
        let dir = tempfile::tempdir().unwrap();
        let fixture = evidence_fixture(dir.path());
        let bundle_dir = fixture.binding.parent().unwrap().to_path_buf();
        let report_name = load_binding(&fixture.binding)
            .unwrap()
            .files
            .iter()
            .find(|f| f.name == "release-report.json")
            .unwrap()
            .name
            .clone();

        let mut altered = fs::read(bundle_dir.join(&report_name)).unwrap();
        altered.push(b'\n');
        fs::write(bundle_dir.join(&report_name), altered).unwrap();
        let error = run(PublicationCommand::Unpack {
            binding: fixture.binding.clone(),
            evidence: bundle_dir.clone(),
            gist: None,
            revision: None,
            out: dir.path().join("out-altered"),
        })
        .unwrap_err()
        .to_string();
        assert!(
            error.contains("differs from its frozen reference"),
            "{error}"
        );

        fs::remove_file(bundle_dir.join(&report_name)).unwrap();
        let error = run(PublicationCommand::Unpack {
            binding: fixture.binding.clone(),
            evidence: bundle_dir.clone(),
            gist: None,
            revision: None,
            out: dir.path().join("out-missing"),
        })
        .unwrap_err()
        .to_string();
        assert!(error.contains("missing the frozen document"), "{error}");
    }

    #[test]
    fn fetch_refuses_a_missing_asset_or_a_different_gist_revision() {
        struct FakeGist {
            revision: String,
            files: BTreeMap<String, Vec<u8>>,
        }
        impl EvidenceFetcher for FakeGist {
            fn get(&self, url: &str) -> Result<Vec<u8>> {
                if url.starts_with("https://api.github.com/gists/") {
                    let files = self
                        .files
                        .keys()
                        .map(|name| {
                            (
                                name.clone(),
                                json!({"raw_url": format!("https://gist.example/{name}")}),
                            )
                        })
                        .collect::<serde_json::Map<_, _>>();
                    return Ok(serde_json::to_vec(&json!({
                        "id": "abcdef",
                        "history": [{"version": self.revision}],
                        "files": files,
                    }))?);
                }
                let name = url.rsplit('/').next().unwrap_or_default();
                self.files
                    .get(name)
                    .cloned()
                    .ok_or_else(|| anyhow::anyhow!("no such evidence file"))
            }
        }

        let dir = tempfile::tempdir().unwrap();
        let fixture = evidence_fixture(dir.path());
        let binding = load_binding(&fixture.binding).unwrap();
        let bundle_dir = fixture.binding.parent().unwrap();
        let mut files = BTreeMap::new();
        for file in &binding.files {
            files.insert(
                file.name.clone(),
                fs::read(bundle_dir.join(&file.name)).unwrap(),
            );
        }
        let revision = "c".repeat(40);
        let fetcher = FakeGist {
            revision: revision.clone(),
            files: files.clone(),
        };
        fetch_with_fetcher(
            &binding,
            "abcdef",
            &revision,
            &dir.path().join("fetched"),
            &fetcher,
        )
        .unwrap();

        let mut missing = files.clone();
        let dropped = binding.files[0].name.clone();
        missing.remove(&dropped);
        let error = fetch_with_fetcher(
            &binding,
            "abcdef",
            &revision,
            &dir.path().join("fetched-missing"),
            &FakeGist {
                revision: revision.clone(),
                files: missing,
            },
        )
        .unwrap_err()
        .to_string();
        assert!(
            error.contains(&format!("missing the frozen document '{dropped}'")),
            "{error}"
        );

        let mut altered = files;
        let first = binding.files[0].name.clone();
        altered.get_mut(&first).unwrap().push(b'\n');
        let error = fetch_with_fetcher(
            &binding,
            "abcdef",
            &revision,
            &dir.path().join("fetched-altered"),
            &FakeGist {
                revision: revision.clone(),
                files: altered,
            },
        )
        .unwrap_err()
        .to_string();
        assert!(
            error.contains("differs from the frozen reference"),
            "{error}"
        );

        let error = fetch_with_fetcher(
            &binding,
            "abcdef",
            &"d".repeat(40),
            &dir.path().join("fetched-revision"),
            &FakeGist {
                revision,
                files: BTreeMap::new(),
            },
        )
        .unwrap_err()
        .to_string();
        assert!(error.contains("revision"), "{error}");
    }

    #[test]
    fn the_preparation_workflow_reads_a_draft_release_through_a_draft_visible_query() {
        let path = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../.github/workflows/publish-release.yml");
        let text = fs::read_to_string(&path).unwrap();
        let start = text
            .find("Obtain draft release identity")
            .expect("the publication workflow has no draft identity step");
        let step = &text[start..(start + 900).min(text.len())];
        assert!(
            step.contains("releases?per_page=") && step.contains("select(.tag_name"),
            "draft release metadata must come from a query that can see drafts: {step}"
        );
        assert!(
            !step.contains("releases/tags/"),
            "the per-tag endpoint hides drafts: {step}"
        );
    }

    #[test]
    fn only_successful_official_next_run_can_supply_candidate() {
        let original = json!({"path": ".github/workflows/release-candidate-next.yml", "head_branch": "next", "head_sha": "a".repeat(40), "status": "completed", "conclusion": "success", "repository": {"full_name": REPOSITORY}});
        assert!(validate_run(&original, &"a".repeat(40)).is_ok());
        assert!(validate_run(&original, &"b".repeat(40)).is_err());
        for (field, value) in [
            ("conclusion", "failure"),
            ("status", "in_progress"),
            ("head_branch", "feature"),
            ("path", "other.yml"),
        ] {
            let mut run = original.clone();
            run[field] = json!(value);
            assert!(validate_run(&run, &"a".repeat(40)).is_err());
        }
    }

    #[test]
    fn external_results_cannot_replace_hosted_lanes() {
        assert!(external_lane("release-gpu"));
        assert!(external_lane("release-hardware"));
        for lane in ["release-ci-core", "release-ci-install", "release-ci-update"] {
            assert!(!external_lane(lane));
        }
    }

    #[test]
    fn publication_refuses_unready_forged_and_misbound_report() {
        let mut plan = ReleasePlan {
            schema: PLAN_SCHEMA.into(),
            product_version: "0.10.0".into(),
            candidate_id: "c".into(),
            source_commit: "a".repeat(40),
            bundle_sha256: "b".repeat(64),
            created_at: "t".into(),
            scenarios: vec![PlannedScenario {
                id: "required".into(),
                scenario_revision: 1,
                lane: "release-gpu".into(),
                tier: Tier::Required,
                title: "required".into(),
            }],
        };
        let decisions = Decisions::new(&plan.bundle_sha256);
        let mut qualification = Qualification {
            report: report::merge(&plan, &[], Some(&decisions)).unwrap(),
            decisions,
            results: vec![],
        };
        assert!(validate_qualification(&qualification, &plan, &[]).is_err());
        qualification.report.ready_for_approval = true;
        assert!(validate_qualification(&qualification, &plan, &[]).is_err());
        plan.scenarios.clear();
        qualification.report = report::merge(&plan, &[], Some(&qualification.decisions)).unwrap();
        assert!(validate_qualification(&qualification, &plan, &[]).is_ok());
        for field in ["source", "candidate", "bundle"] {
            let mut other = plan.clone();
            match field {
                "source" => other.source_commit = "c".repeat(40),
                "candidate" => other.candidate_id = "other".into(),
                _ => other.bundle_sha256 = "c".repeat(64),
            }
            assert!(validate_qualification(&qualification, &other, &[]).is_err());
        }
    }

    #[test]
    fn published_files_must_exist_and_match_qualified_hashes() {
        let dir = tempfile::tempdir().unwrap();
        let bundle = bundle::tests::fixture(dir.path(), "0.10.0");
        let assets = dir.path().join("assets");
        fs::create_dir(&assets).unwrap();
        assert!(verify_packages(&bundle, &assets).is_err());
        for role in [FileRole::Installer, FileRole::Portable] {
            let file = bundle.require(role).unwrap();
            fs::copy(&file, assets.join(file.file_name().unwrap())).unwrap();
        }
        verify_packages(&bundle, &assets).unwrap();
        fs::write(
            assets.join(bundle::portable_name("0.10.0")),
            b"changed public bytes",
        )
        .unwrap();
        assert!(verify_packages(&bundle, &assets).is_err());
        fs::write(
            bundle.require(FileRole::Installer).unwrap(),
            b"changed candidate bytes",
        )
        .unwrap();
        assert!(bundle::open(&bundle.root).is_err());
    }

    struct Fixture {
        args: EvidenceArgs,
        binding: PathBuf,
    }

    fn evidence_fixture(dir: &Path) -> Fixture {
        use crate::model::{Identity, RESULT_SCHEMA_VERSION, ScenarioResult, Verdict};
        use std::collections::BTreeMap;
        let bundle = bundle::tests::fixture(dir, "0.10.0");
        let scenarios = crate::scenarios::registry();
        let plan = ReleasePlan {
            schema: PLAN_SCHEMA.into(),
            product_version: bundle.inventory.product_version.clone(),
            candidate_id: bundle.inventory.candidate_id.clone(),
            source_commit: bundle.inventory.source_commit.clone(),
            bundle_sha256: bundle.sha256.clone(),
            created_at: "t".into(),
            scenarios: scenarios
                .iter()
                .filter(|s| s.lane.is_release())
                .map(|s| PlannedScenario {
                    id: s.id.into(),
                    scenario_revision: s.revision,
                    lane: s.lane.name().into(),
                    tier: s.tier,
                    title: s.title.into(),
                })
                .collect(),
        };
        let mut lanes = BTreeMap::<String, LaneResult>::new();
        for scenario in &plan.scenarios {
            lanes
                .entry(scenario.lane.clone())
                .or_insert_with(|| LaneResult {
                    schema_version: RESULT_SCHEMA_VERSION,
                    lane: scenario.lane.clone(),
                    profile: "fixture".into(),
                    runner_version: "test".into(),
                    started_at: "s".into(),
                    finished_at: "f".into(),
                    attempt: "test:1".into(),
                    identity: Identity {
                        bundle_sha256: Some(bundle.sha256.clone()),
                        ..Default::default()
                    },
                    environment: BTreeMap::new(),
                    tools: BTreeMap::new(),
                    capabilities: vec![],
                    slot: None,
                    backend: None,
                    scenarios: vec![],
                })
                .scenarios
                .push(ScenarioResult {
                    id: scenario.id.clone(),
                    scenario_revision: scenario.scenario_revision,
                    verdict: Verdict::Pass,
                    detail: "fixture measurement".into(),
                    duration_ms: 1,
                    missing_capabilities: vec![],
                    evidence: BTreeMap::new(),
                    artifacts: vec![],
                });
        }
        let results: Vec<_> = lanes.into_values().collect();
        let report = report::merge(&plan, &results, None).unwrap();
        let args = EvidenceArgs {
            bundle: bundle.root.clone(),
            plan: dir.join("plan.json"),
            report: dir.join("report.json"),
            decisions: None,
            results: vec![dir.join("results")],
        };
        fs::create_dir(&args.results[0]).unwrap();
        crate::write_json(&args.plan, &plan).unwrap();
        crate::write_json(&args.report, &report).unwrap();
        for (index, result) in results.iter().enumerate() {
            crate::write_json(
                &args.results[0].join(format!("{index}.result.json")),
                result,
            )
            .unwrap();
        }
        let bundle_dir = dir.join("publication");
        run(PublicationCommand::Bundle {
            evidence: EvidenceArgs {
                bundle: args.bundle.clone(),
                plan: args.plan.clone(),
                report: args.report.clone(),
                decisions: None,
                results: args.results.clone(),
            },
            candidate_run: 10,
            out: bundle_dir.clone(),
        })
        .unwrap();
        // The publication check reads the frozen evidence documents, exactly
        // as the workflow reads the unpacked copies, not the local originals.
        let mut args = args;
        args.report = bundle_dir.join("release-report.json");
        args.decisions = Some(bundle_dir.join("decisions.json"));
        Fixture {
            args,
            binding: bundle_dir.join("publication-binding.json"),
        }
    }

    #[test]
    fn complete_frozen_candidate_passes_and_mutations_are_refused() {
        let dir = tempfile::tempdir().unwrap();
        let Fixture { args, binding } = evidence_fixture(dir.path());
        let (bundle, _) = load_evidence(&args).unwrap();
        let run_path = dir.path().join("run.json");
        crate::write_json(&run_path, &json!({"id": 10, "path": ".github/workflows/release-candidate-next.yml", "head_branch": "next", "head_sha": bundle.inventory.source_commit, "status": "completed", "conclusion": "success", "repository": {"full_name": REPOSITORY}})).unwrap();
        let out = dir.path().join("assets");
        run(PublicationCommand::Check {
            evidence: args,
            binding,
            run_metadata: run_path,
            candidate_id: bundle.inventory.candidate_id.clone(),
            source_commit: bundle.inventory.source_commit.clone(),
            tag: None,
            tag_metadata: None,
            preparation_run_metadata: None,
            out: out.clone(),
            github_output: None,
        })
        .unwrap();
        verify_packages(&bundle, &out).unwrap();
        let key = ed25519_dalek::SigningKey::from_bytes(&[7; 32]);
        let public_key = hex::encode(key.verifying_key().as_bytes());
        let base = "https://github.com/Exoridus/exosnap/releases/download/v0.10.0";
        let manifest = crate::manifest::for_bundle(
            &bundle,
            &format!("{base}/{}", bundle::installer_name("0.10.0")),
            &format!("{base}/{}", bundle::portable_name("0.10.0")),
        )
        .unwrap();
        let manifest_bytes = crate::manifest::serialize(&manifest).unwrap();
        fs::write(out.join(crate::manifest::MANIFEST_NAME), &manifest_bytes).unwrap();
        fs::write(
            out.join(crate::manifest::SIGNATURE_NAME),
            crate::manifest::sign(&manifest_bytes, &key),
        )
        .unwrap();
        let tag_path = dir.path().join("tag.json");
        crate::write_json(&tag_path, &json!({"tag": "v0.10.0", "object": {"type": "commit", "sha": bundle.inventory.source_commit},
            "message": serde_json::to_string(&TagBinding { candidate_run: 10, preparation_run: 20, candidate_id: bundle.inventory.candidate_id.clone(), bundle_sha256: bundle.sha256.clone() }).unwrap()})).unwrap();
        let release_path = dir.path().join("release.json");
        crate::write_json(
            &release_path,
            &json!({"tag_name": "v0.10.0", "draft": false, "prerelease": false}),
        )
        .unwrap();
        let verify_uploaded = || {
            run(PublicationCommand::VerifyUploaded {
                bundle: bundle.root.clone(),
                assets: out.clone(),
                public_key_hex: public_key.clone(),
                tag_metadata: tag_path.clone(),
                release_metadata: release_path.clone(),
                published: true,
            })
        };
        verify_uploaded().unwrap();
        crate::write_json(&tag_path, &json!({"ref": "refs/tags/v0.10.0", "object": {"type": "commit", "sha": "0".repeat(40)}})).unwrap();
        assert!(verify_uploaded().is_err());
        fs::write(
            out.join("ExoSnap-0.10.0-windows-x64.msi.sha256"),
            "wrong hash",
        )
        .unwrap();
        assert!(verify_sidecars(&bundle, &out).is_err());
        let plan_path = dir.path().join("plan.json");
        let mut plan = ReleasePlan::load(&plan_path).unwrap();
        plan.scenarios.pop();
        assert!(
            plan.verify_against(&bundle, &crate::scenarios::registry())
                .is_err()
        );
        let inventory_path = bundle.root.join(bundle::INVENTORY_NAME);
        let mut inventory = bundle.inventory.clone();
        inventory.files[0].sha256 = "0".repeat(64);
        crate::write_json(&inventory_path, &inventory).unwrap();
        assert!(bundle::open(&bundle.root).is_err());
    }

    #[test]
    fn tagged_publication_requires_the_exact_candidate_and_preparation() {
        let dir = tempfile::tempdir().unwrap();
        let Fixture { args, binding } = evidence_fixture(dir.path());
        let (bundle, _) = load_evidence(&args).unwrap();
        let candidate_path = dir.path().join("candidate-run.json");
        crate::write_json(&candidate_path, &json!({"id": 10, "path": ".github/workflows/release-candidate-next.yml", "head_branch": "next", "head_sha": bundle.inventory.source_commit, "status": "completed", "conclusion": "success", "repository": {"full_name": REPOSITORY}})).unwrap();
        let preparation_path = dir.path().join("preparation-run.json");
        crate::write_json(&preparation_path, &json!({"id": 20, "path": ".github/workflows/publish-release.yml", "event": "workflow_dispatch", "head_branch": "next", "head_sha": bundle.inventory.source_commit, "status": "completed", "conclusion": "success", "repository": {"full_name": REPOSITORY}})).unwrap();
        let evidence = || EvidenceArgs {
            bundle: args.bundle.clone(),
            plan: args.plan.clone(),
            report: args.report.clone(),
            decisions: None,
            results: args.results.clone(),
        };
        let message_path = dir.path().join("message.json");
        run(PublicationCommand::TagMessage {
            evidence: evidence(),
            run_metadata: candidate_path.clone(),
            preparation_run_metadata: preparation_path.clone(),
            out: message_path.clone(),
        })
        .unwrap();
        let mut tagged: TagBinding =
            serde_json::from_slice(&fs::read(message_path).unwrap()).unwrap();
        let tag_path = dir.path().join("tag.json");
        let check = |tagged: &TagBinding, out: &str| {
            crate::write_json(&tag_path, &json!({"tag": "v0.10.0", "object": {"type": "commit", "sha": bundle.inventory.source_commit}, "message": serde_json::to_string(tagged).unwrap()})).unwrap();
            run(PublicationCommand::Check {
                evidence: evidence(),
                binding: binding.clone(),
                run_metadata: candidate_path.clone(),
                candidate_id: bundle.inventory.candidate_id.clone(),
                source_commit: bundle.inventory.source_commit.clone(),
                tag: Some("v0.10.0".into()),
                tag_metadata: Some(tag_path.clone()),
                preparation_run_metadata: Some(preparation_path.clone()),
                out: dir.path().join(out),
                github_output: None,
            })
        };
        check(&tagged, "matching").unwrap();
        tagged.preparation_run += 1;
        assert!(
            check(&tagged, "wrong-run")
                .unwrap_err()
                .to_string()
                .contains("tag names different")
        );
        tagged.preparation_run -= 1;
        tagged.bundle_sha256 = "0".repeat(64);
        assert!(
            check(&tagged, "wrong-bundle")
                .unwrap_err()
                .to_string()
                .contains("tag names different")
        );
    }
}
