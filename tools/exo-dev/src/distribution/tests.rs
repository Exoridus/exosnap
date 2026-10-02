//! Distribution preparation tests: a verified release model renders every
//! channel, an unrehearsed package is never READY, and submission only ever
//! invokes the official programs with the frozen artifacts.

use std::cell::RefCell;
use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

use super::*;
use crate::test_support::packaging_fixture;

const VERSION: &str = "0.10.0";

struct FakeCommands {
    calls: RefCell<Vec<Vec<String>>>,
}

impl FakeCommands {
    fn new() -> FakeCommands {
        FakeCommands {
            calls: RefCell::new(Vec::new()),
        }
    }

    fn calls(&self) -> Vec<Vec<String>> {
        self.calls.borrow().clone()
    }
}

impl CommandRunner for FakeCommands {
    fn run(&self, program: &str, args: &[&str], env: &[(&str, String)]) -> Result<CommandOutput> {
        let mut call = vec![program.to_string()];
        call.extend(args.iter().map(|arg| arg.to_string()));
        call.extend(env.iter().map(|(name, _value)| format!("{name}=<secret>")));
        self.calls.borrow_mut().push(call);

        match (program, args.first().copied()) {
            ("choco", Some("pack")) => {
                let out = args
                    .windows(2)
                    .find(|pair| pair[0] == "--output-directory")
                    .map(|pair| PathBuf::from(pair[1]))
                    .context("choco pack without an output directory")?;
                fs::write(out.join(format!("exosnap.{VERSION}.nupkg")), b"nupkg bytes")?;
                Ok(CommandOutput {
                    status: 0,
                    stdout: "packed".into(),
                    stderr: String::new(),
                })
            }
            ("choco", Some("push")) => Ok(CommandOutput {
                status: 0,
                stdout: "pushed to the community feed".into(),
                stderr: String::new(),
            }),
            ("winget", Some("validate")) => Ok(CommandOutput {
                status: 0,
                stdout: "Manifest validation succeeded.".into(),
                stderr: String::new(),
            }),
            ("wingetcreate", Some("submit")) => Ok(CommandOutput {
                status: 0,
                stdout: "Pull request created: https://github.com/microsoft/winget-pkgs/pull/12345"
                    .into(),
                stderr: String::new(),
            }),
            ("gh", _) => {
                if args.contains(&"--method") && args.contains(&"PUT") {
                    Ok(CommandOutput {
                        status: 0,
                        stdout: serde_json::to_string(&serde_json::json!({
                            "commit": {"html_url": "https://github.com/Exoridus/scoop-exosnap/commit/abcdef"},
                        }))?,
                        stderr: String::new(),
                    })
                } else {
                    Ok(CommandOutput {
                        status: 0,
                        stdout: "deadbeef\n".into(),
                        stderr: String::new(),
                    })
                }
            }
            (other, _) => anyhow::bail!("unexpected program {other}"),
        }
    }
}

struct FakeMsi;

impl MsiIdentityReader for FakeMsi {
    fn read(&self, msi: &Path) -> Result<MsiIdentity> {
        ensure!(
            msi.file_name()
                .is_some_and(|name| name == "ExoSnap-0.10.0-windows-x64.msi"),
            "the identity reader must be handed the resolved MSI"
        );
        Ok(MsiIdentity {
            product_code: "{11111111-2222-3333-4444-555555555555}".into(),
            upgrade_code: "{8988DAFC-3AE4-4788-BA6D-62E3F73C7A7D}".into(),
        })
    }
}

struct FakeSecrets(BTreeMap<String, String>);

impl FakeSecrets {
    fn configured() -> FakeSecrets {
        FakeSecrets(BTreeMap::from([
            ("CHOCOLATEY_API_KEY".into(), "choco-secret".into()),
            ("WINGET_SUBMIT_TOKEN".into(), "winget-secret".into()),
            ("SCOOP_BUCKET_TOKEN".into(), "scoop-secret".into()),
        ]))
    }
}

impl SecretSource for FakeSecrets {
    fn get(&self, name: &str) -> Option<String> {
        self.0.get(name).cloned()
    }
}

struct Fixture {
    repo: tempfile::TempDir,
    _root: tempfile::TempDir,
    assets: PathBuf,
    release_json: PathBuf,
    prepared: PathBuf,
}

fn release_assets(root: &Path) -> (PathBuf, PathBuf) {
    let assets = root.join("assets");
    fs::create_dir_all(&assets).unwrap();
    let msi = b"final msi bytes".to_vec();
    let portable = b"final portable bytes".to_vec();
    let msi_sha = release::sha256_hex(&msi);
    let portable_sha = release::sha256_hex(&portable);
    fs::write(assets.join(release::msi_name(VERSION)), &msi).unwrap();
    fs::write(assets.join(release::portable_name(VERSION)), &portable).unwrap();
    fs::write(
        assets.join(format!("{}.sha256", release::msi_name(VERSION))),
        format!("{msi_sha}  {}\n", release::msi_name(VERSION)),
    )
    .unwrap();
    fs::write(
        assets.join(release::portable_sidecar_name(VERSION)),
        format!("{portable_sha}  {}\n", release::portable_name(VERSION)),
    )
    .unwrap();
    let release_json = root.join("release.json");
    let document = serde_json::json!({
        "id": 42,
        "tag_name": "v0.10.0",
        "draft": false,
        "prerelease": false,
        "html_url": "https://github.com/Exoridus/exosnap/releases/tag/v0.10.0",
        "published_at": "2026-10-02T16:28:12Z",
        "assets": [
            {"name": release::msi_name(VERSION), "size": msi.len(), "digest": format!("sha256:{msi_sha}"), "browser_download_url": "https://example.invalid/msi"},
            {"name": format!("{}.sha256", release::msi_name(VERSION)), "size": 1, "digest": format!("sha256:{}", release::sha256_hex(b"m")), "browser_download_url": "https://example.invalid/msi.sha256"},
            {"name": release::portable_name(VERSION), "size": portable.len(), "digest": format!("sha256:{portable_sha}"), "browser_download_url": "https://example.invalid/zip"},
            {"name": release::portable_sidecar_name(VERSION), "size": 1, "digest": format!("sha256:{}", release::sha256_hex(b"z")), "browser_download_url": "https://example.invalid/zip.sha256"},
        ]
    });
    fs::write(&release_json, serde_json::to_vec_pretty(&document).unwrap()).unwrap();
    (assets, release_json)
}

fn fixture() -> Fixture {
    let repo = packaging_fixture(VERSION);
    let root = tempfile::tempdir().unwrap();
    let (assets, release_json) = release_assets(root.path());
    let prepared = root.path().join("prepared");
    Fixture {
        repo,
        _root: root,
        assets,
        release_json,
        prepared,
    }
}

fn prepare_fixture(fixture: &Fixture, runner: &FakeCommands) -> PrepareOutcome {
    prepare(
        &PrepareRequest {
            repo_root: fixture.repo.path().to_path_buf(),
            version: VERSION.into(),
            source_commit: "a".repeat(40),
            release_json: fixture.release_json.clone(),
            assets: fixture.assets.clone(),
            out: fixture.prepared.clone(),
        },
        runner,
        &FakeMsi,
    )
    .unwrap()
}

fn rehearsal_result(fixture: &Fixture, path: &Path) -> PathBuf {
    let msi_sha = release::sha256_file(&fixture.assets.join(release::msi_name(VERSION)))
        .unwrap()
        .0;
    fs::write(
        path,
        serde_json::to_vec_pretty(&serde_json::json!({
            "ok": true,
            "msiSha256": msi_sha,
            "restoreRan": true,
            "steps": [{"name": "install", "ok": true}],
        }))
        .unwrap(),
    )
    .unwrap();
    path.to_path_buf()
}

#[test]
fn prepare_renders_validates_packs_and_is_not_ready_before_the_rehearsal() {
    let fixture = fixture();
    let runner = FakeCommands::new();
    let outcome = prepare_fixture(&fixture, &runner);

    assert_eq!(outcome.release.msi.sha256.len(), 64);
    assert!(
        !outcome.readiness.ready_for_distribution,
        "a package without its rehearsal is never READY"
    );
    assert_eq!(outcome.readiness.channels["chocolatey"].state, "NOT_READY");
    assert_eq!(outcome.readiness.channels["winget"].state, "READY");
    assert_eq!(outcome.readiness.channels["scoop"].state, "READY");
    assert!(fixture.prepared.join(RELEASE_MODEL_NAME).is_file());
    assert!(
        fixture
            .prepared
            .join(format!("packaging/chocolatey/exosnap.{VERSION}.nupkg"))
            .is_file()
    );

    let calls = runner.calls();
    assert!(calls.iter().any(|call| {
        call.first().map(String::as_str) == Some("choco")
            && call.get(1).map(String::as_str) == Some("pack")
    }));
    assert!(calls.iter().any(|call| {
        call.first().map(String::as_str) == Some("winget")
            && call.get(1).map(String::as_str) == Some("validate")
    }));
}

#[test]
fn validate_freezes_a_digest_that_detects_later_changes() {
    let fixture = fixture();
    let runner = FakeCommands::new();
    prepare_fixture(&fixture, &runner);
    let rehearsal = rehearsal_result(&fixture, &fixture.prepared.join("rehearsal.json"));

    let readiness = validate(&ValidateRequest {
        prepared: fixture.prepared.clone(),
        assets: fixture.assets.clone(),
        chocolatey_rehearsal: Some(rehearsal.clone()),
        expect_readiness_digest: None,
    })
    .unwrap();
    assert!(readiness.ready_for_distribution);
    let digest = ReadinessReport::frozen_digest(&fixture.prepared).unwrap();

    let again = validate(&ValidateRequest {
        prepared: fixture.prepared.clone(),
        assets: fixture.assets.clone(),
        chocolatey_rehearsal: Some(rehearsal),
        expect_readiness_digest: Some(digest.clone()),
    })
    .unwrap();
    assert!(again.ready_for_distribution);
    assert_eq!(
        ReadinessReport::frozen_digest(&fixture.prepared).unwrap(),
        digest
    );

    let nupkg = fixture
        .prepared
        .join(format!("packaging/chocolatey/exosnap.{VERSION}.nupkg"));
    fs::write(&nupkg, b"tampered").unwrap();
    assert!(
        validate(&ValidateRequest {
            prepared: fixture.prepared.clone(),
            assets: fixture.assets.clone(),
            chocolatey_rehearsal: None,
            expect_readiness_digest: None,
        })
        .is_err()
    );
}

#[test]
fn submit_invokes_only_the_official_programs_with_the_frozen_artifacts() {
    let fixture = fixture();
    let runner = FakeCommands::new();
    prepare_fixture(&fixture, &runner);
    let rehearsal = rehearsal_result(&fixture, &fixture.prepared.join("rehearsal.json"));
    validate(&ValidateRequest {
        prepared: fixture.prepared.clone(),
        assets: fixture.assets.clone(),
        chocolatey_rehearsal: Some(rehearsal),
        expect_readiness_digest: None,
    })
    .unwrap();
    let digest = ReadinessReport::frozen_digest(&fixture.prepared).unwrap();
    let secrets = FakeSecrets::configured();

    let chocolatey = submit(
        &SubmitRequest {
            prepared: fixture.prepared.clone(),
            channel: Channel::Chocolatey,
            out: fixture.prepared.with_extension("chocolatey.json"),
            expect_readiness_digest: Some(digest.clone()),
        },
        &runner,
        &secrets,
    )
    .unwrap();
    assert_eq!(chocolatey.status, "SUBMITTED");
    assert!(
        chocolatey
            .url
            .contains("community.chocolatey.org/packages/exosnap")
    );

    let winget = submit(
        &SubmitRequest {
            prepared: fixture.prepared.clone(),
            channel: Channel::Winget,
            out: fixture.prepared.with_extension("winget.json"),
            expect_readiness_digest: Some(digest.clone()),
        },
        &runner,
        &secrets,
    )
    .unwrap();
    assert_eq!(
        winget.url,
        "https://github.com/microsoft/winget-pkgs/pull/12345"
    );

    let scoop = submit(
        &SubmitRequest {
            prepared: fixture.prepared.clone(),
            channel: Channel::Scoop,
            out: fixture.prepared.with_extension("scoop.json"),
            expect_readiness_digest: Some(digest),
        },
        &runner,
        &secrets,
    )
    .unwrap();
    assert_eq!(scoop.status, "PUBLISHED");
    assert!(scoop.url.contains("scoop-exosnap/commit/"));

    let calls = runner.calls();
    assert!(calls.iter().any(|call| {
        call.first().map(String::as_str) == Some("choco")
            && call.get(1).map(String::as_str) == Some("push")
            && call.iter().any(|arg| arg == "--source")
    }));
    assert!(calls.iter().any(|call| {
        call.first().map(String::as_str) == Some("wingetcreate")
            && call.get(1).map(String::as_str) == Some("submit")
            && call
                .windows(2)
                .any(|pair| pair[0] == "--token" && pair[1] == "winget-secret")
    }));
    assert!(calls.iter().any(|call| {
        call.first().map(String::as_str) == Some("gh")
            && call.iter().any(|arg| arg == "--method")
            && call.iter().any(|arg| arg == "PUT")
            && call.iter().any(|arg| arg == "GH_TOKEN=<secret>")
    }));
    for (result, secret) in [
        (&chocolatey, "choco-secret"),
        (&winget, "winget-secret"),
        (&scoop, "scoop-secret"),
    ] {
        let text = serde_json::to_string(result).unwrap();
        assert!(
            !text.contains(secret),
            "a credential value must never appear in a recorded result"
        );
    }
}

#[test]
fn submit_refuses_an_unready_or_moved_preparation() {
    let fixture = fixture();
    let runner = FakeCommands::new();
    prepare_fixture(&fixture, &runner);
    let secrets = FakeSecrets::configured();
    let error = submit(
        &SubmitRequest {
            prepared: fixture.prepared.clone(),
            channel: Channel::Chocolatey,
            out: fixture.prepared.with_extension("chocolatey.json"),
            expect_readiness_digest: None,
        },
        &runner,
        &secrets,
    )
    .unwrap_err()
    .to_string();
    assert!(error.contains("not READY"), "{error}");
    assert!(
        !runner
            .calls()
            .iter()
            .any(|call| call.get(1).map(String::as_str) == Some("push")),
        "an unready preparation must not reach a package manager"
    );

    let rehearsal = rehearsal_result(&fixture, &fixture.prepared.join("rehearsal.json"));
    validate(&ValidateRequest {
        prepared: fixture.prepared.clone(),
        assets: fixture.assets.clone(),
        chocolatey_rehearsal: Some(rehearsal),
        expect_readiness_digest: None,
    })
    .unwrap();
    let error = submit(
        &SubmitRequest {
            prepared: fixture.prepared.clone(),
            channel: Channel::Chocolatey,
            out: fixture.prepared.with_extension("chocolatey.json"),
            expect_readiness_digest: Some("0".repeat(64)),
        },
        &runner,
        &secrets,
    )
    .unwrap_err()
    .to_string();
    assert!(error.contains("differs from the approved"), "{error}");
}

#[test]
fn the_distribution_workflow_prepares_without_secrets_and_publishes_only_behind_the_environment() {
    let path =
        Path::new(env!("CARGO_MANIFEST_DIR")).join("../../.github/workflows/distribution.yml");
    let text = fs::read_to_string(&path).unwrap();
    let prepare = text
        .split("\n  publish:\n")
        .next()
        .expect("the distribution workflow has no publish job");
    assert!(
        !prepare.contains("secrets."),
        "the preparation phase must not need publishing credentials"
    );
    assert!(
        text.contains("environment: distribution"),
        "publication must sit behind the protected distribution environment"
    );
    assert!(
        text.contains("if: inputs.publish && needs.prepare.outputs.ready == 'true'"),
        "publication must be an explicit, READY-gated opt-in"
    );
    assert!(text.contains("chocolatey-rehearsal.json"));
    assert!(text.contains("--expect-readiness-digest"));
}

#[test]
fn a_stale_tracked_version_is_refused_before_anything_is_rendered() {
    let repo = packaging_fixture("0.9.0");
    let root = tempfile::tempdir().unwrap();
    let (assets, release_json) = release_assets(root.path());
    let error = prepare(
        &PrepareRequest {
            repo_root: repo.path().to_path_buf(),
            version: VERSION.into(),
            source_commit: "a".repeat(40),
            release_json,
            assets,
            out: root.path().join("prepared"),
        },
        &FakeCommands::new(),
        &FakeMsi,
    )
    .unwrap_err()
    .to_string();
    assert!(error.contains("bump it"), "{error}");
}
