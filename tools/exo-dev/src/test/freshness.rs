//! Whether a build tree that this run did not build matches the source.
//!
//! A build by the run settles the question: its exit code is the evidence.
//! Without one, only a Ninja tree can even be asked, through its own dry run.
//! A timestamp comparison is not an answer: a copied or restored tree keeps
//! the mtimes it was created with, so a stale tree can look newer than the
//! source it is missing.

use std::path::Path;
use std::sync::LazyLock;

use regex::Regex;

use super::receipt::Freshness;

/// A `NAME:TYPE=value` entry of a CMake cache, first occurrence.
pub fn cache_value(cache: &Path, name: &str) -> Option<String> {
    let prefix = format!("{name}:");
    crate::process::read_lines(cache)
        .into_iter()
        .find_map(|line| {
            let rest = line.strip_prefix(&prefix)?;
            let (kind, value) = rest.split_once('=')?;
            let word = !kind.is_empty() && kind.chars().all(|c| c.is_alphanumeric() || c == '_');
            word.then(|| value.to_string())
        })
}

/// The verdict of `ninja -n` output (stdout and stderr together) for a graph
/// ninja could evaluate.
pub fn dry_run_verdict(text: &str) -> (Freshness, String) {
    // Everything after a pending regeneration comes out of a build.ninja the
    // regeneration would rewrite, so ninja lists the regeneration steps and
    // stops. CMake's glob check is dirty on every invocation of a
    // CONFIGURE_DEPENDS tree by construction, so this is the answer on every
    // run of such a tree, and it does not mean "nothing to do".
    if text.contains("Re-running CMake") {
        return (
            Freshness::Unknown,
            "ninja cannot see past the pending CMake regeneration".into(),
        );
    }
    static HOUSEKEEPING: LazyLock<Regex> = LazyLock::new(|| {
        Regex::new(r"(?i)Re-checking globbed directories|Entering directory|no work to do").unwrap()
    });
    let pending = text
        .lines()
        .filter(|line| !line.trim().is_empty() && !HOUSEKEEPING.is_match(line))
        .count();
    if pending == 0 {
        (
            Freshness::Fresh,
            "ninja has no build step left that would change these binaries".into(),
        )
    } else {
        (
            Freshness::Stale,
            format!(
                "ninja would run {pending} build step(s) before these binaries match the source"
            ),
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_cache_entry_is_read_by_name_and_type() {
        let dir = tempfile::tempdir().unwrap();
        let cache = dir.path().join("CMakeCache.txt");
        std::fs::write(
            &cache,
            "//comment\nCMAKE_MAKE_PROGRAM:FILEPATH=/usr/bin/ninja\n\
             CMAKE_MAKE_PROGRAM-ADVANCED:INTERNAL=1\nCMAKE_GENERATOR:INTERNAL=Ninja\n\
             CMAKE_GENERATOR_INSTANCE:INTERNAL=\n",
        )
        .unwrap();
        assert_eq!(
            cache_value(&cache, "CMAKE_GENERATOR").as_deref(),
            Some("Ninja")
        );
        assert_eq!(
            cache_value(&cache, "CMAKE_MAKE_PROGRAM").as_deref(),
            Some("/usr/bin/ninja")
        );
        assert_eq!(cache_value(&cache, "MISSING"), None);
        assert_eq!(
            cache_value(&dir.path().join("none"), "CMAKE_GENERATOR"),
            None
        );
    }

    #[test]
    fn housekeeping_alone_is_fresh() {
        let (freshness, _) = dry_run_verdict(
            "ninja: Entering directory `build'\n[1/1] Re-checking globbed directories...\nninja: no work to do.\n",
        );
        assert_eq!(freshness, Freshness::Fresh);
    }

    #[test]
    fn a_pending_compile_is_stale_and_counted() {
        let (freshness, detail) = dry_run_verdict(
            "ninja: Entering directory `build'\n[1/2] Building CXX object a.obj\n[2/2] Linking CXX executable a.exe\n",
        );
        assert_eq!(freshness, Freshness::Stale);
        assert!(detail.contains("2 build step(s)"), "{detail}");
    }

    #[test]
    fn a_pending_regeneration_is_unknown_not_fresh() {
        let (freshness, _) = dry_run_verdict(
            "[1/2] Re-checking globbed directories...\n[2/2] Re-running CMake...\n",
        );
        assert_eq!(freshness, Freshness::Unknown);
    }
}
