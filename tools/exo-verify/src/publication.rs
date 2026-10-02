//! Offline publication preconditions composed from the frozen candidate contracts.

use anyhow::{Context, Result, ensure};
use base64::Engine as _;
use clap::{Args, Subcommand};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::fs;
use std::io::{Cursor, Read, Write};
use std::path::{Path, PathBuf};

use crate::bundle::{self, Bundle, FileRole};
use crate::model::LaneResult;
use crate::plan::{Decisions, ReleasePlan};
use crate::report::{self, Report};

const REPOSITORY: &str = "Exoridus/exosnap";
const MAXIMUM_BYTES: u64 = 16 * 1024 * 1024;

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
    /// Encode reviewed external qualification for workflow dispatch.
    Encode {
        #[command(flatten)]
        evidence: EvidenceArgs,
        #[arg(long)]
        out: PathBuf,
    },
    /// Decode external results without allowing replacement of hosted lanes.
    Unpack {
        /// Environment variable containing the encoded qualification.
        #[arg(long, default_value = "EXOSNAP_QUALIFICATION")]
        encoded_env: String,
        #[arg(long)]
        sha256: String,
        #[arg(long)]
        out: PathBuf,
    },
    /// Refuse unless the exact official candidate is qualified and publishable.
    Check {
        #[command(flatten)]
        evidence: EvidenceArgs,
        /// GitHub Actions run metadata fetched by the workflow.
        #[arg(long)]
        run_metadata: PathBuf,
        #[arg(long)]
        candidate_id: String,
        #[arg(long)]
        source_commit: String,
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

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Qualification {
    report: Report,
    decisions: Decisions,
    results: Vec<LaneResult>,
}

fn external_lane(lane: &str) -> bool {
    matches!(lane, "release-gpu" | "release-hardware")
}

fn encode(bytes: &[u8]) -> Result<String> {
    let mut writer = zip::ZipWriter::new(Cursor::new(Vec::new()));
    writer.start_file(
        "qualification.json",
        zip::write::SimpleFileOptions::default()
            .compression_method(zip::CompressionMethod::Deflated),
    )?;
    writer.write_all(bytes)?;
    let encoded = base64::engine::general_purpose::STANDARD.encode(writer.finish()?.into_inner());
    ensure!(
        encoded.len() <= 60_000,
        "qualification exceeds workflow dispatch limit"
    );
    Ok(encoded)
}

fn decode(encoded: &str, sha256: &str, maximum: u64) -> Result<Vec<u8>> {
    ensure!(
        encoded.len() <= 60_000,
        "qualification exceeds dispatch limit"
    );
    ensure!(bundle::is_sha256(sha256), "invalid qualification SHA-256");
    let bytes = base64::engine::general_purpose::STANDARD.decode(encoded)?;
    let mut archive = zip::ZipArchive::new(Cursor::new(bytes))?;
    ensure!(
        archive.len() == 1,
        "qualification must contain exactly one document"
    );
    let entry = archive.by_index(0)?;
    ensure!(
        entry.name() == "qualification.json",
        "unexpected qualification entry"
    );
    let mut bytes = Vec::new();
    entry.take(maximum + 1).read_to_end(&mut bytes)?;
    ensure!(
        bytes.len() as u64 <= maximum,
        "qualification exceeds expansion limit"
    );
    ensure!(
        bundle::sha256_bytes(&bytes) == sha256,
        "qualification SHA-256 mismatch"
    );
    Ok(bytes)
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

fn verify_packages(bundle: &Bundle, assets: &Path) -> Result<()> {
    for role in [FileRole::Installer, FileRole::Portable] {
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
    for role in [FileRole::Installer, FileRole::Portable] {
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
        PublicationCommand::Encode { evidence, out } => {
            let (_, mut qualification) = load_evidence(&evidence)?;
            qualification.results.retain(|r| external_lane(&r.lane));
            let bytes = serde_json::to_vec(&qualification)?;
            crate::write_json(
                &out,
                &serde_json::json!({"qualification_sha256": bundle::sha256_bytes(&bytes), "qualification_base64": encode(&bytes)?}),
            )?;
        }
        PublicationCommand::Unpack {
            encoded_env,
            sha256,
            out,
        } => {
            ensure!(!out.exists(), "qualification directory must be fresh");
            let bytes = decode(&std::env::var(encoded_env)?, &sha256, MAXIMUM_BYTES)?;
            let qualification: Qualification = serde_json::from_slice(&bytes)?;
            ensure!(
                qualification.results.iter().all(|r| external_lane(&r.lane)),
                "external qualification cannot replace hosted lanes"
            );
            fs::create_dir_all(out.join("results"))?;
            crate::write_json(&out.join("release-report.json"), &qualification.report)?;
            fs::write(
                out.join("release-report.md"),
                report::markdown(&qualification.report),
            )?;
            qualification.decisions.save(&out.join("decisions.json"))?;
            Decisions::load(&out.join("decisions.json"))?;
            for (index, result) in qualification.results.iter().enumerate() {
                crate::write_json(
                    &out.join("results").join(format!("{index}.result.json")),
                    result,
                )?;
            }
        }
        PublicationCommand::Check {
            evidence,
            run_metadata,
            candidate_id,
            source_commit,
            out,
            github_output,
        } => {
            let (bundle, _) = load_evidence(&evidence)?;
            ensure!(
                bundle.inventory.source_commit == source_commit
                    && bundle.inventory.candidate_id == candidate_id,
                "bundle differs from selected source/candidate"
            );
            let metadata: Value = serde_json::from_slice(&fs::read(run_metadata)?)?;
            validate_run(&metadata, &source_commit)?;
            ensure!(!out.exists(), "publication output directory must be fresh");
            fs::create_dir_all(&out)?;
            for role in [FileRole::Installer, FileRole::Portable] {
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
            ensure!(
                tag_record["ref"] == format!("refs/tags/{tag}")
                    && tag_record["object"]["type"] == "commit"
                    && tag_record["object"]["sha"] == bundle.inventory.source_commit,
                "release tag differs from qualified source"
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
    fn qualification_transport_checks_hash_and_expansion_bound() {
        let bytes = br#"{"results":[]}"#;
        let encoded = encode(bytes).unwrap();
        let hash = bundle::sha256_bytes(bytes);
        assert_eq!(decode(&encoded, &hash, 1024).unwrap(), bytes);
        assert!(decode(&encoded, &"0".repeat(64), 1024).is_err());
        assert!(decode(&encoded, &hash, 2).is_err());
        assert!(decode("invalid", &hash, 1024).is_err());
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

    fn evidence_fixture(dir: &Path) -> EvidenceArgs {
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
        args
    }

    #[test]
    fn complete_frozen_candidate_passes_and_mutations_are_refused() {
        let dir = tempfile::tempdir().unwrap();
        let args = evidence_fixture(dir.path());
        let (bundle, _) = load_evidence(&args).unwrap();
        let run_path = dir.path().join("run.json");
        crate::write_json(&run_path, &json!({"path": ".github/workflows/release-candidate-next.yml", "head_branch": "next", "head_sha": bundle.inventory.source_commit, "status": "completed", "conclusion": "success", "repository": {"full_name": REPOSITORY}})).unwrap();
        let out = dir.path().join("assets");
        run(PublicationCommand::Check {
            evidence: args,
            run_metadata: run_path,
            candidate_id: bundle.inventory.candidate_id.clone(),
            source_commit: bundle.inventory.source_commit.clone(),
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
        crate::write_json(&tag_path, &json!({"ref": "refs/tags/v0.10.0", "object": {"type": "commit", "sha": bundle.inventory.source_commit}})).unwrap();
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
}
