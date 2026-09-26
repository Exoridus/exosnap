//! Discovery for the LLVM-based tools (clang-format, clang-tidy, cppcheck) that
//! `check-format.ps1` and `check-quality.ps1` used to duplicate the search for.
//!
//! Three tiers, in order: the LLVM toolset bundled with the detected Visual
//! Studio "Desktop development with C++" installation, a standalone LLVM
//! install, then PATH. A candidate must be a real file, must not be a WinGet
//! "App Execution Alias" shim (those exist on PATH without resolving to a real
//! binary until the matching Store package is installed), and must answer
//! `--version` with exit code 0.
//!
//! `msvc::find_installation` is shared Phase A infrastructure: it asks
//! vswhere for the *latest* installation, which the build path genuinely
//! wants. `check-format.ps1`'s original search was never "the latest VS" --
//! it recursed literally under `...\Microsoft Visual Studio\2022\...` and
//! never looked at a preview channel (a different major version number, such
//! as a "18" install ahead of "2022" in vswhere's `-latest` ordering). The
//! VS-LLVM tier below keeps that scope: it accepts `find_installation`'s
//! result only when the path names the 2022 line, and otherwise falls
//! through to standalone LLVM and PATH exactly as the original script would
//! have (it would have found nothing under `2022` and moved on).

pub mod format;

use std::ffi::OsStr;
use std::path::{Path, PathBuf};

/// Finds the first of `names` (without the `.exe` suffix) as a real, runnable
/// binary. See the module documentation for the search order and what counts
/// as a match.
pub fn find_tool(names: &[&str]) -> Option<PathBuf> {
    let vs_llvm_root = crate::msvc::find_installation();
    let standalone_llvm_root = std::env::var_os("ProgramFiles").map(|p| Path::new(&p).join("LLVM"));
    let path_var = std::env::var_os("PATH").unwrap_or_default();
    let local_app_data = std::env::var_os("LOCALAPPDATA");
    find_tool_in(
        names,
        vs_llvm_root.as_deref(),
        standalone_llvm_root.as_deref(),
        &path_var,
        local_app_data.as_deref(),
    )
}

/// `find_tool`, with every search root injected instead of read from the real
/// environment, so a test can point each tier at a fixture directory.
fn find_tool_in(
    names: &[&str],
    vs_install_root: Option<&Path>,
    standalone_llvm_root: Option<&Path>,
    path_var: &OsStr,
    local_app_data: Option<&OsStr>,
) -> Option<PathBuf> {
    if let Some(root) = vs_install_root
        && is_vs_2022_install(root)
        && let Some(found) = find_under(root, names, Some("\\llvm\\x64\\bin\\"), local_app_data)
    {
        return Some(found);
    }
    if let Some(root) = standalone_llvm_root
        && let Some(found) = find_under(root, names, None, local_app_data)
    {
        return Some(found);
    }
    find_on_path(names, path_var, local_app_data)
}

/// Whether `find_installation`'s result names the 2022 line: `check-format.ps1`
/// never searched a preview or other-numbered channel, so an install outside
/// that scope must be treated as though vswhere had found nothing.
fn is_vs_2022_install(root: &Path) -> bool {
    root.components()
        .any(|component| component.as_os_str() == "2022")
}

/// The first `{name}.exe` under `root` (recursive) whose path, lowercased,
/// contains `must_contain` (when given) and that passes `is_usable_tool`.
fn find_under(
    root: &Path,
    names: &[&str],
    must_contain: Option<&str>,
    local_app_data: Option<&OsStr>,
) -> Option<PathBuf> {
    let mut candidates = Vec::new();
    collect_named_executables(root, names, &mut candidates);
    if let Some(marker) = must_contain {
        candidates.retain(|p| p.to_string_lossy().to_lowercase().contains(marker));
    }
    candidates
        .into_iter()
        .find(|p| is_usable_tool(p, local_app_data))
}

/// Recursively collects every `{name}.exe` under `dir`, case-insensitively,
/// for any of `names`. An unreadable directory (permissions, a broken
/// junction) is skipped rather than failing the whole search, matching
/// `Get-ChildItem -Recurse -ErrorAction SilentlyContinue`.
fn collect_named_executables(dir: &Path, names: &[&str], found: &mut Vec<PathBuf>) {
    let Ok(entries) = std::fs::read_dir(dir) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        let Ok(file_type) = entry.file_type() else {
            continue;
        };
        if file_type.is_dir() {
            collect_named_executables(&path, names, found);
            continue;
        }
        let is_exe = path
            .extension()
            .and_then(|e| e.to_str())
            .is_some_and(|e| e.eq_ignore_ascii_case("exe"));
        let name_matches = path
            .file_stem()
            .and_then(|s| s.to_str())
            .is_some_and(|stem| names.iter().any(|n| stem.eq_ignore_ascii_case(n)));
        if is_exe && name_matches {
            found.push(path);
        }
    }
}

/// The first `{name}.exe` PATH resolves to, per `names` in order. Only the
/// first PATH hit for a name is considered, matching `Get-Command`: a shim
/// that fails validation is not found, PATH is not searched further for that
/// name.
fn find_on_path(
    names: &[&str],
    path_var: &OsStr,
    local_app_data: Option<&OsStr>,
) -> Option<PathBuf> {
    for name in names {
        let candidate = std::env::split_paths(path_var)
            .map(|dir| dir.join(format!("{name}.exe")))
            .find(|candidate| candidate.is_file());
        if let Some(candidate) = candidate
            && is_usable_tool(&candidate, local_app_data)
        {
            return Some(candidate);
        }
    }
    None
}

fn is_usable_tool(path: &Path, local_app_data: Option<&OsStr>) -> bool {
    if !path.is_file() {
        return false;
    }
    if is_winget_alias_shim(path, local_app_data) {
        return false;
    }
    probes_ok(path)
}

fn is_winget_alias_shim(path: &Path, local_app_data: Option<&OsStr>) -> bool {
    let Some(local_app_data) = local_app_data else {
        return false;
    };
    let links = Path::new(local_app_data)
        .join("Microsoft")
        .join("WinGet")
        .join("Links");
    let links = links.to_string_lossy().to_lowercase();
    path.to_string_lossy().to_lowercase().starts_with(&links)
}

fn probes_ok(path: &Path) -> bool {
    let mut command = crate::process::command(&path.to_string_lossy());
    command.arg("--version");
    match crate::process::query(command) {
        Ok((code, _)) => code == 0,
        Err(_) => false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    /// A real, small executable that answers `--version` with exit 0, so a
    /// fixture candidate genuinely probes as usable without shelling out to
    /// the tool this module is meant to find.
    fn probeable_exe() -> PathBuf {
        let path_var = std::env::var_os("PATH").unwrap_or_default();
        std::env::split_paths(&path_var)
            .map(|dir| dir.join("git.exe"))
            .find(|p| p.is_file())
            .expect("git.exe must be on PATH for this test")
    }

    fn place_as(name: &str, dir: &Path) -> PathBuf {
        std::fs::create_dir_all(dir).unwrap();
        let dest = dir.join(format!("{name}.exe"));
        std::fs::copy(probeable_exe(), &dest).unwrap();
        dest
    }

    fn path_var(dirs: &[&Path]) -> std::ffi::OsString {
        std::env::join_paths(dirs).unwrap()
    }

    #[test]
    fn vs_bundled_llvm_wins_over_standalone_llvm_and_path() {
        let vs_dir = tempfile::tempdir().unwrap();
        // The 2022 component matters: only that line is in scope (see
        // `is_vs_2022_install`).
        let vs_root = vs_dir.path().join("2022/BuildTools");
        let llvm_bin = vs_root.join("VC/Tools/Llvm/x64/bin");
        let vs_tool = place_as("clang-format", &llvm_bin);

        let standalone_dir = tempfile::tempdir().unwrap();
        place_as("clang-format", &standalone_dir.path().join("bin"));

        let path_dir = tempfile::tempdir().unwrap();
        place_as("clang-format", path_dir.path());

        let found = find_tool_in(
            &["clang-format"],
            Some(&vs_root),
            Some(standalone_dir.path()),
            &path_var(&[path_dir.path()]),
            None,
        );
        assert_eq!(found, Some(vs_tool));
    }

    #[test]
    fn a_vs_bundled_clang_format_outside_llvm_x64_bin_is_not_matched() {
        let vs_dir = tempfile::tempdir().unwrap();
        let vs_root = vs_dir.path().join("2022/BuildTools");
        // Not under \Llvm\x64\bin\: the marker filter must reject it.
        place_as("clang-format", &vs_root.join("VC/Tools/Other"));

        let path_dir = tempfile::tempdir().unwrap();
        let path_tool = place_as("clang-format", path_dir.path());

        let found = find_tool_in(
            &["clang-format"],
            Some(&vs_root),
            None,
            &path_var(&[path_dir.path()]),
            None,
        );
        assert_eq!(found, Some(path_tool));
    }

    #[test]
    fn a_vs_install_outside_the_2022_line_is_skipped_in_favor_of_standalone_llvm() {
        let vs_dir = tempfile::tempdir().unwrap();
        // A preview/other-numbered channel, e.g. an install vswhere's
        // `-latest` prefers over 2022 on a machine with both installed. The
        // original script never searched outside `...\2022\...`.
        let preview_root = vs_dir.path().join("18/Community");
        place_as("clang-format", &preview_root.join("VC/Tools/Llvm/x64/bin"));

        let standalone_dir = tempfile::tempdir().unwrap();
        let standalone_tool = place_as("clang-format", &standalone_dir.path().join("bin"));

        let found = find_tool_in(
            &["clang-format"],
            Some(&preview_root),
            Some(standalone_dir.path()),
            &path_var(&[standalone_dir.path()]),
            None,
        );
        assert_eq!(found, Some(standalone_tool));
    }

    #[test]
    fn a_vs_install_outside_the_2022_line_with_nothing_else_available_is_none() {
        let vs_dir = tempfile::tempdir().unwrap();
        let preview_root = vs_dir.path().join("18/Community");
        place_as("clang-format", &preview_root.join("VC/Tools/Llvm/x64/bin"));

        let empty = tempfile::tempdir().unwrap();
        let found = find_tool_in(
            &["clang-format"],
            Some(&preview_root),
            None,
            &path_var(&[empty.path()]),
            None,
        );
        assert_eq!(found, None);
    }

    #[test]
    fn standalone_llvm_wins_over_path_when_vs_has_none() {
        let standalone_dir = tempfile::tempdir().unwrap();
        let standalone_tool = place_as("clang-format", &standalone_dir.path().join("bin"));

        let path_dir = tempfile::tempdir().unwrap();
        place_as("clang-format", path_dir.path());

        let found = find_tool_in(
            &["clang-format"],
            None,
            Some(standalone_dir.path()),
            &path_var(&[path_dir.path()]),
            None,
        );
        assert_eq!(found, Some(standalone_tool));
    }

    #[test]
    fn path_is_used_when_no_llvm_install_has_the_tool() {
        let path_dir = tempfile::tempdir().unwrap();
        let path_tool = place_as("clang-format", path_dir.path());

        let found = find_tool_in(
            &["clang-format"],
            None,
            None,
            &path_var(&[path_dir.path()]),
            None,
        );
        assert_eq!(found, Some(path_tool));
    }

    #[test]
    fn nothing_found_anywhere_is_none() {
        let empty = tempfile::tempdir().unwrap();
        let found = find_tool_in(
            &["clang-format"],
            None,
            None,
            &path_var(&[empty.path()]),
            None,
        );
        assert_eq!(found, None);
    }

    #[test]
    fn a_winget_links_shim_on_path_is_skipped_even_though_it_is_a_real_file() {
        let local_app_data = tempfile::tempdir().unwrap();
        let links = local_app_data.path().join("Microsoft/WinGet/Links");
        // A shim need not itself be a working binary; the path prefix alone
        // must be enough to reject it, matching the WinGet App Execution
        // Alias exception.
        std::fs::create_dir_all(&links).unwrap();
        std::fs::write(links.join("clang-format.exe"), b"").unwrap();

        let found = find_tool_in(
            &["clang-format"],
            None,
            None,
            &path_var(&[&links]),
            Some(local_app_data.path().as_os_str()),
        );
        assert_eq!(found, None);
    }

    #[test]
    fn a_path_entry_that_does_not_resolve_to_a_real_binary_is_not_a_hit() {
        let path_dir = tempfile::tempdir().unwrap();
        // A file with the right name and extension but no valid executable
        // content: the `--version` probe must fail closed, not be skipped.
        std::fs::write(path_dir.path().join("clang-format.exe"), b"not a real exe").unwrap();

        let found = find_tool_in(
            &["clang-format"],
            None,
            None,
            &path_var(&[path_dir.path()]),
            None,
        );
        assert_eq!(found, None);
    }

    #[test]
    fn the_first_matching_name_is_preferred() {
        let path_dir = tempfile::tempdir().unwrap();
        let first = place_as("clang-format", path_dir.path());
        place_as("clang-format-alt", path_dir.path());

        let found = find_tool_in(
            &["clang-format", "clang-format-alt"],
            None,
            None,
            &path_var(&[path_dir.path()]),
            None,
        );
        assert_eq!(found, Some(first));
    }
}
