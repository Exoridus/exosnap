//! Imports the MSVC toolchain environment for the Ninja presets.
//!
//! The Visual Studio generator finds its own toolchain; Ninja needs cl.exe, INCLUDE
//! and LIB in the environment. Two properties of vcvars64.bat are not obvious:
//!
//! - It inherits VSINSTALLDIR. A value without its trailing separator makes vcvars
//!   derive "<install>VC\", which does not exist, and the VC initialisation is
//!   skipped in silence; CMake later reports no CMAKE_CXX_COMPILER. Every
//!   VS-derived variable is therefore cleared before the call.
//! - Its exit code is unusable: optional extension scripts unrelated to the
//!   compiler can fail it while the toolchain came up. The result is validated by
//!   finding cl.exe on the resulting PATH instead.

use std::path::{Path, PathBuf};

use anyhow::{Context, bail};
use serde_json::Value;

pub type EnvVars = Vec<(String, String)>;

#[cfg(windows)]
const INHERITED_VISUAL_STUDIO_VARIABLES: [&str; 11] = [
    "VSINSTALLDIR",
    "VCINSTALLDIR",
    "VCToolsInstallDir",
    "VCToolsRedistDir",
    "VCIDEInstallDir",
    "VS170COMNTOOLS",
    "VS180COMNTOOLS",
    "VSCMD_VER",
    "VSCMD_ARG_HOST_ARCH",
    "VSCMD_ARG_TGT_ARCH",
    "VSCMD_VCVARSALL_INIT",
];

/// Whether a configure preset builds with Ninja, following `inherits`.
pub fn preset_uses_ninja(repo_root: &Path, name: &str) -> bool {
    let Ok(text) = std::fs::read_to_string(repo_root.join("CMakePresets.json")) else {
        return false;
    };
    let Ok(document) = serde_json::from_str::<Value>(&text) else {
        return false;
    };
    let presets = document["configurePresets"]
        .as_array()
        .cloned()
        .unwrap_or_default();
    let mut current = Some(name.to_string());
    // Bounded: a cycle in inherits is a broken presets file, and hanging is a
    // worse way to report it.
    for _ in 0..16 {
        let Some(wanted) = current.take() else { break };
        let Some(preset) = presets.iter().find(|p| p["name"] == wanted.as_str()) else {
            return false;
        };
        if let Some(generator) = preset["generator"].as_str().filter(|g| !g.is_empty()) {
            return generator == "Ninja";
        }
        current = match &preset["inherits"] {
            Value::String(parent) => Some(parent.clone()),
            Value::Array(parents) => parents.first().and_then(Value::as_str).map(str::to_string),
            _ => None,
        };
    }
    false
}

pub fn find_compiler(path_value: &str) -> Option<PathBuf> {
    std::env::split_paths(path_value)
        .map(|dir| dir.join("cl.exe"))
        .find(|candidate| candidate.is_file())
}

/// The environment that makes cl.exe reachable, or `None` when it already is.
/// Returns the compiler path next to the variables for the informational line.
pub fn environment() -> anyhow::Result<Option<(PathBuf, EnvVars)>> {
    if find_compiler(&std::env::var("PATH").unwrap_or_default()).is_some() {
        return Ok(None);
    }
    let installation = find_installation().context(
        "No Visual Studio installation with the x64 C++ toolset was found. Install the \"Desktop development with C++\" workload.",
    )?;
    let variables = vcvars_environment(&installation)?;
    let path = variables
        .iter()
        .find(|(name, _)| name.eq_ignore_ascii_case("Path"))
        .map(|(_, value)| value.as_str())
        .unwrap_or_default();
    let Some(compiler) = find_compiler(path) else {
        bail!(
            "vcvars64.bat ran for '{}' but produced no cl.exe. Run it by hand with VSCMD_DEBUG=2 \
             to see which initialization step failed.",
            installation.display()
        );
    };
    Ok(Some((compiler, variables)))
}

/// The latest Visual Studio installation with the x64 C++ toolset, whatever
/// its product version. The Ninja/vcvars64 import wants exactly this: the
/// newest usable toolchain, not a particular VS line.
pub(crate) fn find_installation() -> Option<PathBuf> {
    vswhere_installation_path(&[])
}

/// The latest VS-2022-line installation with the x64 C++ toolset, or `None`
/// when only an installation outside that line (an older VS, or a preview
/// channel vswhere's plain `-latest` would prefer) exists. `-version
/// "[17.0,18.0)"` is VS 2022's product-version range; this excludes both an
/// older VS and a preview/next line without hardcoding a folder path.
pub(crate) fn find_installation_vs2022() -> Option<PathBuf> {
    vswhere_installation_path(&["-version", "[17.0,18.0)"])
}

fn vswhere_installation_path(extra_args: &[&str]) -> Option<PathBuf> {
    let program_files = std::env::var_os("ProgramFiles(x86)")?;
    let vswhere = Path::new(&program_files).join("Microsoft Visual Studio/Installer/vswhere.exe");
    if !vswhere.is_file() {
        return None;
    }
    let mut command = crate::process::command(&vswhere.to_string_lossy());
    command.args([
        "-latest",
        "-products",
        "*",
        "-requires",
        "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
    ]);
    command.args(extra_args);
    command.args(["-property", "installationPath"]);
    let (_, stdout) = crate::process::query(command).ok()?;
    stdout
        .lines()
        .map(str::trim)
        .find(|l| !l.is_empty())
        .map(PathBuf::from)
}

#[cfg(windows)]
fn vcvars_environment(installation: &Path) -> anyhow::Result<Vec<(String, String)>> {
    use std::os::windows::process::CommandExt;

    let vcvars = installation.join("VC/Auxiliary/Build/vcvars64.bat");
    if !vcvars.is_file() {
        bail!(
            "vcvars64.bat not found under '{}'. The installation lacks the C++ build tools.",
            installation.display()
        );
    }
    let clear: Vec<String> = INHERITED_VISUAL_STUDIO_VARIABLES
        .iter()
        .map(|name| format!("set \"{name}=\""))
        .collect();
    // `&`, not `&&`: the exit code is unusable (see the module documentation).
    let script = format!(
        "{} & call \"{}\" >nul 2>&1 & set",
        clear.join(" & "),
        vcvars.display()
    );
    let mut command = crate::process::command("cmd");
    command.arg("/c").raw_arg(script);
    let (_, stdout) = crate::process::query(command)?;
    Ok(parse_set_output(&stdout))
}

#[cfg(not(windows))]
fn vcvars_environment(_installation: &Path) -> anyhow::Result<Vec<(String, String)>> {
    bail!("the MSVC environment exists only on Windows")
}

/// Parses `set` output. Entries whose name is empty (cmd's per-drive `=C:=C:\`) are
/// skipped.
#[cfg_attr(not(windows), allow(dead_code))]
fn parse_set_output(text: &str) -> Vec<(String, String)> {
    text.lines()
        .filter_map(|line| {
            let split = line.find('=')?;
            (split >= 1).then(|| (line[..split].to_string(), line[split + 1..].to_string()))
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn set_output_keeps_values_with_equals_signs_and_drops_drive_entries() {
        let parsed = parse_set_output("=C:=C:\\x\nPath=C:\\a;C:\\b\nX=a=b\n\n");
        assert_eq!(
            parsed,
            vec![
                ("Path".to_string(), "C:\\a;C:\\b".to_string()),
                ("X".to_string(), "a=b".to_string())
            ]
        );
    }

    #[test]
    fn presets_resolve_their_generator_through_inherits() {
        let dir = tempfile::tempdir().unwrap();
        std::fs::write(
            dir.path().join("CMakePresets.json"),
            r#"{"configurePresets":[
                {"name":"base-ninja","generator":"Ninja"},
                {"name":"child","inherits":["base-ninja"]},
                {"name":"vs","generator":"Visual Studio 17 2022"},
                {"name":"loop-a","inherits":"loop-b"},
                {"name":"loop-b","inherits":"loop-a"}
            ]}"#,
        )
        .unwrap();
        assert!(preset_uses_ninja(dir.path(), "base-ninja"));
        assert!(preset_uses_ninja(dir.path(), "child"));
        assert!(!preset_uses_ninja(dir.path(), "vs"));
        assert!(!preset_uses_ninja(dir.path(), "missing"));
        assert!(!preset_uses_ninja(dir.path(), "loop-a"));
    }

    #[test]
    fn the_repository_debug_preset_is_ninja() {
        let repo = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
        assert!(preset_uses_ninja(&repo, "windows-x64-ninja-debug"));
    }
}
