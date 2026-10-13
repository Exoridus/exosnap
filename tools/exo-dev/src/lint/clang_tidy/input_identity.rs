//! Identity and checkout ownership of repository headers.

use std::collections::{BTreeSet, HashMap};
use std::path::Path;

pub(super) struct HeaderInventory {
    root: String,
    suffixes: HashMap<String, Vec<String>>,
    hash: String,
}

impl HeaderInventory {
    pub(super) fn read(repo: &Path, build: &Path) -> Option<Self> {
        let mut pending = Vec::new();
        for name in ["app", "apps", "libs", "tools", "tests", "third_party"] {
            let path = repo.join(name);
            match std::fs::metadata(&path) {
                Ok(metadata) if metadata.is_dir() => pending.push(path),
                Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
                _ => return None,
            }
        }
        let mut names = BTreeSet::new();
        while let Some(directory) = pending.pop() {
            for entry in std::fs::read_dir(directory).ok()? {
                let entry = entry.ok()?;
                let path = entry.path();
                if path == build || path == repo.join("tools/target") {
                    continue;
                }
                let kind = entry.file_type().ok()?;
                if kind.is_symlink() {
                    return None;
                }
                if kind.is_dir() {
                    pending.push(path);
                } else if kind.is_file() {
                    let relative = path.strip_prefix(repo).ok()?.to_str()?.replace('\\', "/");
                    if super::is_header(&relative) {
                        names.insert(relative);
                    }
                }
            }
        }
        let hash = super::sha256_hex(&serde_json::to_vec(&names).ok()?);
        let mut suffixes: HashMap<String, Vec<String>> = HashMap::new();
        for name in names {
            let relative = name.to_ascii_lowercase();
            suffixes
                .entry(relative.rsplit('/').next()?.to_string())
                .or_default()
                .push(format!("/{relative}"));
        }
        Some(Self {
            root: format!("{}/", super::normalize_root(repo).to_ascii_lowercase()),
            suffixes,
            hash,
        })
    }

    pub(super) fn configuration_hash(&self, repo: &Path) -> Option<String> {
        let config = super::hash_file(&repo.join(".clang-tidy").to_string_lossy())?;
        Some(super::sha256_hex(
            format!(".clang-tidy={config}\nheaders={}", self.hash).as_bytes(),
        ))
    }

    fn ambiguous_header(&self, path: &str) -> bool {
        let path = path.replace('\\', "/").to_ascii_lowercase();
        if path.starts_with(&self.root) {
            return false;
        }
        self.suffixes
            .get(path.rsplit('/').next().unwrap_or_default())
            .is_some_and(|suffixes| suffixes.iter().any(|suffix| path.ends_with(suffix)))
    }
}

#[cfg(test)]
pub(super) fn configuration_hash(repo: &Path, build: &Path) -> Option<String> {
    HeaderInventory::read(repo, build)?.configuration_hash(repo)
}

pub(super) fn foreign_roots(repo: &Path) -> Option<Vec<String>> {
    let (code, output) = crate::git::Git::new(repo).run(&["worktree", "list", "--porcelain"]);
    if code != 0 {
        return None;
    }
    let current = super::normalize_segments(&super::normalize_root(repo));
    let roots: Vec<String> = output
        .lines()
        .filter_map(|line| line.strip_prefix("worktree "))
        .map(|path| super::normalize_segments(&path.replace('\\', "/")))
        .collect();
    if !roots.iter().any(|root| root.eq_ignore_ascii_case(&current)) {
        return None;
    }
    Some(
        roots
            .into_iter()
            .filter(|root| !root.eq_ignore_ascii_case(&current))
            .collect(),
    )
}

pub(super) fn retain_dependencies(
    graph: &mut HashMap<String, Vec<String>>,
    foreign: Option<&[String]>,
    inventory: Option<&HeaderInventory>,
) {
    let (Some(foreign), Some(inventory)) = (foreign, inventory) else {
        graph.clear();
        return;
    };
    graph.retain(|_, dependencies| {
        dependencies.iter().all(|path| {
            if inventory.ambiguous_header(path) {
                return false;
            }
            let path = path.replace('\\', "/").to_ascii_lowercase();
            !foreign.iter().any(|root| {
                let prefix = format!("{}/", root.to_ascii_lowercase());
                let Some(relative) = path.strip_prefix(&prefix) else {
                    return false;
                };
                matches!(
                    relative.split('/').next().unwrap_or_default(),
                    "app" | "apps" | "libs" | "tools" | "tests" | "third_party"
                ) && super::is_header(relative)
            })
        })
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn full_runs_include_untracked_header_membership_in_the_global_identity() {
        let dir = tempfile::tempdir().unwrap();
        std::fs::create_dir_all(dir.path().join("libs")).unwrap();
        std::fs::write(dir.path().join(".clang-tidy"), "Checks: '-*'\n").unwrap();
        let build = dir.path().join("build");
        let before = configuration_hash(dir.path(), &build).unwrap();
        std::fs::write(dir.path().join("libs/shadow.h"), "int shadow;\n").unwrap();
        assert_ne!(before, configuration_hash(dir.path(), &build).unwrap());
    }

    #[test]
    fn registered_checkout_headers_are_rejected_but_external_borrowed_inputs_remain_valid() {
        let dir = crate::test_support::fixture_repo_committed(&[("libs/a.h", "int a;\n")]);
        let other = dir.path().join("other");
        assert_eq!(
            crate::git::Git::new(dir.path())
                .run(&[
                    "worktree",
                    "add",
                    "--detach",
                    other.to_str().unwrap(),
                    "HEAD"
                ])
                .0,
            0
        );
        let roots = foreign_roots(dir.path()).unwrap();
        let inventory = HeaderInventory::read(dir.path(), &dir.path().join("build")).unwrap();
        let source = format!("{}/libs/a.cpp", super::super::normalize_root(dir.path()));
        let mut graph = HashMap::from([(
            source.clone(),
            vec![super::super::normalize_root(&other.join("libs/a.h"))],
        )]);
        retain_dependencies(&mut graph, Some(&roots), Some(&inventory));
        assert!(graph.is_empty());
        for path in [
            dir.path().join("libs/a.h"),
            other.join("build/borrowed_deps/include/vendor.h"),
        ] {
            let mut graph =
                HashMap::from([(source.clone(), vec![super::super::normalize_root(&path)])]);
            retain_dependencies(&mut graph, Some(&roots), Some(&inventory));
            assert!(graph.contains_key(&source));
        }
    }

    #[test]
    fn unknown_checkout_ownership_disables_dependency_reuse() {
        let mut graph = HashMap::from([("source.cpp".into(), vec!["header.h".into()])]);
        retain_dependencies(&mut graph, None, None);
        assert!(graph.is_empty());
    }

    #[test]
    fn optional_repository_vendor_headers_change_membership_without_changing_ownership() {
        let dir = tempfile::tempdir().unwrap();
        std::fs::create_dir_all(dir.path().join("third_party/nvidia")).unwrap();
        std::fs::write(dir.path().join(".clang-tidy"), "Checks: '-*'\n").unwrap();
        let build = dir.path().join("build");
        let before = configuration_hash(dir.path(), &build).unwrap();
        std::fs::write(
            dir.path().join("third_party/nvidia/nvEncodeAPI.h"),
            "int api;\n",
        )
        .unwrap();
        assert_ne!(before, configuration_hash(dir.path(), &build).unwrap());
    }

    #[test]
    fn unregistered_checkout_header_suffixes_are_ambiguous_but_sdk_inputs_are_valid() {
        let dir =
            crate::test_support::fixture_repo_committed(&[("libs/engine/detail/a.h", "int a;\n")]);
        let inventory = HeaderInventory::read(dir.path(), &dir.path().join("build")).unwrap();
        let mut graph = HashMap::from([(
            "source.cpp".into(),
            vec!["C:/unregistered/libs/engine/detail/a.h".into()],
        )]);
        retain_dependencies(&mut graph, Some(&[]), Some(&inventory));
        assert!(graph.is_empty());
        let mut graph = HashMap::from([("source.cpp".into(), vec!["C:/sdk/include/a.h".into()])]);
        retain_dependencies(&mut graph, Some(&[]), Some(&inventory));
        assert_eq!(graph.len(), 1);
    }
}
