//! Distribution preparation: resolving the immutable public release into one
//! typed model, rendering and validating every package channel, rehearsing
//! what can be rehearsed, freezing a readiness digest, and submitting exactly
//! the frozen artifacts through the official package-manager programs.
//!
//! This module owns ExoSnap's deterministic distribution policy. The workflow
//! supplies the release metadata and downloaded assets, keeps the package
//! manager credentials in its protected environment, and does not restate any
//! rule from here.

pub mod msi;
pub mod readiness;
pub mod release;
pub mod render;

use std::fs;
use std::path::{Path, PathBuf};

use anyhow::{Context as _, Result, ensure};
use serde::{Deserialize, Serialize};

pub use msi::{MsiIdentity, read_msi_identity};
pub use readiness::{
    ChannelReadiness, READINESS_MARKDOWN, READINESS_NAME, READINESS_SCHEMA, READINESS_SIDECAR,
    ReadinessReport,
};
pub use release::{DistributionRelease, PackageAsset, is_sha256, sha256_file};

pub const RELEASE_MODEL_NAME: &str = "distribution-release.json";

/// Runs one external program. Injected so every invocation a submission makes
/// can be asserted without a package manager installed.
pub struct CommandOutput {
    pub status: i32,
    pub stdout: String,
    pub stderr: String,
}

/// One argument of an external command. A secret argument is handed to the
/// real process value for value but appears as `***` in every recorded or
/// printed form, so a credential cannot reach a transcript, an error message
/// or a submission record.
pub enum Arg<'a> {
    Plain(&'a str),
    Secret(&'a str),
}

impl<'a> Arg<'a> {
    /// The value the process must receive. Never use this for display.
    fn expose(&self) -> &'a str {
        match self {
            Self::Plain(value) | Self::Secret(value) => value,
        }
    }

    /// The value safe to record or print.
    fn redacted(&self) -> &'a str {
        match self {
            Self::Plain(value) => value,
            Self::Secret(_) => "***",
        }
    }
}

/// Replaces a credential's exact value anywhere it could have been echoed by
/// an external program, so captured output can never carry a secret.
fn redact(text: &str, secret: &str) -> String {
    if secret.is_empty() {
        text.to_string()
    } else {
        text.replace(secret, "***")
    }
}

/// A command line with every secret argument replaced, safe for an error
/// message. Used when the process could not even be started.
fn call_description(program: &str, args: &[Arg<'_>]) -> String {
    let mut text = program.to_string();
    for arg in args {
        text.push(' ');
        text.push_str(arg.redacted());
    }
    text
}

pub trait CommandRunner {
    fn run(&self, program: &str, args: &[Arg<'_>], env: &[(&str, String)])
    -> Result<CommandOutput>;
}

pub struct RealCommands;

impl CommandRunner for RealCommands {
    fn run(
        &self,
        program: &str,
        args: &[Arg<'_>],
        env: &[(&str, String)],
    ) -> Result<CommandOutput> {
        let mut command = crate::process::command(program);
        command.args(args.iter().map(Arg::expose));
        for (name, value) in env {
            command.env(name, value);
        }
        let output = command
            .output()
            .with_context(|| format!("could not run {}", call_description(program, args)))?;
        Ok(CommandOutput {
            status: output.status.code().unwrap_or(-1),
            stdout: String::from_utf8_lossy(&output.stdout).into_owned(),
            stderr: String::from_utf8_lossy(&output.stderr).into_owned(),
        })
    }
}

/// Reads installer identity from a built MSI. Injected so the preparation
/// policy can be exercised without a real MSI database.
pub trait MsiIdentityReader {
    fn read(&self, msi: &Path) -> Result<MsiIdentity>;
}

pub struct SystemMsi;

impl MsiIdentityReader for SystemMsi {
    fn read(&self, msi: &Path) -> Result<MsiIdentity> {
        read_msi_identity(msi)
    }
}

/// Reads a credential by name from the process environment.
pub trait SecretSource {
    fn get(&self, name: &str) -> Option<String>;
}

pub struct ProcessSecrets;

impl SecretSource for ProcessSecrets {
    fn get(&self, name: &str) -> Option<String> {
        std::env::var(name)
            .ok()
            .filter(|value| !value.trim().is_empty())
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Channel {
    Chocolatey,
    Winget,
    Scoop,
}

impl Channel {
    pub fn name(self) -> &'static str {
        match self {
            Channel::Chocolatey => "chocolatey",
            Channel::Winget => "winget",
            Channel::Scoop => "scoop",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SubmissionResult {
    pub schema: String,
    pub channel: String,
    pub version: String,
    pub status: String,
    pub url: String,
    pub detail: String,
}

pub struct PrepareRequest {
    pub repo_root: PathBuf,
    pub version: String,
    pub source_commit: String,
    pub release_json: PathBuf,
    pub assets: PathBuf,
    pub out: PathBuf,
}

#[derive(Debug)]
pub struct PrepareOutcome {
    pub release: DistributionRelease,
    pub readiness: ReadinessReport,
    pub readiness_digest: String,
}

pub struct ValidateRequest {
    pub prepared: PathBuf,
    pub assets: PathBuf,
    pub chocolatey_rehearsal: Option<PathBuf>,
    pub expect_readiness_digest: Option<String>,
}

pub struct SubmitRequest {
    pub prepared: PathBuf,
    pub channel: Channel,
    pub out: PathBuf,
    pub expect_readiness_digest: Option<String>,
}

fn nupkg_name(version: &str) -> String {
    format!("exosnap.{version}.nupkg")
}

fn load_release_model(prepared: &Path) -> Result<DistributionRelease> {
    let path = prepared.join(RELEASE_MODEL_NAME);
    serde_json::from_slice(
        &fs::read(&path).with_context(|| format!("could not read {}", path.display()))?,
    )
    .with_context(|| format!("{} is not a distribution release model", path.display()))
}

fn verify_downloaded(release: &DistributionRelease, assets: &Path) -> Result<()> {
    for asset in [&release.msi, &release.portable] {
        let (digest, size) = sha256_file(&assets.join(&asset.filename))?;
        ensure!(
            digest == asset.sha256 && size == asset.size,
            "the downloaded '{}' no longer matches the resolved release",
            asset.filename
        );
    }
    Ok(())
}

fn check(readiness: &mut ReadinessReport, channel: &str, name: &str, passed: bool) {
    readiness
        .channels
        .entry(channel.to_string())
        .or_default()
        .checks
        .insert(name.to_string(), passed);
}

fn full_validation(prepared: &Path, version: &str) -> Result<()> {
    let chocolatey = crate::packaging::validate_chocolatey(
        prepared,
        version,
        crate::packaging::ChocolateyMode::default(),
    )?;
    ensure!(
        chocolatey.ok(),
        "prepared Chocolatey package failed validation:\n{}",
        crate::packaging::render(&chocolatey)
    );
    let winget = crate::packaging::validate_winget(prepared, version)?;
    ensure!(
        winget.ok(),
        "prepared WinGet manifests failed validation:\n{}",
        crate::packaging::render(&winget)
    );
    let scoop = crate::packaging::validate_scoop(prepared, version)?;
    ensure!(
        scoop.ok(),
        "prepared Scoop manifest failed validation:\n{}",
        crate::packaging::render(&scoop)
    );
    Ok(())
}

/// The digest of every prepared manifest file, so the frozen readiness record
/// covers the exact bytes each channel will publish.
fn manifest_digest(prepared: &Path, version: &str) -> Result<String> {
    let mut parts = Vec::new();
    for relative in [
        format!("packaging/winget/manifests/c/Codexo/ExoSnap/{version}/Codexo.ExoSnap.yaml"),
        format!(
            "packaging/winget/manifests/c/Codexo/ExoSnap/{version}/Codexo.ExoSnap.installer.yaml"
        ),
        format!(
            "packaging/winget/manifests/c/Codexo/ExoSnap/{version}/Codexo.ExoSnap.locale.en-US.yaml"
        ),
        "packaging/scoop/exosnap.json".to_string(),
    ] {
        let bytes = fs::read(prepared.join(&relative))
            .with_context(|| format!("prepared manifest '{relative}' is missing"))?;
        parts.push(format!("{relative}\n{}", release::sha256_hex(&bytes)));
    }
    Ok(release::sha256_hex(parts.join("\n").as_bytes()))
}

fn channel_stub(version: &str) -> ChannelReadiness {
    ChannelReadiness {
        version: version.to_string(),
        ..Default::default()
    }
}

pub fn prepare(
    request: &PrepareRequest,
    runner: &dyn CommandRunner,
    msi_reader: &dyn MsiIdentityReader,
) -> Result<PrepareOutcome> {
    ensure!(
        !request.out.exists(),
        "prepared output directory must be fresh"
    );
    let tracked_version = crate::packaging::cmake_project_version(&request.repo_root)?;
    ensure!(
        tracked_version == request.version,
        "the tracked packaging tree declares {tracked_version}; bump it to {} before preparing distribution",
        request.version
    );
    let release = DistributionRelease::load(
        &request.version,
        &request.source_commit,
        &request.release_json,
        &request.assets,
    )?;
    let identity = msi_reader
        .read(&release.msi_path(&request.assets))
        .context("the published MSI must carry a readable ProductCode and UpgradeCode")?;
    ensure!(
        identity.product_code.starts_with('{') && identity.product_code.ends_with('}'),
        "the MSI ProductCode '{}' is not a braced GUID",
        identity.product_code
    );

    crate::distribution::render::render_chocolatey(&request.repo_root, &request.out, &release)?;
    let winget_dir = crate::distribution::render::render_winget(
        &request.repo_root,
        &request.out,
        &release,
        &identity,
    )?;
    crate::distribution::render::render_scoop(&request.repo_root, &request.out, &release)?;
    full_validation(&request.out, &request.version)?;

    let mut readiness = ReadinessReport::new(&release);
    readiness
        .channels
        .insert("chocolatey".into(), channel_stub(&request.version));
    readiness
        .channels
        .insert("winget".into(), channel_stub(&request.version));
    readiness
        .channels
        .insert("scoop".into(), channel_stub(&request.version));
    check(&mut readiness, "chocolatey", "packageValidation", true);
    check(&mut readiness, "chocolatey", "rehearsal", false);
    check(&mut readiness, "winget", "manifestValidation", true);
    check(&mut readiness, "scoop", "manifestValidation", true);
    readiness.channels.get_mut("winget").unwrap().product_code =
        Some(identity.product_code.clone());
    readiness
        .channels
        .get_mut("winget")
        .unwrap()
        .manifest_sha256 = Some(manifest_digest(&request.out, &request.version)?);

    let nuspec = request
        .out
        .join("packaging/chocolatey")
        .join("exosnap.nuspec");
    let nuspec_arg = nuspec.to_string_lossy().into_owned();
    let chocolatey_dir = request
        .out
        .join("packaging/chocolatey")
        .to_string_lossy()
        .into_owned();
    let pack = runner.run(
        "choco",
        &[
            Arg::Plain("pack"),
            Arg::Plain(&nuspec_arg),
            Arg::Plain("--output-directory"),
            Arg::Plain(&chocolatey_dir),
        ],
        &[],
    )?;
    ensure!(
        pack.status == 0,
        "choco pack failed with {}:\n{}{}",
        pack.status,
        pack.stdout,
        pack.stderr
    );
    let nupkg = request
        .out
        .join("packaging/chocolatey")
        .join(nupkg_name(&request.version));
    let (nupkg_sha, _) = sha256_file(&nupkg)
        .with_context(|| format!("choco pack produced no {}", nupkg.display()))?;
    check(&mut readiness, "chocolatey", "pack", true);
    readiness
        .channels
        .get_mut("chocolatey")
        .unwrap()
        .package_filename = Some(nupkg_name(&request.version));
    readiness
        .channels
        .get_mut("chocolatey")
        .unwrap()
        .package_sha256 = Some(nupkg_sha);

    let winget_dir_arg = winget_dir.to_string_lossy().into_owned();
    let winget_validate = runner.run(
        "winget",
        &[
            Arg::Plain("validate"),
            Arg::Plain("--manifest"),
            Arg::Plain(&winget_dir_arg),
        ],
        &[],
    )?;
    ensure!(
        winget_validate.status == 0,
        "winget validate failed with {}:\n{}{}",
        winget_validate.status,
        winget_validate.stdout,
        winget_validate.stderr
    );
    check(&mut readiness, "winget", "wingetValidate", true);

    for channel in readiness.channels.values_mut() {
        channel.msi_url = Some(release.msi.url.clone());
        channel.msi_sha256 = Some(release.msi.sha256.clone());
    }
    readiness.channels.get_mut("scoop").unwrap().zip_url = Some(release.portable.url.clone());
    readiness.channels.get_mut("scoop").unwrap().zip_sha256 = Some(release.portable.sha256.clone());
    readiness.finalize();

    let model_bytes = serde_json::to_vec_pretty(&release)?;
    fs::write(request.out.join(RELEASE_MODEL_NAME), &model_bytes)?;
    let readiness_digest = readiness.write(&request.out)?;
    Ok(PrepareOutcome {
        release,
        readiness,
        readiness_digest,
    })
}

pub struct StatusRequest {
    pub repo_root: PathBuf,
    pub version: String,
    pub source_commit: String,
    pub release_json: PathBuf,
    pub assets: PathBuf,
    pub prepared: Option<PathBuf>,
}

/// Resolves the immutable public release and reports the declared channel
/// states, plus a prepared readiness report when one exists. Read-only.
pub fn status(request: &StatusRequest) -> Result<String> {
    let release = DistributionRelease::load(
        &request.version,
        &request.source_commit,
        &request.release_json,
        &request.assets,
    )?;
    let mut out = format!(
        "ExoSnap {} from {} (release {}, published {})\n",
        release.version, release.tag, release.release_id, release.published_at
    );
    out.push_str(&format!(
        "  MSI      {}  {}\n",
        release.msi.filename, release.msi.sha256
    ));
    out.push_str(&format!(
        "  portable {}  {}\n",
        release.portable.filename, release.portable.sha256
    ));
    let policy_path = request.repo_root.join("packaging/publication-policy.json");
    if policy_path.is_file() {
        let policy: serde_json::Value = serde_json::from_slice(&fs::read(&policy_path)?)?;
        if let Some(channels) = policy["channels"].as_object() {
            for (name, channel) in channels {
                out.push_str(&format!(
                    "  {name:<12} {} {}\n",
                    channel["state"].as_str().unwrap_or("?"),
                    channel["version"].as_str().unwrap_or("?")
                ));
            }
        }
    }
    if let Some(prepared) = &request.prepared {
        let readiness = ReadinessReport::load(&prepared.join(READINESS_NAME))?;
        out.push_str(&format!(
            "\nprepared readiness: {}\n",
            if readiness.ready_for_distribution {
                "READY FOR DISTRIBUTION"
            } else {
                "NOT READY"
            }
        ));
        for (name, channel) in &readiness.channels {
            out.push_str(&format!("  {name:<12} {}\n", channel.state));
        }
    }
    Ok(out)
}

pub fn validate(request: &ValidateRequest) -> Result<ReadinessReport> {
    ensure!(
        request.prepared.is_dir(),
        "no prepared distribution at {}",
        request.prepared.display()
    );
    let release = load_release_model(&request.prepared)?;
    verify_downloaded(&release, &request.assets)?;
    full_validation(&request.prepared, &release.version)?;

    let mut readiness = ReadinessReport::new(&release);
    readiness
        .channels
        .insert("chocolatey".into(), channel_stub(&release.version));
    readiness
        .channels
        .insert("winget".into(), channel_stub(&release.version));
    readiness
        .channels
        .insert("scoop".into(), channel_stub(&release.version));
    check(&mut readiness, "chocolatey", "packageValidation", true);
    check(&mut readiness, "winget", "manifestValidation", true);
    check(&mut readiness, "scoop", "manifestValidation", true);
    readiness
        .channels
        .get_mut("winget")
        .unwrap()
        .manifest_sha256 = Some(manifest_digest(&request.prepared, &release.version)?);

    let nupkg = request
        .prepared
        .join("packaging/chocolatey")
        .join(nupkg_name(&release.version));
    let (nupkg_sha, _) = sha256_file(&nupkg)
        .with_context(|| format!("the frozen {} is missing", nupkg.display()))?;
    check(&mut readiness, "chocolatey", "pack", true);
    readiness
        .channels
        .get_mut("chocolatey")
        .unwrap()
        .package_filename = Some(nupkg_name(&release.version));
    readiness
        .channels
        .get_mut("chocolatey")
        .unwrap()
        .package_sha256 = Some(nupkg_sha);

    let frozen_path = request.prepared.join(READINESS_NAME);
    if frozen_path.is_file() {
        let frozen = ReadinessReport::load(&frozen_path)?;
        for (channel, field, value) in [
            (
                "chocolatey",
                "package",
                readiness.channels["chocolatey"]
                    .package_sha256
                    .clone()
                    .unwrap_or_default(),
            ),
            (
                "winget",
                "manifest",
                readiness.channels["winget"]
                    .manifest_sha256
                    .clone()
                    .unwrap_or_default(),
            ),
        ] {
            let recorded = if field == "package" {
                frozen.channels[channel].package_sha256.clone()
            } else {
                frozen.channels[channel].manifest_sha256.clone()
            };
            ensure!(
                recorded.as_deref() == Some(value.as_str()),
                "the frozen {channel} {field} changed after preparation"
            );
        }
    }

    match &request.chocolatey_rehearsal {
        Some(path) => {
            let text =
                fs::read(path).with_context(|| format!("could not read {}", path.display()))?;
            let rehearsal: serde_json::Value =
                serde_json::from_slice(&text).context("the rehearsal result is not JSON")?;
            ensure!(
                rehearsal["ok"] == true,
                "the Chocolatey rehearsal did not pass"
            );
            ensure!(
                rehearsal["msiSha256"]
                    .as_str()
                    .is_some_and(|sha| sha.eq_ignore_ascii_case(&release.msi.sha256)),
                "the Chocolatey rehearsal measured a different MSI"
            );
            check(&mut readiness, "chocolatey", "rehearsal", true);
        }
        None => {
            check(&mut readiness, "chocolatey", "rehearsal", false);
            readiness
                .channels
                .get_mut("chocolatey")
                .unwrap()
                .detail
                .push("no rehearsal result was supplied".into());
        }
    }

    for channel in readiness.channels.values_mut() {
        channel.msi_url = Some(release.msi.url.clone());
        channel.msi_sha256 = Some(release.msi.sha256.clone());
    }
    readiness.channels.get_mut("scoop").unwrap().zip_url = Some(release.portable.url.clone());
    readiness.channels.get_mut("scoop").unwrap().zip_sha256 = Some(release.portable.sha256.clone());
    readiness.finalize();

    let bytes = serde_json::to_vec_pretty(&readiness)?;
    let digest = release::sha256_hex(&bytes);
    if let Some(expected) = &request.expect_readiness_digest {
        ensure!(
            digest.eq_ignore_ascii_case(expected),
            "the revalidated readiness digest {digest} differs from the frozen preparation digest {expected}"
        );
    }
    readiness.write(&request.prepared)?;
    ensure!(
        ReadinessReport::frozen_digest(&request.prepared)? == digest,
        "the readiness digest did not survive writing"
    );
    Ok(readiness)
}

fn require_ready(readiness: &ReadinessReport) -> Result<()> {
    ensure!(
        readiness.ready_for_distribution,
        "the distribution readiness report is not READY FOR DISTRIBUTION"
    );
    for (name, channel) in &readiness.channels {
        ensure!(
            channel.state == "READY",
            "channel {name} is {}",
            channel.state
        );
    }
    Ok(())
}

/// Credentials a publish operation needs. Every one is checked before the
/// first external write so a missing later credential cannot leave a channel
/// already submitted; only PRESENT or MISSING is ever printed.
pub const SUBMISSION_CREDENTIALS: [&str; 3] = [
    "CHOCOLATEY_API_KEY",
    "WINGET_SUBMIT_TOKEN",
    "SCOOP_BUCKET_TOKEN",
];

/// Non-empty status of every required submission credential, by name.
pub fn credential_status(secrets: &dyn SecretSource) -> Vec<(&'static str, bool)> {
    SUBMISSION_CREDENTIALS
        .iter()
        .map(|name| (*name, secrets.get(name).is_some()))
        .collect()
}

/// Fails before any external write when a required credential is missing.
/// Prints one `NAME PRESENT` or `NAME MISSING` line per credential and never
/// a value, prefix, suffix, length or hash.
pub fn credential_preflight(secrets: &dyn SecretSource) -> Result<()> {
    let status = credential_status(secrets);
    for (name, present) in &status {
        println!("{name} {}", if *present { "PRESENT" } else { "MISSING" });
    }
    let missing: Vec<&str> = status
        .iter()
        .filter(|(_, present)| !present)
        .map(|(name, _)| *name)
        .collect();
    ensure!(
        missing.is_empty(),
        "required submission credentials are missing: {}",
        missing.join(", ")
    );
    Ok(())
}

pub fn submit(
    request: &SubmitRequest,
    runner: &dyn CommandRunner,
    secrets: &dyn SecretSource,
) -> Result<SubmissionResult> {
    ensure!(!request.out.exists(), "submission output must not exist");
    let frozen = ReadinessReport::frozen_digest(&request.prepared)?;
    if let Some(expected) = &request.expect_readiness_digest {
        ensure!(
            frozen.eq_ignore_ascii_case(expected),
            "the frozen readiness digest {frozen} differs from the approved {expected}"
        );
    }
    let readiness = ReadinessReport::load(&request.prepared.join(READINESS_NAME))?;
    require_ready(&readiness)?;
    // Every credential the publish operation needs is checked before the first
    // external write, so a missing later credential cannot leave one channel
    // submitted and the rest unwritten.
    credential_preflight(secrets)?;

    let (status, url, detail) = match request.channel {
        Channel::Chocolatey => {
            let key = secrets
                .get("CHOCOLATEY_API_KEY")
                .context("CHOCOLATEY_API_KEY is not configured")?;
            let nupkg = request
                .prepared
                .join("packaging/chocolatey")
                .join(nupkg_name(&readiness.version))
                .to_string_lossy()
                .into_owned();
            // Chocolatey consumes an API key only through --api-key or a
            // previously configured source; it never reads an environment
            // variable, so the key is passed explicitly for this push.
            let output = runner.run(
                "choco",
                &[
                    Arg::Plain("push"),
                    Arg::Plain(&nupkg),
                    Arg::Plain("--source"),
                    Arg::Plain("https://push.chocolatey.org/"),
                    Arg::Plain("--api-key"),
                    Arg::Secret(&key),
                ],
                &[],
            )?;
            let stdout = redact(&output.stdout, &key);
            let stderr = redact(&output.stderr, &key);
            ensure!(
                output.status == 0,
                "choco push failed with {}:\n{stdout}{stderr}",
                output.status
            );
            (
                "SUBMITTED",
                format!(
                    "https://community.chocolatey.org/packages/exosnap/{}",
                    readiness.version
                ),
                stdout.trim().to_string(),
            )
        }
        Channel::Winget => {
            let token = secrets
                .get("WINGET_SUBMIT_TOKEN")
                .context("WINGET_SUBMIT_TOKEN is not configured")?;
            let manifest = request.prepared.join(format!(
                "packaging/winget/manifests/c/Codexo/ExoSnap/{}",
                readiness.version
            ));
            let title = format!("New package: Codexo.ExoSnap version {}", readiness.version);
            let manifest = manifest.to_string_lossy().into_owned();
            let output = runner.run(
                "wingetcreate",
                &[
                    Arg::Plain("submit"),
                    Arg::Plain("--prtitle"),
                    Arg::Plain(&title),
                    Arg::Plain("--token"),
                    Arg::Secret(&token),
                    Arg::Plain(&manifest),
                ],
                &[],
            )?;
            let stdout = redact(&output.stdout, &token);
            let stderr = redact(&output.stderr, &token);
            ensure!(
                output.status == 0,
                "wingetcreate submit failed with {}:\n{stdout}{stderr}",
                output.status
            );
            let url = stdout
                .split_whitespace()
                .find(|word| word.starts_with("https://github.com/microsoft/winget-pkgs/pull/"))
                .context("wingetcreate did not report a pull request URL")?
                .trim_end_matches(|c: char| !c.is_ascii_digit())
                .to_string();
            ("SUBMITTED", url, stdout.trim().to_string())
        }
        Channel::Scoop => {
            let token = secrets
                .get("SCOOP_BUCKET_TOKEN")
                .context("SCOOP_BUCKET_TOKEN is not configured")?;
            let manifest = request.prepared.join("packaging/scoop/exosnap.json");
            let content = fs::read(&manifest)?;
            let existing = runner.run(
                "gh",
                &[
                    Arg::Plain("api"),
                    Arg::Plain("--method"),
                    Arg::Plain("GET"),
                    Arg::Plain("repos/Exoridus/scoop-exosnap/contents/bucket/exosnap.json"),
                    Arg::Plain("--jq"),
                    Arg::Plain(".sha"),
                ],
                &[("GH_TOKEN", token.clone())],
            )?;
            let mut body = serde_json::json!({
                "message": format!("exosnap: update to {}", readiness.version),
                "content": base64_of(&content),
            });
            if existing.status == 0 {
                body["sha"] = serde_json::Value::String(existing.stdout.trim().to_string());
            } else {
                ensure!(
                    existing.stderr.contains("404"),
                    "could not read the bucket manifest: {}",
                    redact(&existing.stderr, &token)
                );
            }
            let body_path = request.out.with_extension("scoop-put.json");
            fs::write(&body_path, serde_json::to_vec(&body)?)?;
            let body_arg = body_path.to_string_lossy().into_owned();
            let output = runner.run(
                "gh",
                &[
                    Arg::Plain("api"),
                    Arg::Plain("--method"),
                    Arg::Plain("PUT"),
                    Arg::Plain("repos/Exoridus/scoop-exosnap/contents/bucket/exosnap.json"),
                    Arg::Plain("--input"),
                    Arg::Plain(&body_arg),
                ],
                &[("GH_TOKEN", token.clone())],
            )?;
            ensure!(
                output.status == 0,
                "the scoop bucket update failed with {}:\n{}{}",
                output.status,
                redact(&output.stdout, &token),
                redact(&output.stderr, &token)
            );
            let response: serde_json::Value = serde_json::from_str(&output.stdout)
                .context("the bucket update returned no JSON")?;
            let commit = response["commit"]["html_url"]
                .as_str()
                .or_else(|| response["content"]["html_url"].as_str())
                .unwrap_or("https://github.com/Exoridus/scoop-exosnap")
                .to_string();
            ("PUBLISHED", commit, "bucket manifest committed".to_string())
        }
    };

    let result = SubmissionResult {
        schema: "exosnap.distribution-submission/1".into(),
        channel: request.channel.name().to_string(),
        version: readiness.version.clone(),
        status: status.to_string(),
        url,
        detail,
    };
    if let Some(parent) = request.out.parent() {
        fs::create_dir_all(parent)?;
    }
    let mut bytes = serde_json::to_vec_pretty(&result)?;
    bytes.push(b'\n');
    fs::write(&request.out, bytes)?;
    Ok(result)
}

fn base64_of(bytes: &[u8]) -> String {
    use base64::Engine as _;
    base64::engine::general_purpose::STANDARD.encode(bytes)
}

#[cfg(test)]
mod tests;
