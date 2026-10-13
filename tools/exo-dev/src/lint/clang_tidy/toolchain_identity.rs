//! Content identity of MSVC and SDK inputs omitted by Ninja's MSVC dependency parser.

use std::path::{Path, PathBuf};

pub(super) fn hash(build: &Path) -> Option<String> {
    let mut environment = Vec::new();
    for name in [
        "INCLUDE",
        "EXTERNAL_INCLUDE",
        "CL",
        "_CL_",
        "VCToolsInstallDir",
        "VCToolsVersion",
        "WindowsSdkDir",
        "WindowsSDKVersion",
        "UniversalCRTSdkDir",
        "UCRTVersion",
    ] {
        let value = match std::env::var(name) {
            Ok(value) => Some(value),
            Err(std::env::VarError::NotPresent) => None,
            Err(_) => return None,
        };
        environment.push((name, value));
    }
    hash_with_environment(build, &environment)
}

fn hash_with_environment(build: &Path, environment: &[(&str, Option<String>)]) -> Option<String> {
    // MSVC-mode Clang uses INCLUDE when supplied. Without that snapshot, SDK
    // registry discovery can diverge from CMake's cached resource-compiler path.
    if !environment.iter().any(|(name, value)| {
        *name == "INCLUDE"
            && value
                .as_deref()
                .is_some_and(|value| !value.trim().is_empty())
    }) {
        return None;
    }
    let cache = std::fs::read_to_string(build.join("CMakeCache.txt")).ok()?;
    let compiler = cache_path(&cache, "CMAKE_CXX_COMPILER")?;
    let resources = cache_path(&cache, "CMAKE_RC_COMPILER")?;
    let toolset = compiler.ancestors().find(|path| {
        path.parent()
            .and_then(Path::file_name)
            .is_some_and(|name| name.eq_ignore_ascii_case("MSVC"))
    })?;
    let sdk = resources.ancestors().find_map(|path| {
        path.file_name()
            .is_some_and(|name| name.eq_ignore_ascii_case("bin"))
            .then(|| path.parent())
            .flatten()
    })?;
    let mut roots = vec![toolset.join("include"), sdk.join("Include")];
    let atl = toolset.join("atlmfc/include");
    match std::fs::metadata(&atl) {
        Ok(metadata) if metadata.is_dir() => roots.push(atl),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
        _ => return None,
    }
    let mut inputs = vec![
        (
            super::normalize_root(&compiler),
            super::hash_file(compiler.to_str()?)?,
        ),
        (
            super::normalize_root(&resources),
            super::hash_file(resources.to_str()?)?,
        ),
    ];
    for (name, value) in environment {
        match value {
            Some(value) => {
                if matches!(*name, "INCLUDE" | "EXTERNAL_INCLUDE") {
                    for path in value.split(';').filter(|path| !path.trim().is_empty()) {
                        let path = PathBuf::from(path.trim().trim_matches('"'));
                        roots.push(if path.is_absolute() {
                            path
                        } else {
                            build.join(path)
                        });
                    }
                }
                inputs.push((format!("environment:{name}:present"), value.clone()));
            }
            None => inputs.push((format!("environment:{name}:absent"), String::new())),
        }
    }
    // Ninja intentionally omits installed system headers from its MSVC graph.
    // Hash their trees once, including SDK version directories that can change
    // implicit include selection, rather than trusting only recorded project headers.
    let selected = select_roots(roots);
    for root in selected {
        collect(&root, &mut inputs)?;
    }
    inputs.sort();
    Some(super::sha256_hex(&serde_json::to_vec(&inputs).ok()?))
}

fn select_roots(roots: Vec<PathBuf>) -> Vec<PathBuf> {
    let mut roots: Vec<PathBuf> = roots
        .into_iter()
        .map(|path| PathBuf::from(super::normalize_segments(&super::normalize_root(&path))))
        .collect();
    roots.sort_by_key(|path| super::normalize_root(path).to_ascii_lowercase());
    let mut selected: Vec<PathBuf> = Vec::new();
    for root in roots {
        let identity = super::normalize_root(&root).to_ascii_lowercase();
        if selected.iter().any(|parent| {
            let parent = super::normalize_root(parent).to_ascii_lowercase();
            identity == parent || identity.starts_with(&format!("{parent}/"))
        }) {
            continue;
        }
        selected.push(root);
    }
    selected
}

fn cache_path(cache: &str, name: &str) -> Option<PathBuf> {
    let prefix = format!("{name}:FILEPATH=");
    let path = cache.lines().find_map(|line| line.strip_prefix(&prefix))?;
    (!path.is_empty()).then(|| PathBuf::from(path))
}

fn collect(root: &Path, inputs: &mut Vec<(String, String)>) -> Option<()> {
    let metadata = std::fs::symlink_metadata(root).ok()?;
    if !metadata.is_dir() || metadata.file_type().is_symlink() {
        return None;
    }
    inputs.push((super::normalize_root(root), "directory".into()));
    for entry in std::fs::read_dir(root).ok()? {
        let entry = entry.ok()?;
        let kind = entry.file_type().ok()?;
        let path = entry.path();
        if kind.is_symlink() {
            return None;
        }
        if kind.is_dir() {
            collect(&path, inputs)?;
        } else if kind.is_file() {
            inputs.push((
                super::normalize_root(&path),
                super::hash_file(path.to_str()?)?,
            ));
        } else {
            return None;
        }
    }
    Some(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fixture_hash(build: &Path) -> Option<String> {
        let include = build.parent()?.join("VC/Tools/MSVC/14.44/include");
        hash_with_environment(
            build,
            &[("INCLUDE", Some(super::super::normalize_root(&include)))],
        )
    }

    #[test]
    fn implicit_include_selection_requires_an_explicit_environment() {
        let dir = fixture();
        assert!(hash_with_environment(&dir.path().join("build"), &[]).is_none());
    }

    #[test]
    fn parent_components_cannot_hide_an_include_directory() {
        let roots = select_roots(vec![
            PathBuf::from("C:/toolset/include"),
            PathBuf::from("C:/toolset/include/../extra"),
        ]);
        assert_eq!(roots.len(), 2);
    }

    fn fixture() -> tempfile::TempDir {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path();
        let compiler = root.join("VC/Tools/MSVC/14.44/bin/Hostx64/x64/cl.exe");
        let rc = root.join("Windows Kits/10/bin/10.0.26100.0/x64/rc.exe");
        for (path, content) in [
            (compiler.clone(), "compiler"),
            (rc.clone(), "resources"),
            (
                root.join("VC/Tools/MSVC/14.44/include/vector"),
                "bounds implementation",
            ),
            (
                root.join("Windows Kits/10/Include/10.0.26100.0/shared/sdk.h"),
                "SDK declarations",
            ),
        ] {
            std::fs::create_dir_all(path.parent().unwrap()).unwrap();
            std::fs::write(path, content).unwrap();
        }
        std::fs::create_dir_all(root.join("build")).unwrap();
        std::fs::write(
            root.join("build/CMakeCache.txt"),
            format!(
                "CMAKE_CXX_COMPILER:FILEPATH={}\nCMAKE_RC_COMPILER:FILEPATH={}\n",
                compiler.display(),
                rc.display()
            ),
        )
        .unwrap();
        dir
    }

    #[test]
    fn compiler_stl_and_sdk_content_changes_invalidate_reuse() {
        for file in [
            "VC/Tools/MSVC/14.44/bin/Hostx64/x64/cl.exe",
            "VC/Tools/MSVC/14.44/include/vector",
            "Windows Kits/10/Include/10.0.26100.0/shared/sdk.h",
        ] {
            let dir = fixture();
            let before = fixture_hash(&dir.path().join("build")).unwrap();
            std::fs::write(dir.path().join(file), "changed bytes").unwrap();
            assert_ne!(
                before,
                fixture_hash(&dir.path().join("build")).unwrap(),
                "{file}"
            );
        }
    }

    #[test]
    fn new_header_or_sdk_version_changes_identity() {
        let dir = fixture();
        let build = dir.path().join("build");
        let before = fixture_hash(&build).unwrap();
        let added = dir
            .path()
            .join("Windows Kits/10/Include/10.0.28000.0/shared/new.h");
        std::fs::create_dir_all(added.parent().unwrap()).unwrap();
        std::fs::write(&added, "new SDK").unwrap();
        assert_ne!(before, fixture_hash(&build).unwrap());
        std::fs::remove_file(&added).unwrap();
        assert_ne!(
            before,
            fixture_hash(&build).unwrap(),
            "empty SDK directory is an input too"
        );
    }

    #[test]
    fn missing_sdk_input_disables_cache_identity() {
        let dir = fixture();
        std::fs::rename(
            dir.path().join("Windows Kits/10/Include"),
            dir.path().join("unavailable"),
        )
        .unwrap();
        assert!(fixture_hash(&dir.path().join("build")).is_none());
    }
}
