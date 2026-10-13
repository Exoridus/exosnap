//! Identity and checkout ownership of repository headers.

use std::collections::{BTreeSet, HashMap};
use std::path::{Path, PathBuf};

pub(super) struct CheckoutRoot {
    canonical: String,
    lexical: String,
    components: Vec<(String, String)>,
}

impl CheckoutRoot {
    fn read(path: &Path) -> Option<Self> {
        let resolved = std::fs::canonicalize(path).ok()?;
        if !resolved.is_dir() {
            return None;
        }
        let canonical = checkout_name(&resolved)?;
        let lexical = checkout_name(path)?;
        let short = checkout_name(&short_root(&resolved)?)?;
        let long: Vec<_> = canonical.split('/').collect();
        let short: Vec<_> = short.split('/').collect();
        if long.len() != short.len() {
            return None;
        }
        let components = long
            .into_iter()
            .zip(short)
            .map(|(long, short)| (long.to_string(), short.to_string()))
            .collect();
        Some(Self {
            canonical: format!("{canonical}/"),
            lexical: format!("{lexical}/"),
            components,
        })
    }

    fn relative<'a>(&self, path: &'a str) -> Option<&'a str> {
        if let Some(relative) = path
            .strip_prefix(&self.lexical)
            .or_else(|| path.strip_prefix(&self.canonical))
        {
            return Some(relative);
        }
        // Windows can mix short and long names within one path. Match only
        // names obtained from the physical root, without probing every header.
        let mut relative = path;
        for (long, short) in &self.components {
            let (component, rest) = relative.split_once('/')?;
            if component != long && component != short {
                return None;
            }
            relative = rest;
        }
        Some(relative)
    }
}

fn checkout_name(path: &Path) -> Option<String> {
    let path = path.to_str()?.replace('\\', "/");
    let path = if let Some(unc) = path.strip_prefix("//?/UNC/") {
        format!("//{unc}")
    } else {
        path.strip_prefix("//?/").unwrap_or(&path).to_string()
    };
    Some(
        super::normalize_segments(&path)
            .trim_end_matches('/')
            .to_ascii_lowercase(),
    )
}

#[cfg(windows)]
fn short_root(path: &Path) -> Option<PathBuf> {
    use std::os::windows::ffi::OsStrExt as _;
    use windows::Win32::Storage::FileSystem::GetShortPathNameW;
    use windows::core::PCWSTR;
    let wide: Vec<u16> = path
        .as_os_str()
        .encode_wide()
        .chain(std::iter::once(0))
        .collect();
    let capacity = unsafe { GetShortPathNameW(PCWSTR(wide.as_ptr()), None) };
    if capacity == 0 {
        return None;
    }
    let mut output = vec![0u16; capacity as usize];
    let copied = unsafe { GetShortPathNameW(PCWSTR(wide.as_ptr()), Some(&mut output)) } as usize;
    if copied == 0 || copied >= output.len() {
        return None;
    }
    Some(PathBuf::from(String::from_utf16(&output[..copied]).ok()?))
}

#[cfg(not(windows))]
fn short_root(path: &Path) -> Option<PathBuf> {
    Some(path.to_path_buf())
}

pub(super) struct HeaderInventory {
    root: CheckoutRoot,
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
            root: CheckoutRoot::read(repo)?,
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

    pub(super) fn repository_relative(&self, path: &str) -> Option<String> {
        let path = checkout_name(Path::new(path))?;
        self.root.relative(&path).map(str::to_string)
    }

    fn ambiguous_header(&self, path: &str) -> bool {
        let Some(path) = checkout_name(Path::new(path)) else {
            return true;
        };
        if self.root.relative(&path).is_some() {
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

pub(super) fn foreign_roots(repo: &Path) -> Option<Vec<CheckoutRoot>> {
    let (code, output) = crate::git::Git::new(repo).run(&["worktree", "list", "--porcelain"]);
    if code != 0 {
        return None;
    }
    let current = format!("{}/", checkout_name(&std::fs::canonicalize(repo).ok()?)?);
    let roots: Vec<CheckoutRoot> = output
        .lines()
        .filter_map(|line| line.strip_prefix("worktree "))
        .map(|path| CheckoutRoot::read(Path::new(path)))
        .collect::<Option<_>>()?;
    if !roots.iter().any(|root| root.canonical == current) {
        return None;
    }
    Some(
        roots
            .into_iter()
            .filter(|root| root.canonical != current)
            .collect(),
    )
}

pub(super) fn retain_dependencies(
    graph: &mut HashMap<String, Vec<String>>,
    foreign: Option<&[CheckoutRoot]>,
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
            let Some(path) = checkout_name(Path::new(path)) else {
                return false;
            };
            !foreign.iter().any(|root| {
                let Some(relative) = root.relative(&path) else {
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
    fn a_physical_current_root_alias_preserves_dependency_ownership() {
        let dir = crate::test_support::fixture_repo_committed(&[("libs/a.h", "int a;\n")]);
        let alias = std::fs::canonicalize(dir.path()).unwrap();
        let roots =
            foreign_roots(&alias).expect("Git's listed root must identify the physical checkout");
        assert!(roots.is_empty());
        let inventory = HeaderInventory::read(&alias, &alias.join("build")).unwrap();
        let mut graph = HashMap::from([(
            "source.cpp".into(),
            vec![super::super::normalize_root(&dir.path().join("libs/a.h"))],
        )]);
        retain_dependencies(&mut graph, Some(&roots), Some(&inventory));
        assert_eq!(
            graph.len(),
            1,
            "a different spelling of the current root is not a foreign checkout"
        );
    }

    #[test]
    fn mixed_short_and_long_root_components_require_the_proven_names() {
        let root = CheckoutRoot {
            canonical: "c:/users/runneradmin/checkout directory/".into(),
            lexical: "c:/users/runneradmin/checkout directory/".into(),
            components: vec![
                ("c:".into(), "c:".into()),
                ("users".into(), "users".into()),
                ("runneradmin".into(), "runner~1".into()),
                ("checkout directory".into(), "checko~1".into()),
            ],
        };
        for path in [
            "c:/users/runner~1/checkout directory/libs/a.h",
            "c:/users/runneradmin/checko~1/libs/a.h",
            "c:/users/runner~1/checko~1/libs/a.h",
        ] {
            assert_eq!(root.relative(path), Some("libs/a.h"));
        }
        for path in [
            "c:/users/runner~2/checko~1/libs/a.h",
            "c:/users/runner~1/checko~10/libs/a.h",
            "c:/users/runner~1/checkout directory suffix/libs/a.h",
        ] {
            assert!(root.relative(path).is_none());
        }
    }

    #[cfg(windows)]
    #[test]
    fn native_root_aliases_accept_current_headers_and_reject_foreign_only_headers() {
        let dir = crate::test_support::fixture_repo_committed(&[("libs/a.h", "int a;\n")]);
        let foreign = dir.path().join("other checkout with long name");
        assert_eq!(
            crate::git::Git::new(dir.path())
                .run(&[
                    "worktree",
                    "add",
                    "--detach",
                    foreign.to_str().unwrap(),
                    "HEAD"
                ])
                .0,
            0
        );
        std::fs::write(foreign.join("libs/foreign_only.h"), "int foreign;\n").unwrap();
        let aliases = native_aliases(dir.path());
        let foreign_aliases = native_aliases(&foreign);
        for alias in aliases {
            let roots = foreign_roots(&alias)
                .expect("current root must match Git through its physical identity");
            assert_eq!(roots.len(), 1);
            let inventory = HeaderInventory::read(&alias, &alias.join("build")).unwrap();
            for current in native_aliases(dir.path()) {
                let mut graph = HashMap::from([(
                    "source.cpp".into(),
                    vec![super::super::normalize_root(&current.join("libs/a.h"))],
                )]);
                retain_dependencies(&mut graph, Some(&roots), Some(&inventory));
                assert_eq!(graph.len(), 1);
            }
            for foreign in &foreign_aliases {
                let mut graph = HashMap::from([(
                    "source.cpp".into(),
                    vec![super::super::normalize_root(
                        &foreign.join("libs/foreign_only.h"),
                    )],
                )]);
                retain_dependencies(&mut graph, Some(&roots), Some(&inventory));
                assert!(
                    graph.is_empty(),
                    "foreign header absent from the current inventory must still be rejected"
                );
            }
            let mut graph = HashMap::from([(
                "source.cpp".into(),
                vec![super::super::normalize_root(
                    &foreign.join("build/borrowed_deps/include/vendor.h"),
                )],
            )]);
            retain_dependencies(&mut graph, Some(&roots), Some(&inventory));
            assert_eq!(graph.len(), 1);
        }
    }

    #[cfg(windows)]
    fn native_aliases(path: &Path) -> Vec<PathBuf> {
        let canonical = std::fs::canonicalize(path).unwrap();
        let short = short_root(&canonical).unwrap();
        let long_name = checkout_name(&canonical).unwrap();
        let short_name = checkout_name(&short).unwrap();
        let mut aliases = BTreeSet::from([path.to_path_buf(), canonical, short]);
        let components: Vec<_> = long_name.split('/').collect();
        let mut short_components = 0;
        for (index, short) in short_name.split('/').enumerate() {
            if short != components[index] {
                short_components += 1;
                let mut mixed = components.clone();
                mixed[index] = short;
                aliases.insert(PathBuf::from(mixed.join("/")));
            }
        }
        eprintln!(
            "native checkout aliases: {} spellings, {short_components} short-name components",
            aliases.len()
        );
        for alias in &aliases {
            assert!(alias.is_dir());
        }
        aliases.into_iter().collect()
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
