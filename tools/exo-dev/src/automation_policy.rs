//! Tracked-source contract for Rust-owned automation.

use std::io::Read;
use std::path::Path;

pub fn check(root: &Path) -> anyhow::Result<Vec<String>> {
    check_tree(root, false)
}

pub fn check_staged(root: &Path) -> anyhow::Result<Vec<String>> {
    check_tree(root, true)
}

fn check_tree(root: &Path, staged: bool) -> anyhow::Result<Vec<String>> {
    let git = crate::git::Git::new(root);
    let (code, inventory) = git.run(&["ls-files", "-z"]);
    anyhow::ensure!(code == 0, "automation-policy: git inventory failed");
    let (code, differences) = git.run(&["diff", "--name-only", "-z"]);
    anyhow::ensure!(
        code == 0,
        "automation-policy: git working tree comparison failed"
    );
    let differences: std::collections::HashSet<_> = differences.split('\0').collect();
    let mut findings = Vec::new();
    for name in inventory.split('\0').filter(|name| !name.is_empty()) {
        let path = root.join(name);
        let indexed = if staged && differences.contains(name) {
            let (code, text) = git.run(&["show", &format!(":{name}")]);
            anyhow::ensure!(
                code == 0,
                "automation-policy: cannot read index entry {name}"
            );
            Some(text)
        } else {
            None
        };
        // A tracked deletion is part of the candidate tree, not a script to run.
        if indexed.is_none() && !path.try_exists()? {
            continue;
        }
        if name == "packaging/chocolatey/tools/chocolateyinstall.ps1" {
            let text = match indexed {
                Some(text) => text,
                None => std::fs::read_to_string(&path)?,
            };
            if !chocolatey_adapter(&text) {
                findings.push(format!(
                    "{name}: external adapter contains unexpected automation"
                ));
            }
            continue;
        }
        let extension = path
            .extension()
            .and_then(|s| s.to_str())
            .unwrap_or_default()
            .to_ascii_lowercase();
        if matches!(
            extension.as_str(),
            "ps1"
                | "psm1"
                | "psd1"
                | "ps1xml"
                | "py"
                | "pyw"
                | "sh"
                | "bash"
                | "zsh"
                | "fish"
                | "ksh"
                | "cmd"
                | "bat"
                | "csx"
                | "cs"
        ) {
            findings.push(format!(
                "{name}: standalone automation must be implemented in Rust"
            ));
            continue;
        }
        let mut prefix = Vec::new();
        let complete = if let Some(text) = indexed {
            prefix.extend_from_slice(&text.as_bytes()[..text.len().min(4096)]);
            text.len() <= 4096
        } else {
            std::fs::File::open(&path)?
                .take(4096)
                .read_to_end(&mut prefix)?;
            std::fs::metadata(&path)?.len() == prefix.len() as u64
        };
        let text = String::from_utf8_lossy(&prefix);
        if matches!(name, ".githooks/pre-commit" | ".githooks/pre-push")
            && complete
            && hook_launcher(name, &text)
        {
            continue;
        }
        if prohibited_shebang(&text) {
            findings.push(format!(
                "{name}: interpreter shebang hides standalone automation"
            ));
        }
    }
    Ok(findings)
}

const CHOCOLATEY_ADAPTER: &str = r#"
$ErrorActionPreference = 'Stop'
if (-not (Get-OSArchitectureWidth -Compare 64) -or $env:ChocolateyForceX86 -eq 'true') {
throw 'ExoSnap only ships an x64 build. No 32-bit package is published.'
}
$packageArgs = @{
packageName    = $env:ChocolateyPackageName
fileType       = 'msi'
url64bit       = '<url>'
checksum64     = '<sha256>'
checksumType64 = 'sha256'
softwareName   = 'ExoSnap*'
silentArgs     = '/qn /norestart EXOSNAP_DISTRIBUTION_OWNER=chocolatey'
validExitCodes = @(0, 3010, 1641)
}
Install-ChocolateyPackage @packageArgs
"#;

fn chocolatey_adapter(text: &str) -> bool {
    let url = regex::Regex::new(r"^url64bit\s*=\s*'https://github\.com/Exoridus/exosnap/releases/download/v[0-9.]+/ExoSnap-[0-9.]+-windows-x64\.msi'$").expect("adapter URL");
    let checksum =
        regex::Regex::new(r"^checksum64\s*=\s*'[0-9a-f]{64}'$").expect("adapter checksum");
    let statements = |text: &str, normalize: bool| -> Vec<String> {
        text.lines()
            .map(str::trim)
            .filter(|line| !line.is_empty() && !line.starts_with('#'))
            .map(|line| {
                if normalize && url.is_match(line) {
                    "url64bit       = '<url>'".into()
                } else if normalize && checksum.is_match(line) {
                    "checksum64     = '<sha256>'".into()
                } else {
                    line.to_string()
                }
            })
            .collect()
    };
    statements(text, true) == statements(CHOCOLATEY_ADAPTER, false)
}

fn hook_launcher(name: &str, text: &str) -> bool {
    if text.lines().next() != Some("#!/bin/sh") {
        return false;
    }
    let lines: Vec<_> = text
        .lines()
        .map(str::trim)
        .filter(|line| !line.is_empty() && !line.starts_with('#'))
        .collect();
    let hook = name.strip_prefix(".githooks/").unwrap_or_default();
    lines
        == [
            r#"cd "$(git rev-parse --show-toplevel)" || exit 1"#,
            &format!("exec cargo exo-dev hook {hook}"),
        ]
}

fn prohibited_shebang(text: &str) -> bool {
    let Some(line) = text
        .trim_start_matches('\u{feff}')
        .lines()
        .next()
        .and_then(|line| line.strip_prefix("#!"))
    else {
        return false;
    };
    line.split_whitespace().any(|word| {
        let name = word
            .rsplit(['/', '\\'])
            .next()
            .unwrap_or(word)
            .to_ascii_lowercase();
        let name = name.trim_end_matches(".exe");
        matches!(
            name,
            "sh" | "bash"
                | "zsh"
                | "fish"
                | "ksh"
                | "dash"
                | "pwsh"
                | "powershell"
                | "dotnet-script"
                | "csi"
                | "cmd"
        ) || name
            .strip_prefix("python")
            .is_some_and(|suffix| suffix.chars().all(|c| c.is_ascii_digit() || c == '.'))
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_support::fixture_repo;

    #[test]
    fn standalone_automation_extensions_fail() {
        for extension in [
            "ps1", "psm1", "psd1", "ps1xml", "py", "pyw", "sh", "bash", "zsh", "fish", "ksh",
            "cmd", "bat", "csx", "cs",
        ] {
            let name = format!("tools/helper.{extension}");
            let dir = fixture_repo(&[(&name, "fixture\n")]);
            assert_eq!(check(dir.path()).unwrap().len(), 1, "{name}");
        }
    }

    #[test]
    fn obvious_interpreters_cannot_hide_in_extensionless_files() {
        for interpreter in [
            "/bin/sh",
            "/usr/bin/python3",
            "/usr/bin/env bash",
            "/usr/bin/env -S python3 -u",
            "/usr/bin/env pwsh",
            "/usr/bin/env dotnet-script",
        ] {
            let content = format!("#!{interpreter}\n");
            let dir = fixture_repo(&[("tools/helper", &content)]);
            assert_eq!(check(dir.path()).unwrap().len(), 1, "{interpreter}");
        }
    }

    #[test]
    fn windows_shebang_paths_are_not_a_renaming_escape() {
        assert!(prohibited_shebang(r"#!C:\Python312\python.exe"));
        assert!(prohibited_shebang(
            r"#!C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe"
        ));
    }

    #[test]
    fn source_documentation_and_embedded_fixture_examples_pass() {
        let dir = fixture_repo(&[
            ("docs/example.md", "```sh\n#!/bin/bash\n```\n"),
            ("tools/example.rs", "const FIXTURE: &str = \"#!/bin/sh\";\n"),
            ("tests/fixture.cpp", "// Example fixture\n"),
            ("CMakeLists.txt", "cmake_minimum_required(VERSION 3.27)\n"),
            (".github/workflows/test.yml", "run: echo test\n"),
        ]);
        assert!(check(dir.path()).unwrap().is_empty());
    }

    #[test]
    fn external_adapter_exception_is_one_exact_path() {
        let dir = fixture_repo(&[
            (
                "packaging/chocolatey/tools/chocolateyinstall.ps1",
                include_str!("../../../packaging/chocolatey/tools/chocolateyinstall.ps1"),
            ),
            ("packaging/chocolatey/tools/helper.ps1", "helper\n"),
            ("tools/chocolateyinstall.ps1", "helper\n"),
            (
                "packaging/chocolatey/tools/chocolateyuninstall.ps1",
                "helper\n",
            ),
        ]);
        let findings = check(dir.path()).unwrap();
        assert_eq!(findings.len(), 3);
        assert!(
            !findings
                .iter()
                .any(|f| f.starts_with("packaging/chocolatey/tools/chocolateyinstall.ps1:"))
        );
    }

    #[test]
    fn inventory_failure_is_not_a_clean_repository() {
        let dir = tempfile::tempdir().unwrap();
        assert!(check(dir.path()).is_err());
    }

    #[test]
    fn external_adapter_cannot_gain_repository_logic() {
        let adapter = include_str!("../../../packaging/chocolatey/tools/chocolateyinstall.ps1");
        assert!(chocolatey_adapter(adapter));
        assert!(!chocolatey_adapter(&format!(
            "{adapter}\nStart-Process exo-dev\n"
        )));
        assert!(!chocolatey_adapter(&adapter.replace(
            "Install-ChocolateyPackage @packageArgs",
            "Invoke-Expression $packageArgs"
        )));
    }

    #[test]
    fn unusual_git_path_and_uppercase_extension_are_checked() {
        let dir = fixture_repo(&[("tools/a space.PS1", "fixture\n")]);
        assert_eq!(check(dir.path()).unwrap().len(), 1);
    }

    #[test]
    fn git_shell_launchers_cannot_accumulate_logic() {
        let launcher = "#!/bin/sh\ncd \"$(git rev-parse --show-toplevel)\" || exit 1\nexec cargo exo-dev hook pre-commit\n";
        let dir = fixture_repo(&[(".githooks/pre-commit", launcher)]);
        assert!(check(dir.path()).unwrap().is_empty());
        std::fs::write(
            dir.path().join(".githooks/pre-commit"),
            format!("{launcher}echo policy\n"),
        )
        .unwrap();
        assert_eq!(check(dir.path()).unwrap().len(), 1);
    }

    #[test]
    fn staged_automation_cannot_hide_behind_worktree_edits() {
        for (name, body) in [
            ("tools/helper.ps1", "echo helper\n"),
            ("tools/helper", "#!/bin/sh\necho helper\n"),
        ] {
            let dir = fixture_repo(&[(name, body)]);
            std::fs::remove_file(dir.path().join(name)).unwrap();
            assert_eq!(check_staged(dir.path()).unwrap().len(), 1);
            assert!(check(dir.path()).unwrap().is_empty());
        }
    }

    #[test]
    fn hook_interpreter_cannot_inject_more_commands() {
        let body = "#!/usr/bin/env -S bash -c 'echo injected'\ncd \"$(git rev-parse --show-toplevel)\" || exit 1\nexec cargo exo-dev hook pre-commit\n";
        let dir = fixture_repo(&[(".githooks/pre-commit", body)]);
        assert_eq!(check(dir.path()).unwrap().len(), 1);
    }

    #[test]
    fn policy_runs_in_both_local_modes_and_ci_guardrails() {
        for profile in [
            crate::profile::Profile::PreCommit,
            crate::profile::Profile::PrePush,
            crate::profile::Profile::CiGuardrails,
        ] {
            let scope = crate::scope::Scope::of(&["docs/example.md".into()]);
            let mut input = crate::plan::tests::input(profile, &scope);
            input.event = crate::plan::Event::PullRequest;
            let plan = crate::plan::build(&input);
            assert!(
                plan.check(crate::step::StepId::AutomationPolicy)
                    .unwrap()
                    .applicable
            );
        }
    }
}
