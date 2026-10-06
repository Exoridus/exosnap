//! Shared CI provisioning for analysis tools.

use std::io::Write;
use std::path::Path;

const CPPCHECK_PACKAGE_VERSION: &str = "2.19.0";

fn cppcheck_version_matches(output: &str) -> bool {
    matches!(output.trim(), "Cppcheck 2.19" | "Cppcheck 2.19.0")
}

fn version(tool: &Path) -> anyhow::Result<String> {
    let mut command = crate::process::command(&tool.to_string_lossy());
    command.arg("--version");
    let (code, output) = crate::process::query(command)?;
    anyhow::ensure!(code == 0, "cppcheck --version failed: {output}");
    Ok(output)
}

pub fn cppcheck() -> anyhow::Result<()> {
    let installed = crate::lint::quality::discover_cppcheck();
    let matching = installed
        .as_ref()
        .is_some_and(|path| version(path).is_ok_and(|v| cppcheck_version_matches(&v)));
    if !matching {
        anyhow::ensure!(
            cfg!(windows),
            "install cppcheck {CPPCHECK_PACKAGE_VERSION} before running setup on this platform"
        );
        let status = crate::process::command("choco")
            .args([
                "upgrade",
                "cppcheck",
                "--version",
                CPPCHECK_PACKAGE_VERSION,
                "--yes",
                "--no-progress",
                "--allow-downgrade",
            ])
            .status()?;
        anyhow::ensure!(status.success(), "cppcheck provisioning failed ({status})");
    }
    let tool = crate::lint::quality::discover_cppcheck()
        .ok_or_else(|| anyhow::anyhow!("cppcheck unavailable after provisioning"))?;
    let version = version(&tool)?;
    anyhow::ensure!(
        cppcheck_version_matches(&version),
        "expected cppcheck {CPPCHECK_PACKAGE_VERSION}, found {version}"
    );
    if let Some(path) = std::env::var_os("GITHUB_PATH") {
        let mut file = std::fs::OpenOptions::new().append(true).open(path)?;
        writeln!(
            file,
            "{}",
            tool.parent().expect("executable parent").display()
        )?;
    }
    println!("{}: {}", tool.display(), version.trim());
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cppcheck_pin_rejects_missing_failed_and_different_tools() {
        for output in [
            "",
            "not installed",
            "Cppcheck 2.16",
            "Cppcheck 2.190",
            "Cppcheck 2.19\nerror",
        ] {
            assert!(!cppcheck_version_matches(output), "{output}");
        }
        assert!(cppcheck_version_matches("Cppcheck 2.19\r\n"));
        assert!(cppcheck_version_matches("Cppcheck 2.19.0\n"));
    }
}
