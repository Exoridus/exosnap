//! The identity of every third-party component that reaches a shipped
//! ExoSnap binary.
//!
//! The release manifest already names every file in the package with its
//! SHA-256. That answers which bytes shipped, not which upstream sources
//! those bytes were built from, and a statically linked library leaves no
//! file behind to hash at all.
//!
//! This module answers the second question from the files that already pin
//! the dependencies, so it adds no second authority: `third_party/CMakeLists.txt`
//! for the FetchContent pins, `cmake/VendorFFmpeg.cmake` for the prebuilt
//! FFmpeg archive, `.qt-version` for Qt, and the vendored source trees
//! themselves for the amalgamations that carry no version string.
//!
//! The release packaging path does not call `derive` yet: the dependency
//! inventory it should join is still assembled by
//! `scripts/build-release-artifacts.ps1`, so this is a parallel, currently
//! unwired implementation kept alongside that script's own.

#![cfg_attr(
    not(test),
    allow(
        dead_code,
        reason = "the release packaging path does not consume the dependency inventory yet"
    )
)]

use anyhow::{Result, bail};
use std::collections::BTreeMap;
use std::path::Path;
use std::sync::LazyLock;

use crate::bundle::sha256_bytes;
use crate::package::walk_files;

/// One third-party component's identity, with whichever fields its pin
/// provides.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DependencyEntry {
    pub name: String,
    pub linkage: String,
    pub version: Option<String>,
    pub source: Option<String>,
    pub revision: Option<String>,
    pub archive_sha256: Option<String>,
    pub upstream_version: Option<String>,
    pub library_majors: Option<BTreeMap<String, u32>>,
    pub source_sha256: Option<String>,
}

impl DependencyEntry {
    fn named(name: &str, linkage: &str) -> DependencyEntry {
        DependencyEntry {
            name: name.to_string(),
            linkage: linkage.to_string(),
            version: None,
            source: None,
            revision: None,
            archive_sha256: None,
            upstream_version: None,
            library_majors: None,
            source_sha256: None,
        }
    }
}

/// One third-party component that reaches a shipped binary, with how it
/// gets there.
pub struct DependencyInventory {
    pub entries: Vec<DependencyEntry>,
}

impl DependencyInventory {
    pub fn find(&self, name: &str) -> Option<&DependencyEntry> {
        self.entries.iter().find(|entry| entry.name == name)
    }
}

// googletest is deliberately absent: it links into test executables only and
// never into the package.
const EXPECTED_FETCH_CONTENT: &[(&str, &str)] = &[
    ("spdlog", "static"),
    ("nlohmann_json", "header-only"),
    ("tomlplusplus", "header-only"),
    ("libopus", "static"),
    ("flac", "static"),
    ("rnnoise", "static"),
    ("EBML", "static"),
    ("libmatroska", "static"),
    ("presentmon", "static"),
];

// Amalgamations vendored into the tree. They have no upstream pin to read,
// so their identity is the hash of the sources actually compiled, plus the
// release THIRD_PARTY_NOTICES.md names them against.
const EXPECTED_VENDORED: &[(&str, &str, &str)] = &[
    (
        "miniz",
        "libs/update/third_party/miniz",
        "static (vendored amalgamation)",
    ),
    (
        "monocypher",
        "libs/update/third_party/monocypher",
        "static (vendored amalgamation)",
    ),
    (
        "nvEncodeAPI",
        "third_party/nvidia",
        "header only (NVENC entry points, resolved at runtime)",
    ),
    ("fonts", "app/assets/fonts", "compiled into the Qt resource"),
];

const FFMPEG_LIBRARY_MAJORS: &[&str] = &["AVFORMAT", "AVCODEC", "AVUTIL", "SWRESAMPLE"];

/// One `FetchContent_Declare` block's pinned identity.
#[derive(Debug, Clone, PartialEq, Eq)]
struct FetchContentPin {
    source: String,
    revision: String,
    version: Option<String>,
}

/// Reads the `FetchContent_Declare(<name> ...)` block for `name` out of
/// `text`, matched as one block so a `GIT_TAG` belonging to the next
/// component can never be read as this one's.
fn fetch_content_pin(text: &str, name: &str) -> Option<FetchContentPin> {
    let declare = Regex::new(&format!(
        r"(?is)FetchContent_Declare\(\s*{}\s(.*?)\)",
        regex::escape(name)
    ))
    .expect("valid FetchContent_Declare pattern");
    let block = declare.captures(text)?;
    let body = block.get(1)?.as_str();

    static REPOSITORY: LazyLock<Regex> =
        LazyLock::new(|| Regex::new(r"(?i)GIT_REPOSITORY\s+(\S+)").unwrap());
    static REVISION: LazyLock<Regex> = LazyLock::new(|| {
        Regex::new(r"(?i)GIT_TAG\s+([0-9a-f]{7,40})\s*(?:#\s*(?:==\s*)?(?:tag\s+)?(\S+))?").unwrap()
    });

    let repository = REPOSITORY.captures(body)?;
    let revision = REVISION.captures(body)?;
    Some(FetchContentPin {
        source: repository[1].to_string(),
        revision: revision[1].to_string(),
        version: revision.get(2).map(|m| m.as_str().to_string()),
    })
}

/// One hash over the sorted relative paths and their contents: a renamed
/// file has to change the digest, or the digest would not identify the
/// tree.
fn vendored_source_hash(directory: &Path) -> Result<String> {
    // PowerShell's default `Sort-Object` is case-insensitive, unlike a plain
    // byte comparison of paths. Matching it keeps this hash identical to the
    // one `scripts/lib/DependencyIdentity.psm1` records for the same tree.
    let mut relatives: Vec<(String, std::path::PathBuf)> = walk_files(directory)?
        .into_iter()
        .map(|path| {
            let relative = path
                .strip_prefix(directory)
                .unwrap_or(&path)
                .to_string_lossy()
                .replace('\\', "/");
            (relative, path)
        })
        .collect();
    relatives.sort_by_key(|a| a.0.to_ascii_lowercase());

    let mut buffer = Vec::new();
    for (relative, path) in relatives {
        buffer.extend_from_slice(relative.as_bytes());
        buffer.push(b'\n');
        buffer.extend_from_slice(&std::fs::read(&path)?);
    }
    Ok(sha256_bytes(&buffer))
}

use regex::Regex;

/// Returns one entry per third-party component that reaches a shipped
/// binary.
///
/// Throws when an expected component has no readable pin. A release
/// manifest that silently dropped a dependency would be worse than no
/// dependency section at all.
pub fn derive(repo_root: &Path) -> Result<DependencyInventory> {
    let third_party_path = repo_root.join("third_party/CMakeLists.txt");
    let ffmpeg_path = repo_root.join("cmake/VendorFFmpeg.cmake");
    let qt_version_path = repo_root.join(".qt-version");
    for required in [&third_party_path, &ffmpeg_path, &qt_version_path] {
        if !required.is_file() {
            bail!("dependency identity: '{}' is missing", required.display());
        }
    }

    let mut entries = Vec::new();

    let qt_version = std::fs::read_to_string(&qt_version_path)?
        .trim()
        .to_string();
    static QT_VERSION: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"^\d+\.\d+\.\d+$").unwrap());
    if !QT_VERSION.is_match(&qt_version) {
        bail!(
            "dependency identity: .qt-version contains '{qt_version}', which is not a three-part version"
        );
    }
    entries.push(DependencyEntry {
        version: Some(qt_version),
        source: Some("https://download.qt.io/official_releases/qt/".to_string()),
        ..DependencyEntry::named("Qt", "dynamic")
    });

    let ffmpeg_text = std::fs::read_to_string(&ffmpeg_path)?;
    entries.push(ffmpeg_entry(&ffmpeg_text)?);

    let third_party_text = std::fs::read_to_string(&third_party_path)?;
    for (name, linkage) in EXPECTED_FETCH_CONTENT {
        let Some(pin) = fetch_content_pin(&third_party_text, name) else {
            bail!(
                "dependency identity: no readable FetchContent pin for '{name}' in third_party/CMakeLists.txt"
            );
        };
        entries.push(DependencyEntry {
            version: pin.version,
            source: Some(pin.source),
            revision: Some(pin.revision),
            ..DependencyEntry::named(name, linkage)
        });
    }

    for (name, relative, linkage) in EXPECTED_VENDORED {
        let directory = repo_root.join(relative);
        if !directory.is_dir() {
            bail!("dependency identity: vendored source tree '{relative}' is missing");
        }
        entries.push(DependencyEntry {
            source: Some((*relative).to_string()),
            source_sha256: Some(vendored_source_hash(&directory)?),
            ..DependencyEntry::named(name, linkage)
        });
    }

    Ok(DependencyInventory { entries })
}

/// The FFmpeg entry: the archive is pinned by tag and SHA-256 rather than by
/// a FetchContent GIT_TAG, so it is read separately from the other
/// components.
fn ffmpeg_entry(ffmpeg_text: &str) -> Result<DependencyEntry> {
    static TAG: LazyLock<Regex> = LazyLock::new(|| {
        Regex::new(r#"(?i)set\(\s*EXOSNAP_FFMPEG_PACKAGE_TAG\s+"([^"$]+)""#).unwrap()
    });
    static UPSTREAM: LazyLock<Regex> = LazyLock::new(|| {
        Regex::new(r#"(?i)set\(\s*EXOSNAP_FFMPEG_UPSTREAM_TAG\s+"([^"$]+)""#).unwrap()
    });
    static URL: LazyLock<Regex> =
        LazyLock::new(|| Regex::new(r#"(?im)^\s*URL\s+"([^"]+)""#).unwrap());
    static HASH: LazyLock<Regex> =
        LazyLock::new(|| Regex::new(r#"(?i)URL_HASH\s+"?SHA256=([0-9a-fA-F]{64})"?"#).unwrap());

    let tag = TAG.captures(ffmpeg_text);
    let upstream = UPSTREAM.captures(ffmpeg_text);
    let url = URL.captures(ffmpeg_text);
    let hash = HASH.captures(ffmpeg_text);
    let (Some(tag), Some(upstream), Some(url), Some(hash)) = (tag, upstream, url, hash) else {
        bail!(
            "dependency identity: cannot read the FFmpeg package tag, upstream tag, URL and SHA-256 from cmake/VendorFFmpeg.cmake"
        );
    };
    let tag = tag[1].to_string();
    let upstream = upstream[1].to_string();

    // The pin is expressed through CMake variables, so the URL literal in
    // the file still contains `${...}` references. Reading the literal
    // would record a placeholder as the shipped identity, which is worse
    // than recording nothing: it looks like an answer. Resolve the two
    // variables the URL is built from, then substitute them.
    let resolved_url = url[1]
        .replace("${EXOSNAP_FFMPEG_PACKAGE_TAG}", &tag)
        .replace("${EXOSNAP_FFMPEG_UPSTREAM_TAG}", &upstream);
    if resolved_url.contains("${") {
        bail!(
            "dependency identity: the FFmpeg URL still contains an unresolved variable after substitution: {resolved_url}"
        );
    }

    let mut majors = BTreeMap::new();
    for lib in FFMPEG_LIBRARY_MAJORS {
        let pattern = Regex::new(&format!(
            r"(?i)set\(\s*EXOSNAP_FFMPEG_{lib}_MAJOR\s+(\d+)\s*\)"
        ))
        .expect("valid FFmpeg major pattern");
        let Some(major) = pattern.captures(ffmpeg_text) else {
            bail!(
                "dependency identity: cmake/VendorFFmpeg.cmake declares no pinned major for {lib}"
            );
        };
        majors.insert(
            lib.to_ascii_lowercase(),
            major[1].parse().expect("digits matched by the pattern"),
        );
    }

    Ok(DependencyEntry {
        version: Some(tag),
        upstream_version: Some(upstream),
        source: Some(resolved_url),
        archive_sha256: Some(hash[1].to_ascii_lowercase()),
        library_majors: Some(majors),
        ..DependencyEntry::named("FFmpeg", "dynamic")
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    fn repo_root() -> std::path::PathBuf {
        Path::new(env!("CARGO_MANIFEST_DIR")).join("../..")
    }

    fn write(root: &Path, relative: &str, contents: &str) {
        let path = root.join(relative);
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        fs::write(path, contents).unwrap();
    }

    /// A minimal tree carrying the four inputs `derive` consults.
    fn fixture_root() -> tempfile::TempDir {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path();
        write(root, ".qt-version", "6.11.2\n");
        write(root, "libs/update/third_party/miniz/miniz.c", "int miniz;");
        write(
            root,
            "libs/update/third_party/monocypher/monocypher.c",
            "int mono;",
        );
        write(root, "third_party/nvidia/nvEncodeAPI.h", "#pragma once");
        write(root, "app/assets/fonts/font.txt", "font");
        write(
            root,
            "cmake/VendorFFmpeg.cmake",
            concat!(
                "set(EXOSNAP_FFMPEG_PACKAGE_TAG  \"n9.0.2-exosnap.1\")\n",
                "set(EXOSNAP_FFMPEG_UPSTREAM_TAG \"n9.0.2\")\n",
                "set(EXOSNAP_FFMPEG_AVFORMAT_MAJOR   63)\n",
                "set(EXOSNAP_FFMPEG_AVCODEC_MAJOR    63)\n",
                "set(EXOSNAP_FFMPEG_AVUTIL_MAJOR     61)\n",
                "set(EXOSNAP_FFMPEG_SWRESAMPLE_MAJOR 7)\n",
                "FetchContent_Declare(\n",
                "    ffmpeg_prebuilt\n",
                "    URL      \"https://example.invalid/${EXOSNAP_FFMPEG_PACKAGE_TAG}/ffmpeg-${EXOSNAP_FFMPEG_UPSTREAM_TAG}-win64-lgpl-shared.zip\"\n",
                "    URL_HASH \"SHA256=E895E66FC9CE1871ABC09A09FD9B99B663971ADFDD2BC1F10B119942E656AFCB\"\n",
                ")\n",
            ),
        );
        let declarations: String = [
            "spdlog",
            "nlohmann_json",
            "tomlplusplus",
            "libopus",
            "flac",
            "rnnoise",
            "EBML",
            "libmatroska",
            "presentmon",
        ]
        .iter()
        .map(|name| {
            format!(
                "FetchContent_Declare(\n  {name}\n  GIT_REPOSITORY https://example.invalid/{name}.git\n  GIT_TAG        0123456789abcdef0123456789abcdef01234567 # tag v1.2.3\n)\n"
            )
        })
        .collect();
        write(root, "third_party/CMakeLists.txt", &declarations);
        dir
    }

    #[test]
    fn every_expected_component_in_the_real_repository_has_a_readable_pin() {
        let inventory = derive(&repo_root()).unwrap();
        for expected in [
            "Qt",
            "FFmpeg",
            "spdlog",
            "nlohmann_json",
            "tomlplusplus",
            "libopus",
            "flac",
            "rnnoise",
            "EBML",
            "libmatroska",
            "presentmon",
            "miniz",
            "monocypher",
        ] {
            let entry = inventory
                .find(expected)
                .unwrap_or_else(|| panic!("'{expected}' is missing from the dependency identity"));
            assert!(entry.source.is_some(), "'{expected}' has no source");
        }
    }

    #[test]
    fn every_fetch_content_component_names_the_exact_revision_it_was_built_from() {
        let inventory = derive(&repo_root()).unwrap();
        static FULL_REVISION: LazyLock<Regex> =
            LazyLock::new(|| Regex::new(r"^[0-9a-f]{40}$").unwrap());
        for name in [
            "spdlog",
            "libopus",
            "flac",
            "rnnoise",
            "EBML",
            "libmatroska",
            "presentmon",
        ] {
            let entry = inventory.find(name).unwrap();
            let revision = entry.revision.as_deref().unwrap_or_default();
            assert!(
                FULL_REVISION.is_match(revision),
                "'{name}' does not carry a full commit revision; '{revision}' cannot identify a source tree"
            );
        }
    }

    #[test]
    fn the_ffmpeg_entry_carries_the_archive_hash_the_build_verifies_against() {
        let inventory = derive(&repo_root()).unwrap();
        let ffmpeg = inventory.find("FFmpeg").unwrap();
        let hash = ffmpeg.archive_sha256.as_deref().unwrap_or_default();
        assert_eq!(hash.len(), 64);
        assert!(hash.chars().all(|c| c.is_ascii_hexdigit()));
        let vendor_text = fs::read_to_string(repo_root().join("cmake/VendorFFmpeg.cmake")).unwrap();
        assert!(
            vendor_text.to_ascii_lowercase().contains(hash),
            "the recorded FFmpeg hash is not the one cmake/VendorFFmpeg.cmake pins"
        );
    }

    #[test]
    fn the_ffmpeg_source_is_recorded_resolved_never_as_a_cmake_placeholder() {
        let inventory = derive(&repo_root()).unwrap();
        let ffmpeg = inventory.find("FFmpeg").unwrap();
        let source = ffmpeg.source.as_deref().unwrap();
        let version = ffmpeg.version.as_deref().unwrap();
        let upstream_version = ffmpeg.upstream_version.as_deref().unwrap();
        assert!(
            !source.contains("${"),
            "the recorded URL still carries a placeholder: {source}"
        );
        assert!(
            !version.contains("${"),
            "the recorded version still carries a placeholder: {version}"
        );
        assert!(
            source.contains(version),
            "the resolved URL does not contain the package tag it was built from"
        );
        assert!(
            source.contains(upstream_version),
            "the resolved URL does not contain the upstream tag it was built from"
        );
    }

    #[test]
    fn a_url_whose_variables_cannot_be_resolved_is_refused() {
        let dir = fixture_root();
        let path = dir.path().join("cmake/VendorFFmpeg.cmake");
        let text = fs::read_to_string(&path)
            .unwrap()
            .replace("EXOSNAP_FFMPEG_UPSTREAM_TAG}", "SOMETHING_ELSE}");
        fs::write(&path, text).unwrap();
        assert!(
            derive(dir.path()).is_err(),
            "an unresolved variable was written into the recorded URL"
        );
    }

    #[test]
    fn a_missing_pinned_library_major_is_refused() {
        let dir = fixture_root();
        let path = dir.path().join("cmake/VendorFFmpeg.cmake");
        let text: String = fs::read_to_string(&path)
            .unwrap()
            .lines()
            .filter(|line| !line.starts_with("set(EXOSNAP_FFMPEG_AVCODEC_MAJOR"))
            .map(|line| format!("{line}\n"))
            .collect();
        fs::write(&path, text).unwrap();
        assert!(
            derive(dir.path()).is_err(),
            "a pin file with no avcodec major still produced a dependency identity"
        );
    }

    #[test]
    fn a_component_that_disappears_from_its_pin_file_is_refused_not_dropped() {
        let dir = fixture_root();
        let path = dir.path().join("third_party/CMakeLists.txt");
        let text = fs::read_to_string(&path).unwrap();
        let stripped = Regex::new(r"(?is)FetchContent_Declare\(\s*libopus\s.*?\)")
            .unwrap()
            .replace(&text, "")
            .to_string();
        assert!(
            !stripped.contains("libopus"),
            "the fixture still declares libopus; the case proves nothing"
        );
        fs::write(&path, stripped).unwrap();
        assert!(
            derive(dir.path()).is_err(),
            "a missing libopus pin produced a shorter list instead of a refusal"
        );
    }

    #[test]
    fn an_ffmpeg_pin_without_its_hash_is_refused() {
        let dir = fixture_root();
        let path = dir.path().join("cmake/VendorFFmpeg.cmake");
        let text = Regex::new(r"(?i)\s*URL_HASH[^\r\n]*")
            .unwrap()
            .replace(&fs::read_to_string(&path).unwrap(), "")
            .to_string();
        fs::write(&path, text).unwrap();
        assert!(
            derive(dir.path()).is_err(),
            "an unpinned FFmpeg archive was recorded as an identity"
        );
    }

    #[test]
    fn a_qt_version_that_is_not_three_parts_is_refused() {
        let dir = fixture_root();
        fs::write(dir.path().join(".qt-version"), "6.11\n").unwrap();
        assert!(
            derive(dir.path()).is_err(),
            "a two-part Qt version was accepted as an SDK identity"
        );
    }

    #[test]
    fn the_vendored_hash_follows_the_content_not_just_the_file_names() {
        let dir = fixture_root();
        let before = derive(dir.path())
            .unwrap()
            .find("miniz")
            .unwrap()
            .source_sha256
            .clone();
        let path = dir.path().join("libs/update/third_party/miniz/miniz.c");
        let mut text = fs::read_to_string(&path).unwrap();
        text.push_str("int added;");
        fs::write(&path, text).unwrap();
        let after = derive(dir.path())
            .unwrap()
            .find("miniz")
            .unwrap()
            .source_sha256
            .clone();
        assert_ne!(
            before, after,
            "an edited vendored source produced the same digest"
        );
    }

    #[test]
    fn the_vendored_hash_follows_the_file_names_not_just_the_content() {
        let dir = fixture_root();
        let before = derive(dir.path())
            .unwrap()
            .find("miniz")
            .unwrap()
            .source_sha256
            .clone();
        let old = dir.path().join("libs/update/third_party/miniz/miniz.c");
        let new = dir.path().join("libs/update/third_party/miniz/renamed.c");
        fs::rename(old, new).unwrap();
        let after = derive(dir.path())
            .unwrap()
            .find("miniz")
            .unwrap()
            .source_sha256
            .clone();
        assert_ne!(
            before, after,
            "a renamed vendored source produced the same digest"
        );
    }

    #[test]
    fn a_missing_vendored_tree_is_refused() {
        let dir = fixture_root();
        fs::remove_dir_all(dir.path().join("libs/update/third_party/monocypher")).unwrap();
        assert!(
            derive(dir.path()).is_err(),
            "a vendored component that is no longer in the tree was silently omitted"
        );
    }

    #[test]
    fn a_pin_block_never_borrows_the_revision_of_the_declaration_after_it() {
        let text = concat!(
            "FetchContent_Declare(\n",
            "  first\n",
            "  GIT_REPOSITORY https://example.invalid/first.git\n",
            ")\n",
            "FetchContent_Declare(\n",
            "  second\n",
            "  GIT_REPOSITORY https://example.invalid/second.git\n",
            "  GIT_TAG        aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa # tag v9.9.9\n",
            ")\n",
        );
        assert!(
            fetch_content_pin(text, "first").is_none(),
            "a declaration without a GIT_TAG was given the next declaration's revision"
        );
        assert_eq!(
            fetch_content_pin(text, "second").unwrap().revision,
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
            "the second declaration lost its own revision"
        );
    }
}
