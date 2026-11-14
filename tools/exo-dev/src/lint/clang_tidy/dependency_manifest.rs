//! Dependency manifests: the files clang itself opened for a fresh analysis.
//!
//! Every clang-tidy invocation asks clang to report each file its
//! preprocessor opens, including system, external and forced-include headers.
//! The report is a by-product of the analysis, not a second parse. The notes
//! are removed from the output before anything validates, caches or reports
//! it, so verdicts and cache keys do not depend on them.
//!
//! After a keyed unit's verdict passes its checkpoint revalidation, the files
//! clang opened for every compile command of that unit are hashed and stored
//! next to the verdict as `<key>.deps.json`. Manifests take no part in cache
//! keys or lookups yet. They record the inputs a precise key would have to
//! cover, and the profile compares them with the Ninja records the current
//! keys rely on.
//!
//! A manifest is written only when it is complete: every noted path and
//! forced include resolves, is readable, and was not modified after analysis
//! began. A probe for a missing file, such as `__has_include`, opens nothing.
//! Manifests therefore never describe header membership, which stays with the
//! global header inventory.

use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::io::Read as _;
use std::path::Path;
use std::sync::Mutex;
use std::time::{Duration, Instant, SystemTime};

use super::{CompileEntry, TidyUnitResult};

pub(super) const MANIFEST_SCHEMA: u32 = 1;

/// Makes clang print one `Note: including file:` stdout line per opened file,
/// system headers included. Diagnostics are unaffected, so these flags are
/// not part of the cache salt.
pub(super) const OBSERVATION_FLAGS: &[&str] = &[
    "--extra-arg=-Xclang",
    "--extra-arg=--show-includes",
    "--extra-arg=-Xclang",
    "--extra-arg=-sys-header-deps",
];

const INCLUDE_NOTE: &str = "Note: including file:";

/// Coarsest file-time resolution in use on Windows volumes (FAT). NTFS also
/// stamps files from a clock that can lag the precise system time.
const FILE_TIME_GRANULARITY: Duration = Duration::from_secs(2);

/// Removes clang's include notes from `output`. Returns the output exactly as
/// an invocation without [`OBSERVATION_FLAGS`] prints it, and the noted paths.
fn split_output(output: &str) -> (String, Vec<String>) {
    let mut verdict = String::with_capacity(output.len());
    let mut included = Vec::new();
    for line in output.split_inclusive('\n') {
        match line.strip_prefix(INCLUDE_NOTE) {
            Some(path) => included.push(
                path.trim_start_matches(' ')
                    .trim_end_matches(['\r', '\n'])
                    .to_string(),
            ),
            None => verdict.push_str(line),
        }
    }
    (verdict, included)
}

/// Include notes of fresh units, held from analysis until their checkpoint.
pub(super) struct Observed {
    enabled: bool,
    included: Mutex<HashMap<String, Vec<String>>>,
}

impl Observed {
    /// Notes are only retained when `enabled`, that is, when verdicts can be
    /// keyed and checkpointed.
    pub(super) fn new(enabled: bool) -> Self {
        Self {
            enabled,
            included: Mutex::default(),
        }
    }

    /// Returns `unit` with its include notes removed from the output.
    pub(super) fn strip(&self, (source, output, code, timing): TidyUnitResult) -> TidyUnitResult {
        let (verdict, included) = split_output(&output);
        if self.enabled {
            self.included
                .lock()
                .unwrap()
                .insert(source.clone(), included);
        }
        (source, verdict, code, timing)
    }

    pub(super) fn take(&self, source: &str) -> Option<Vec<String>> {
        self.included.lock().unwrap().remove(source)
    }
}

/// Per-root counts of files in one view but not the other.
#[derive(Clone, Debug, Default, serde::Serialize)]
pub struct RootCounts {
    pub repository: usize,
    pub build: usize,
    pub absolute: usize,
}

impl RootCounts {
    fn add(&mut self, root: &str) {
        match root {
            "repository" => self.repository += 1,
            "build" => self.build += 1,
            _ => self.absolute += 1,
        }
    }
}

/// Manifest recording and clang/Ninja comparison for one run.
///
/// `clang_only` counts, per compared unit, files clang opened that the
/// unit's Ninja record lacks. Ninja omits installed MSVC/SDK headers, so
/// `absolute` entries are expected there. `ninja_only` counts recorded files
/// clang did not open. The file sets list each distinct path and are omitted
/// from public profiles.
#[derive(Clone, Debug, Default, serde::Serialize)]
pub struct ManifestSummary {
    pub recorded: usize,
    pub observed_files: usize,
    pub incomplete: BTreeMap<&'static str, usize>,
    pub unkeyed: usize,
    pub write_failures: usize,
    pub compared_units: usize,
    pub clang_only: RootCounts,
    pub ninja_only: RootCounts,
    pub units_with_clang_only_project_files: usize,
    pub units_with_ninja_only_files: usize,
    pub clang_only_project_files: BTreeSet<String>,
    pub ninja_only_files: BTreeSet<String>,
    pub seconds: f64,
}

#[derive(serde::Serialize)]
struct Manifest<'a> {
    manifest_schema: u32,
    cache_schema: u32,
    verdict_key: &'a str,
    source: &'a str,
    tool_version: &'a str,
    observation: &'static [&'static str],
    compile_commands: usize,
    compile_commands_sha256: String,
    roots: Roots<'a>,
    files: Vec<ManifestFile>,
}

#[derive(serde::Serialize)]
struct Roots<'a> {
    repository: &'a str,
    build: &'a str,
}

#[derive(serde::Serialize)]
struct ManifestFile {
    root: &'static str,
    path: String,
    sha256: String,
    /// Physical path, when it differs from the opened path: a link or
    /// junction in the path, or an alias spelling of a component.
    #[serde(skip_serializing_if = "Option::is_none")]
    target: Option<String>,
    via: &'static str,
}

/// Opened files by lower-case path: spelling as opened, and how it was reached.
type Opened = BTreeMap<String, (String, &'static str)>;

pub(super) struct Recorder<'a> {
    repository: &'a str,
    build: &'a str,
    tool_version: &'a str,
    analysis_started: SystemTime,
    entries: HashMap<&'a str, Vec<&'a CompileEntry>>,
    identities: &'a HashMap<String, String>,
    pub(super) summary: ManifestSummary,
}

impl<'a> Recorder<'a> {
    /// `analysis_started` must precede every analysis whose notes are recorded.
    pub(super) fn new(
        repository: &'a str,
        build: &'a str,
        tool_version: &'a str,
        entries: &'a [CompileEntry],
        identities: &'a HashMap<String, String>,
        analysis_started: SystemTime,
    ) -> Self {
        let mut by_source: HashMap<&str, Vec<&CompileEntry>> = HashMap::new();
        for entry in entries {
            by_source.entry(&entry.file).or_default().push(entry);
        }
        Self {
            repository,
            build,
            tool_version,
            analysis_started,
            entries: by_source,
            identities,
            summary: ManifestSummary::default(),
        }
    }

    /// Records the manifest of a checkpointed unit and compares its opened
    /// files with `ninja`, the unit's recorded dependencies. Never fails the
    /// analysis: an incomplete or unwritable manifest is only counted.
    pub(super) fn record(
        &mut self,
        cache_dir: &Path,
        key: Option<&str>,
        source: &str,
        included: Option<Vec<String>>,
        ninja: Option<&[String]>,
    ) {
        let started = Instant::now();
        let opened = match included {
            Some(included) => self.opened(source, &included),
            None => Err("not_observed"),
        };
        if let (Ok(opened), Some(ninja)) = (&opened, ninja) {
            self.compare(source, opened, ninja);
        }
        match (key, opened) {
            (None, _) => self.summary.unkeyed += 1,
            (Some(key), opened) => {
                let written = opened
                    .and_then(|opened| self.manifest(key, source, &opened))
                    .map(|manifest| {
                        (
                            write_manifest(cache_dir, key, &manifest),
                            manifest.files.len(),
                        )
                    });
                match written {
                    Ok((true, files)) => {
                        self.summary.recorded += 1;
                        self.summary.observed_files += files;
                    }
                    Ok((false, _)) => self.summary.write_failures += 1,
                    Err(reason) => *self.summary.incomplete.entry(reason).or_default() += 1,
                }
            }
        }
        self.summary.seconds += started.elapsed().as_secs_f64();
    }

    /// The source, every noted file and every forced include of every
    /// compile command, as absolute normalized paths.
    fn opened(&self, source: &str, included: &[String]) -> Result<Opened, &'static str> {
        let entries = self.entries.get(source).ok_or("no_compile_command")?;
        if included.is_empty() {
            return Err("no_includes_observed");
        }
        let commands = entries
            .iter()
            .map(|entry| command_arguments(&entry.command_line).ok_or("unparsable_command"))
            .collect::<Result<Vec<_>, _>>()?;
        let directories: BTreeSet<&str> = commands
            .iter()
            .map(|(directory, _)| directory.as_str())
            .collect();
        let mut opened = Opened::new();
        opened.insert(source.to_ascii_lowercase(), (source.to_string(), "source"));
        for noted in included {
            if noted.is_empty() {
                return Err("malformed_include_note");
            }
            let path = resolve(noted, &directories).ok_or("ambiguous_relative_path")?;
            opened
                .entry(path.to_ascii_lowercase())
                .or_insert((path, "include"));
        }
        for (directory, arguments) in &commands {
            for forced in forced_includes(arguments)? {
                let path =
                    resolve(&forced, &BTreeSet::from([directory.as_str()])).unwrap_or_default();
                if !Path::new(&path).is_file() {
                    return Err("unresolved_forced_include");
                }
                opened
                    .entry(path.to_ascii_lowercase())
                    .and_modify(|(_, via)| *via = "forced")
                    .or_insert((path, "forced"));
            }
        }
        Ok(opened)
    }

    fn compare(&mut self, source: &str, opened: &Opened, ninja: &[String]) {
        let source = source.to_ascii_lowercase();
        let recorded: BTreeMap<String, &String> = ninja
            .iter()
            .map(|path| (path.to_ascii_lowercase(), path))
            .filter(|(lower, _)| *lower != source)
            .collect();
        let mut clang_only_project = false;
        for (lower, (path, _)) in opened {
            if *lower == source || recorded.contains_key(lower) {
                continue;
            }
            let (root, relative) = self.locate(path);
            self.summary.clang_only.add(root);
            if root != "absolute" {
                clang_only_project = true;
                self.summary
                    .clang_only_project_files
                    .insert(format!("{root}:{relative}"));
            }
        }
        let mut ninja_only = false;
        for (lower, path) in &recorded {
            if opened.contains_key(lower) {
                continue;
            }
            ninja_only = true;
            let (root, relative) = self.locate(path);
            self.summary.ninja_only.add(root);
            self.summary
                .ninja_only_files
                .insert(format!("{root}:{relative}"));
        }
        self.summary.compared_units += 1;
        self.summary.units_with_clang_only_project_files += usize::from(clang_only_project);
        self.summary.units_with_ninja_only_files += usize::from(ninja_only);
    }

    fn manifest<'m>(
        &'m self,
        key: &'m str,
        source: &'m str,
        opened: &Opened,
    ) -> Result<Manifest<'m>, &'static str> {
        let mut files = Vec::with_capacity(opened.len());
        for (path, via) in opened.values() {
            let (sha256, target) = self.content(path)?;
            let (root, path) = self.locate(path);
            files.push(ManifestFile {
                root,
                path,
                sha256,
                target,
                via,
            });
        }
        let entries = self.entries.get(source).map_or(0, Vec::len);
        Ok(Manifest {
            manifest_schema: MANIFEST_SCHEMA,
            cache_schema: super::CACHE_SCHEMA,
            verdict_key: key,
            source,
            tool_version: self.tool_version,
            observation: OBSERVATION_FLAGS,
            compile_commands: entries,
            compile_commands_sha256: super::sha256_hex(
                self.identities
                    .get(source)
                    .ok_or("no_compile_command")?
                    .as_bytes(),
            ),
            roots: Roots {
                repository: self.repository,
                build: self.build,
            },
            files,
        })
    }

    /// Content digest and physical path. A file changed after analysis began
    /// may differ from what clang read, so it makes the manifest incomplete.
    fn content(&self, path: &str) -> Result<(String, Option<String>), &'static str> {
        let mut file = std::fs::File::open(path).map_err(|error| {
            if error.kind() == std::io::ErrorKind::NotFound {
                "missing_file"
            } else {
                "unreadable_file"
            }
        })?;
        let modified = file
            .metadata()
            .and_then(|metadata| metadata.modified())
            .map_err(|_| "unreadable_file")?;
        if modified + FILE_TIME_GRANULARITY >= self.analysis_started {
            return Err("modified_during_analysis");
        }
        let mut bytes = Vec::new();
        file.read_to_end(&mut bytes)
            .map_err(|_| "unreadable_file")?;
        let physical =
            super::normalize_root(&std::fs::canonicalize(path).map_err(|_| "unreadable_file")?);
        let target = (!physical.eq_ignore_ascii_case(path)).then_some(physical);
        Ok((super::sha256_hex(&bytes), target))
    }

    /// Build-tree paths (generated and fetched sources) and repository paths
    /// are root-relative. Everything else stays absolute.
    fn locate(&self, path: &str) -> (&'static str, String) {
        for (root, prefix) in [("build", self.build), ("repository", self.repository)] {
            let prefix = format!("{prefix}/");
            if path
                .get(..prefix.len())
                .is_some_and(|head| head.eq_ignore_ascii_case(&prefix))
            {
                return (root, path[prefix.len()..].to_string());
            }
        }
        ("absolute", path.to_string())
    }
}

/// Written aside and moved into place, like verdict entries.
fn write_manifest(cache_dir: &Path, key: &str, manifest: &Manifest) -> bool {
    let path = super::cache_entry_path(cache_dir, key).with_extension("deps.json");
    let temp = path.with_extension(format!("{}.tmp", std::process::id()));
    let Ok(bytes) = serde_json::to_vec(manifest) else {
        return false;
    };
    path.parent()
        .is_some_and(|parent| std::fs::create_dir_all(parent).is_ok())
        && std::fs::write(&temp, bytes).is_ok()
        && std::fs::rename(&temp, &path).is_ok()
}

/// A noted path is relative to the working directory of the command that
/// opened it. With differing directories that command is unknown.
fn resolve(path: &str, directories: &BTreeSet<&str>) -> Option<String> {
    let forward = path.replace('\\', "/");
    let joined = if super::is_rooted(&forward) {
        forward
    } else if directories.len() == 1 {
        format!("{}/{forward}", directories.first()?)
    } else {
        return None;
    };
    Some(super::normalize_root(Path::new(&joined)))
}

/// Normalized working directory and argument vector of a compile command.
fn command_arguments(command_line: &str) -> Option<(String, Vec<String>)> {
    let (_, identity) = command_line.split_once("\nidentity=")?;
    let (directory, command, arguments): (String, String, Vec<String>) =
        serde_json::from_str(identity).ok()?;
    let arguments = if arguments.is_empty() {
        split_command(&command)?
    } else {
        arguments
    };
    Some((directory, arguments))
}

/// Windows command-line splitting: quotes group, and backslashes escape only
/// a following quote. `None` for an unterminated quote.
fn split_command(command: &str) -> Option<Vec<String>> {
    let mut arguments = Vec::new();
    let mut current = String::new();
    let (mut quoted, mut started, mut backslashes) = (false, false, 0);
    for character in command.chars() {
        if character == '\\' {
            backslashes += 1;
            started = true;
            continue;
        }
        if character == '"' {
            current.push_str(&"\\".repeat(backslashes / 2));
            if backslashes % 2 == 1 {
                current.push('"');
            } else {
                quoted = !quoted;
            }
            backslashes = 0;
            started = true;
            continue;
        }
        current.push_str(&"\\".repeat(backslashes));
        backslashes = 0;
        if character.is_whitespace() && !quoted {
            if started {
                arguments.push(std::mem::take(&mut current));
                started = false;
            }
        } else {
            current.push(character);
            started = true;
        }
    }
    if quoted {
        return None;
    }
    current.push_str(&"\\".repeat(backslashes));
    if started {
        arguments.push(current);
    }
    Some(arguments)
}

/// Files named by `/FI`, `-FI`, `-include` and `--include=`. A response file
/// can hide any of them, so it makes the set unknown.
fn forced_includes(arguments: &[String]) -> Result<Vec<String>, &'static str> {
    let mut forced = Vec::new();
    let mut arguments = arguments.iter().skip(1);
    while let Some(argument) = arguments.next() {
        if argument.starts_with('@') {
            return Err("response_file");
        }
        let value = if argument == "-include" {
            ""
        } else if let Some(value) = ["/FI", "-FI", "--include="]
            .into_iter()
            .find_map(|prefix| argument.strip_prefix(prefix))
        {
            value
        } else {
            continue;
        };
        let value = if value.is_empty() {
            arguments
                .next()
                .map(String::as_str)
                .ok_or("unresolved_forced_include")?
        } else {
            value
        };
        forced.push(value.to_string());
    }
    Ok(forced)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    const KEY: &str = "4b1d00c0ffee";

    struct Fixture {
        _dir: tempfile::TempDir,
        root: PathBuf,
        repository: String,
        build: String,
        source: String,
        entries: Vec<CompileEntry>,
        identities: HashMap<String, String>,
        cache: PathBuf,
    }

    impl Fixture {
        /// A repository with one source, a build tree holding a generated
        /// header, and a header directory outside both. One compile command
        /// in the build directory per entry of `flags`.
        fn new(flags: &[&str]) -> Self {
            let dir = tempfile::tempdir().unwrap();
            let root = dir.path().to_path_buf();
            for (path, text) in [
                ("repo/libs/a.cpp", "int a;\n"),
                ("repo/libs/a.h", "int a_h;\n"),
                ("repo/libs/optional.h", "int optional;\n"),
                ("repo/libs/variant.h", "int variant;\n"),
                ("repo/libs/forced.h", "int forced;\n"),
                ("repo/build/generated/gen.h", "int generated;\n"),
                ("external/ext.h", "int external;\n"),
            ] {
                let path = root.join(path);
                std::fs::create_dir_all(path.parent().unwrap()).unwrap();
                std::fs::write(path, text).unwrap();
            }
            let repository = super::super::normalize_root(&root.join("repo"));
            let build = format!("{repository}/build");
            let source = format!("{repository}/libs/a.cpp");
            let directories = vec![build.as_str(); flags.len()];
            let entries = compile_entries(&source, &directories, flags);
            let identities = super::super::command_identities(&entries);
            Self {
                cache: root.join("cache"),
                _dir: dir,
                root,
                repository,
                build,
                source,
                entries,
                identities,
            }
        }

        fn path(&self, relative: &str) -> String {
            super::super::normalize_root(&self.root.join(relative))
        }

        /// Analysis "began" after every fixture file was written.
        fn recorder(&self) -> Recorder<'_> {
            self.recorder_started(SystemTime::now() + Duration::from_secs(60))
        }

        fn recorder_started(&self, started: SystemTime) -> Recorder<'_> {
            Recorder::new(
                &self.repository,
                &self.build,
                "19.1.5",
                &self.entries,
                &self.identities,
                started,
            )
        }

        fn manifest_path(&self) -> PathBuf {
            super::super::cache_entry_path(&self.cache, KEY).with_extension("deps.json")
        }

        fn manifest(&self) -> serde_json::Value {
            serde_json::from_slice(&std::fs::read(self.manifest_path()).unwrap()).unwrap()
        }
    }

    fn compile_entries(source: &str, directories: &[&str], flags: &[&str]) -> Vec<CompileEntry> {
        let records: Vec<_> = directories
            .iter()
            .zip(flags)
            .map(|(directory, flags)| {
                serde_json::json!({
                    "directory": directory,
                    "file": source,
                    "command": format!("cl.exe /nologo /TP {flags} -c \"{source}\""),
                })
            })
            .collect();
        super::super::parse_compile_db(&serde_json::to_string(&records).unwrap()).unwrap()
    }

    /// `root:path` to `(via, sha256)`.
    fn files(manifest: &serde_json::Value) -> BTreeMap<String, (String, String)> {
        manifest["files"]
            .as_array()
            .unwrap()
            .iter()
            .map(|file| {
                (
                    format!(
                        "{}:{}",
                        file["root"].as_str().unwrap(),
                        file["path"].as_str().unwrap()
                    ),
                    (
                        file["via"].as_str().unwrap().to_string(),
                        file["sha256"].as_str().unwrap().to_string(),
                    ),
                )
            })
            .collect()
    }

    #[test]
    fn include_notes_are_removed_without_changing_the_verdict_output() {
        let raw = "Note: including file: C:\\repo\\a.h\r\n\
                   Note: including file:  .\\sub/b.h\n\
                   C:/repo/a.cpp:3:5: warning: text [bugprone-x]\r\n\
                   Note: including file:   C:/repo/c.h\n\
                   1 warning generated.";
        let (verdict, included) = split_output(raw);
        assert_eq!(
            verdict,
            "C:/repo/a.cpp:3:5: warning: text [bugprone-x]\r\n1 warning generated."
        );
        assert_eq!(included, ["C:\\repo\\a.h", ".\\sub/b.h", "C:/repo/c.h"]);
        assert_eq!(split_output("plain\n"), ("plain\n".to_string(), Vec::new()));

        let timing = super::super::UnitTiming {
            unit: None,
            source: "a.cpp".into(),
            wall_seconds: 0.0,
            cpu_seconds: None,
            peak_working_set_bytes: None,
        };
        let observed = Observed::new(true);
        let (_, output, code, _) = observed.strip(("a.cpp".into(), raw.into(), 1, timing.clone()));
        assert_eq!((output.as_str(), code), (verdict.as_str(), 1));
        assert_eq!(observed.take("a.cpp").unwrap().len(), 3);
        assert!(observed.take("a.cpp").is_none());
        let disabled = Observed::new(false);
        disabled.strip(("a.cpp".into(), raw.into(), 1, timing));
        assert!(disabled.take("a.cpp").is_none());
    }

    #[test]
    fn observation_flags_leave_existing_cache_keys_unchanged() {
        let flags = super::super::invocation_flags("-*,check", "filter");
        assert!(
            OBSERVATION_FLAGS
                .iter()
                .all(|observation| !flags.iter().any(|flag| flag == observation))
        );
    }

    #[test]
    fn manifests_hash_generated_external_forced_and_relative_includes_by_root() {
        let fixture = Fixture::new(&["/FI../libs/forced.h"]);
        let mut recorder = fixture.recorder();
        let included = vec![
            fixture.path("repo/libs/a.h").replace('/', "\\"),
            "generated\\gen.h".into(),
            fixture.path("external/ext.h"),
            fixture.path("repo/libs/A.H"),
        ];
        recorder.record(
            &fixture.cache,
            Some(KEY),
            &fixture.source,
            Some(included),
            None,
        );
        assert_eq!(recorder.summary.recorded, 1, "{:?}", recorder.summary);
        assert_eq!(recorder.summary.observed_files, 5);
        let manifest = fixture.manifest();
        let files = files(&manifest);
        let external = format!("absolute:{}", fixture.path("external/ext.h"));
        let roots: Vec<(&str, &str)> = files
            .iter()
            .map(|(path, (via, _))| (path.as_str(), via.as_str()))
            .collect();
        assert_eq!(
            roots,
            [
                (external.as_str(), "include"),
                ("build:generated/gen.h", "include"),
                ("repository:libs/a.cpp", "source"),
                ("repository:libs/a.h", "include"),
                ("repository:libs/forced.h", "forced"),
            ]
        );
        assert_eq!(
            files["repository:libs/a.h"].1,
            super::super::hash_file(&fixture.path("repo/libs/a.h")).unwrap()
        );
        assert_eq!(manifest["manifest_schema"], MANIFEST_SCHEMA);
        assert_eq!(manifest["cache_schema"], super::super::CACHE_SCHEMA);
        assert_eq!(manifest["verdict_key"], KEY);
        assert_eq!(manifest["source"], fixture.source.as_str());
        assert_eq!(manifest["tool_version"], "19.1.5");
        assert_eq!(manifest["compile_commands"], 1);
        assert_eq!(
            manifest["compile_commands_sha256"],
            super::super::sha256_hex(fixture.identities[&fixture.source].as_bytes())
        );
        assert_eq!(manifest["roots"]["build"], fixture.build.as_str());
        assert!(
            manifest["files"]
                .as_array()
                .unwrap()
                .iter()
                .all(|file| file.get("target").is_none())
        );

        super::super::store_cache_entry(&fixture.cache, KEY, &fixture.source, "", 0);
        assert_eq!(
            super::super::cached_verdicts(&fixture.cache),
            1,
            "a manifest is not a verdict"
        );
        assert!(
            super::super::read_cache_entry(
                &super::super::cache_entry_path(&fixture.cache, KEY),
                KEY,
                &fixture.source
            )
            .is_some()
        );
    }

    #[test]
    fn every_compile_command_contributes_its_conditional_includes() {
        // The second command defines VARIANT. One clang-tidy process runs both,
        // so their notes arrive concatenated. absent.h was only probed with
        // __has_include and never opened.
        let fixture = Fixture::new(&["/I../libs", "/I../libs /DVARIANT"]);
        let mut recorder = fixture.recorder();
        let included = [
            "repo/libs/a.h",
            "repo/libs/optional.h",
            "repo/libs/a.h",
            "repo/libs/optional.h",
            "repo/libs/variant.h",
        ]
        .map(|path| fixture.path(path))
        .to_vec();
        recorder.record(
            &fixture.cache,
            Some(KEY),
            &fixture.source,
            Some(included),
            None,
        );
        let manifest = fixture.manifest();
        assert_eq!(manifest["compile_commands"], 2);
        assert_eq!(
            files(&manifest).into_keys().collect::<Vec<_>>(),
            [
                "repository:libs/a.cpp",
                "repository:libs/a.h",
                "repository:libs/optional.h",
                "repository:libs/variant.h",
            ]
        );
    }

    #[test]
    fn incomplete_manifests_are_counted_and_never_written() {
        let present = Some(vec!["../libs/a.h"]);
        let cases: [(&str, &str, Option<Vec<&str>>, bool); 9] = [
            ("not_observed", "", None, false),
            ("no_includes_observed", "", Some(vec![]), false),
            ("malformed_include_note", "", Some(vec![""]), false),
            ("missing_file", "", Some(vec!["deleted.h"]), false),
            ("modified_during_analysis", "", present.clone(), true),
            (
                "unresolved_forced_include",
                "/FInot-there.h",
                present.clone(),
                false,
            ),
            (
                "unresolved_forced_include",
                "-include",
                present.clone(),
                false,
            ),
            ("response_file", "@flags.rsp", present.clone(), false),
            (
                "unparsable_command",
                "\"/Iunterminated",
                present.clone(),
                false,
            ),
        ];
        for (reason, flags, included, modified) in cases {
            let fixture = Fixture::new(&[flags]);
            let mut recorder = if modified {
                fixture.recorder_started(SystemTime::now())
            } else {
                fixture.recorder()
            };
            let included = included.map(|paths| paths.into_iter().map(String::from).collect());
            recorder.record(&fixture.cache, Some(KEY), &fixture.source, included, None);
            assert_eq!(recorder.summary.recorded, 0, "{reason}");
            assert_eq!(
                recorder.summary.incomplete,
                BTreeMap::from([(reason, 1)]),
                "{reason}"
            );
            assert!(!fixture.manifest_path().exists(), "{reason}");
        }

        let fixture = Fixture::new(&[]);
        let entries = compile_entries(
            &fixture.source,
            &[&fixture.build, &fixture.repository],
            &["", ""],
        );
        let identities = super::super::command_identities(&entries);
        let mut recorder = Recorder::new(
            &fixture.repository,
            &fixture.build,
            "19.1.5",
            &entries,
            &identities,
            SystemTime::now() + Duration::from_secs(60),
        );
        recorder.record(
            &fixture.cache,
            Some(KEY),
            &fixture.source,
            Some(vec!["a.h".into()]),
            None,
        );
        assert_eq!(
            recorder.summary.incomplete,
            BTreeMap::from([("ambiguous_relative_path", 1)])
        );
        assert!(!fixture.manifest_path().exists());
        recorder.record(
            &fixture.cache,
            Some(KEY),
            &fixture.source,
            Some(vec![fixture.path("repo/libs/a.h")]),
            None,
        );
        assert_eq!(
            recorder.summary.recorded, 1,
            "absolute notes stay unambiguous"
        );
    }

    #[test]
    fn clang_and_ninja_differences_are_counted_by_root() {
        let fixture = Fixture::new(&[""]);
        let mut recorder = fixture.recorder();
        let ninja = [
            fixture.source.clone(),
            fixture.path("repo/libs/A.h"),
            fixture.path("repo/libs/optional.h"),
        ];
        recorder.record(
            &fixture.cache,
            None,
            &fixture.source,
            Some(vec![
                fixture.path("repo/libs/a.h"),
                fixture.path("repo/build/generated/gen.h"),
                fixture.path("external/ext.h"),
            ]),
            Some(&ninja),
        );
        let summary = &recorder.summary;
        assert_eq!((summary.unkeyed, summary.recorded), (1, 0));
        assert!(!fixture.manifest_path().exists());
        assert_eq!(summary.compared_units, 1);
        let counts = |counts: &RootCounts| (counts.repository, counts.build, counts.absolute);
        assert_eq!(counts(&summary.clang_only), (0, 1, 1));
        assert_eq!(counts(&summary.ninja_only), (1, 0, 0));
        assert_eq!(summary.units_with_clang_only_project_files, 1);
        assert_eq!(summary.units_with_ninja_only_files, 1);
        assert_eq!(
            summary.clang_only_project_files,
            BTreeSet::from(["build:generated/gen.h".to_string()])
        );
        assert_eq!(
            summary.ninja_only_files,
            BTreeSet::from(["repository:libs/optional.h".to_string()])
        );
    }

    #[cfg(windows)]
    #[test]
    fn junctioned_headers_record_their_physical_target() {
        let fixture = Fixture::new(&[""]);
        for directory in ["first", "second"] {
            let path = fixture.root.join(directory);
            std::fs::create_dir(&path).unwrap();
            std::fs::write(path.join("linked.h"), "int linked;\n").unwrap();
        }
        let link = fixture.root.join("repo/libs/linked");
        let observe = |target: &str| {
            crate::test_support::create_junction(&link, &fixture.root.join(target));
            let mut recorder = fixture.recorder();
            recorder.record(
                &fixture.cache,
                Some(KEY),
                &fixture.source,
                Some(vec![fixture.path("repo/libs/linked/linked.h")]),
                None,
            );
            assert_eq!(recorder.summary.recorded, 1, "{:?}", recorder.summary);
            std::fs::remove_dir(&link).unwrap();
            fixture.manifest()["files"]
                .as_array()
                .unwrap()
                .iter()
                .find(|file| file["path"] == "libs/linked/linked.h")
                .cloned()
                .unwrap()
        };
        let first = observe("first");
        let second = observe("second");
        assert_eq!(first["sha256"], second["sha256"]);
        let target =
            |file: &serde_json::Value| file["target"].as_str().unwrap().to_ascii_lowercase();
        assert_eq!(
            target(&first),
            fixture.path("first/linked.h").to_ascii_lowercase()
        );
        assert_eq!(
            target(&second),
            fixture.path("second/linked.h").to_ascii_lowercase()
        );
    }

    #[test]
    fn commands_split_like_windows_and_name_their_forced_includes() {
        assert_eq!(
            split_command(r#"cl.exe /FI"C:\a b\f.h" -include x.h "a\\\"b" c\d"#).unwrap(),
            [
                "cl.exe",
                r"/FIC:\a b\f.h",
                "-include",
                "x.h",
                r#"a\"b"#,
                r"c\d"
            ]
        );
        assert!(split_command("cl.exe \"open").is_none());
        let arguments: Vec<String> = [
            "cl.exe",
            "/FIa.h",
            "-FI",
            "b.h",
            "-include",
            "c.h",
            "--include=d.h",
            "/Fifile.i",
        ]
        .into_iter()
        .map(String::from)
        .collect();
        assert_eq!(
            forced_includes(&arguments).unwrap(),
            ["a.h", "b.h", "c.h", "d.h"]
        );
        assert_eq!(
            forced_includes(&["cl.exe".into(), "/FI".into()]),
            Err("unresolved_forced_include")
        );
    }

    /// Runs the real clang-tidy, and skips visibly when none is installed.
    #[cfg(windows)]
    #[test]
    fn clang_reports_every_opened_file_without_changing_the_verdict() {
        let Some(tool) = super::super::discover_vs2022_clang_tidy() else {
            assert!(
                std::env::var_os("EXO_DEV_REQUIRE_CLANG_TIDY").is_none(),
                "EXO_DEV_REQUIRE_CLANG_TIDY is set but no Visual Studio clang-tidy was found"
            );
            eprintln!(
                "SKIP clang_reports_every_opened_file_without_changing_the_verdict: \
                 no Visual Studio clang-tidy, so clang's include reporting is unverified here."
            );
            return;
        };
        let dir = tempfile::tempdir().unwrap();
        let root = super::super::normalize_root(dir.path());
        for (path, text) in [
            (
                "a.cpp",
                "#include \"used.h\"\n\
                 #if __has_include(\"optional.h\")\n#include \"optional.h\"\n#endif\n\
                 #if __has_include(\"absent.h\")\n#include \"absent.h\"\n#endif\n\
                 #ifdef __clang__\n#include \"clang_only.h\"\n#endif\n\
                 #ifdef VARIANT\n#include \"variant.h\"\n#endif\n\
                 double half() { return 1 / 2; }\n\
                 int total() { return forced + used + optional + clang_only; }\n",
            ),
            ("forced.h", "static int forced = 1;\n"),
            ("real/used.h", "static int used = 1;\n"),
            ("optional.h", "static int optional = 1;\n"),
            ("clang_only.h", "static int clang_only = 1;\n"),
            ("variant.h", "static int variant = 1;\n"),
        ] {
            let path = dir.path().join(path);
            std::fs::create_dir_all(path.parent().unwrap()).unwrap();
            std::fs::write(path, text).unwrap();
        }
        crate::test_support::create_junction(&dir.path().join("linked"), &dir.path().join("real"));
        let source = format!("{root}/a.cpp");
        let records: Vec<_> = ["", "/DVARIANT"]
            .into_iter()
            .map(|define| {
                serde_json::json!({
                    "directory": root,
                    "file": source,
                    "command": format!("cl.exe /nologo /TP /FI\"{root}/forced.h\" /Ilinked {define} -c \"{source}\""),
                })
            })
            .collect();
        let database = serde_json::to_string(&records).unwrap();
        std::fs::write(dir.path().join("compile_commands.json"), &database).unwrap();
        let flags = super::super::invocation_flags("-*,bugprone-integer-division", ".*");

        let mut plain = crate::process::command(&tool.to_string_lossy());
        plain.arg("-p").arg(dir.path()).args(&flags).arg(&source);
        let (expected, expected_code, _) = super::super::timing::invoke(&mut plain, &source);
        let unit = super::super::invoke_clang_tidy(&tool, dir.path(), &flags, &source, &[]);
        assert!(unit.0.contains(INCLUDE_NOTE), "{}", unit.0);
        let observed = Observed::new(true);
        let (_, output, code, _) = observed.strip((source.clone(), unit.0, unit.1, unit.2));
        assert_eq!((output.as_str(), code), (expected.as_str(), expected_code));
        assert!(output.contains("[bugprone-integer-division]"), "{output}");

        let entries = super::super::parse_compile_db(&database).unwrap();
        let identities = super::super::command_identities(&entries);
        let cache = dir.path().join("cache");
        let build = format!("{root}/build");
        let mut recorder = Recorder::new(
            &root,
            &build,
            "test",
            &entries,
            &identities,
            SystemTime::now() + Duration::from_secs(60),
        );
        recorder.record(&cache, Some(KEY), &source, observed.take(&source), None);
        assert_eq!(recorder.summary.recorded, 1, "{:?}", recorder.summary);
        let manifest: serde_json::Value = serde_json::from_slice(
            &std::fs::read(super::super::cache_entry_path(&cache, KEY).with_extension("deps.json"))
                .unwrap(),
        )
        .unwrap();
        assert_eq!(manifest["compile_commands"], 2);
        let opened: BTreeMap<String, &serde_json::Value> = manifest["files"]
            .as_array()
            .unwrap()
            .iter()
            .map(|file| (file["path"].as_str().unwrap().to_ascii_lowercase(), file))
            .collect();
        assert_eq!(
            opened.keys().map(String::as_str).collect::<Vec<_>>(),
            [
                "a.cpp",
                "clang_only.h",
                "forced.h",
                "linked/used.h",
                "optional.h",
                "variant.h"
            ]
        );
        assert_eq!(
            opened["linked/used.h"]["target"]
                .as_str()
                .unwrap()
                .to_ascii_lowercase(),
            format!("{root}/real/used.h").to_ascii_lowercase()
        );
    }
}
