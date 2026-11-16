//! Dependency manifests: the files clang itself opened for a fresh analysis,
//! and the verdict lookups they key.
//!
//! Every clang-tidy invocation asks clang to report each file its
//! preprocessor opens, including system, external and forced-include headers.
//! The report is a by-product of the analysis, not a second parse. The notes
//! are removed from the output before anything validates, caches or reports
//! it, so verdicts do not depend on them.
//!
//! After a unit's verdict passes its checkpoint revalidation, the files clang
//! opened for every compile command of that unit are hashed into a manifest
//! stored under the unit's lookup key. The verdict itself is stored under a
//! key derived from the lookup key and every manifest file's content and
//! physical target. A later lookup replays a verdict only through a manifest
//! whose files all still match, in the manner of a direct-mode compiler cache.
//!
//! A manifest is written only when it is complete: every noted path and
//! forced include resolves, is admitted by the run's identity, is readable,
//! and was not modified after analysis began. A probe for a missing file,
//! such as `__has_include`, opens nothing. Manifests therefore never describe
//! which files exist. The lookup key's salt covers that membership.

use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::time::{Duration, Instant, SystemTime};

use super::{CompileEntry, TidyUnitResult};

pub(super) const MANIFEST_SCHEMA: u32 = 2;
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

/// Manifest lookups, recording and clang/Ninja comparison for one run.
///
/// `lookup_candidates` counts stored manifests examined for selected units.
/// `lookup_rejected` counts candidates that could not prove a hit, by reason:
/// `changed_input` (an opened file's content or physical target differs),
/// `invalid_manifest`, `missing_verdict`, or the reason an input is no longer
/// admissible. `clang_only` counts, per compared unit, files clang opened that
/// the unit's Ninja record lacks. Ninja omits installed MSVC/SDK headers, so
/// `absolute` entries are expected there. `ninja_only` counts recorded files
/// clang did not open. The file sets list each distinct path and are omitted
/// from public profiles.
#[derive(Clone, Debug, Default, serde::Serialize)]
pub struct ManifestSummary {
    pub lookup_candidates: usize,
    pub lookup_rejected: BTreeMap<&'static str, usize>,
    pub lookup_seconds: f64,
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

#[derive(Debug, serde::Serialize, serde::Deserialize)]
#[serde(deny_unknown_fields)]
struct Manifest {
    manifest_schema: u32,
    cache_schema: u32,
    lookup_key: String,
    verdict_key: String,
    source: String,
    tool_version: String,
    observation: Vec<String>,
    compile_commands: usize,
    compile_commands_sha256: String,
    roots: Roots,
    files: Vec<ManifestFile>,
}

#[derive(Debug, PartialEq, serde::Serialize, serde::Deserialize)]
#[serde(deny_unknown_fields)]
struct Roots {
    repository: String,
    build: String,
}

#[derive(Debug, serde::Serialize, serde::Deserialize)]
#[serde(deny_unknown_fields)]
struct ManifestFile {
    root: String,
    path: String,
    sha256: String,
    /// Physical path, when it differs from the opened path: a link or
    /// junction in the path.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    target: Option<String>,
    via: String,
}

/// The verdict key binds a lookup key to the exact content and physical
/// location of every opened file, so a verdict entry is only reachable
/// through a manifest whose files all still match.
fn verdict_key(lookup_key: &str, files: &[ManifestFile]) -> Option<String> {
    let identity: Vec<_> = files
        .iter()
        .map(|file| (&file.root, &file.path, &file.sha256, &file.target))
        .collect();
    Some(super::sha256_hex(
        format!("{lookup_key}\n{}", serde_json::to_string(&identity).ok()?).as_bytes(),
    ))
}

fn manifest_directory(cache_dir: &Path, lookup_key: &str) -> PathBuf {
    cache_dir
        .join(lookup_key.get(..2).unwrap_or(lookup_key))
        .join(lookup_key)
}

/// Content digest and physical target of a file.
type Content = (String, Option<String>);

/// Opened files by lower-case path: spelling as opened, and how it was reached.
type Opened = BTreeMap<String, (String, &'static str)>;

/// What the run's identity covers. Membership of the directories a lookup
/// searched, and ownership of repository headers, are what make the content
/// of the files clang opened sufficient for a verdict.
pub(super) struct Coverage<'a> {
    /// Roots whose every name is part of the lookup key's salt, lower-case
    /// with a trailing `/`.
    pub(super) roots: Vec<String>,
    /// Toolchain roots, also covered by `roots`, whose files' content and
    /// names are part of the toolchain identity. Only their files may name
    /// an include through a macro.
    pub(super) trusted: Vec<String>,
    /// Lower-case text of every analysis option and configuration file that
    /// can define a macro for any unit besides its compile command: invocation
    /// flags, `ExtraArgs` in `.clang-tidy` files and compiler option
    /// environment variables.
    pub(super) invocation: String,
    /// False for a path that may belong to another checkout.
    pub(super) owned: Box<dyn Fn(&str) -> bool + 'a>,
}

impl Coverage<'_> {
    fn covers(&self, path: &str) -> bool {
        within(&self.roots, path)
    }
}

fn within(roots: &[String], path: &str) -> bool {
    let lower = path.to_ascii_lowercase();
    roots.iter().any(|root| lower.starts_with(root.as_str()))
}

/// One include lookup in a file: whether it is quoted, and its literal name.
/// `None` names a lookup computed from a macro.
type Directive = (bool, Option<String>);

pub(super) struct Recorder<'a> {
    repository: &'a str,
    build: &'a str,
    tool_version: &'a str,
    analysis_started: SystemTime,
    entries: HashMap<&'a str, Vec<&'a CompileEntry>>,
    identities: &'a HashMap<String, String>,
    coverage: Coverage<'a>,
    admissions: HashMap<String, Option<&'static str>>,
    directives: HashMap<String, Result<Vec<String>, &'static str>>,
    contents: HashMap<String, Result<Content, &'static str>>,
    pub(super) summary: ManifestSummary,
    refused: Option<String>,
}

impl<'a> Recorder<'a> {
    /// `analysis_started` must precede every analysis whose notes are recorded.
    pub(super) fn new(
        repository: &'a str,
        build: &'a str,
        tool_version: &'a str,
        entries: &'a [CompileEntry],
        identities: &'a HashMap<String, String>,
        coverage: Coverage<'a>,
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
            coverage,
            admissions: HashMap::new(),
            directives: HashMap::new(),
            contents: HashMap::new(),
            summary: ManifestSummary::default(),
            refused: None,
        }
    }

    /// The stored verdict of `source` whose manifest matches every opened
    /// file's current content and physical target. Every file read here is
    /// rechecked by [`Recorder::revalidate`].
    pub(super) fn lookup(
        &mut self,
        cache_dir: &Path,
        lookup_key: &str,
        source: &str,
    ) -> Option<String> {
        let started = Instant::now();
        let mut candidates: Vec<PathBuf> =
            std::fs::read_dir(manifest_directory(cache_dir, lookup_key))
                .into_iter()
                .flatten()
                .flatten()
                .map(|entry| entry.path())
                .filter(|path| path.to_string_lossy().ends_with(".deps.json"))
                .collect();
        candidates.sort();
        let mut hit = None;
        for candidate in candidates {
            self.summary.lookup_candidates += 1;
            let rejected = match self.verify(&candidate, lookup_key, source) {
                Ok(verdict) => {
                    match super::read_cache_entry(
                        &super::cache_entry_path(cache_dir, &verdict),
                        &verdict,
                        source,
                    ) {
                        Some(output) => {
                            hit = Some(output);
                            break;
                        }
                        None => "missing_verdict",
                    }
                }
                Err(reason) => reason,
            };
            *self.summary.lookup_rejected.entry(rejected).or_default() += 1;
        }
        self.summary.lookup_seconds += started.elapsed().as_secs_f64();
        hit
    }

    fn verify(
        &mut self,
        path: &Path,
        lookup_key: &str,
        source: &str,
    ) -> Result<String, &'static str> {
        const INVALID: &str = "invalid_manifest";
        let manifest: Manifest = serde_json::from_slice(&std::fs::read(path).map_err(|_| INVALID)?)
            .map_err(|_| INVALID)?;
        let named = path
            .file_name()
            .and_then(|name| name.to_str())
            .and_then(|name| name.strip_suffix(".deps.json"));
        let identity = self.identities.get(source).ok_or(INVALID)?;
        let consistent = manifest.manifest_schema == MANIFEST_SCHEMA
            && manifest.cache_schema == super::CACHE_SCHEMA
            && manifest.lookup_key == lookup_key
            && manifest.source == source
            && named == Some(manifest.verdict_key.as_str())
            && manifest.tool_version == self.tool_version
            && manifest.observation == OBSERVATION_FLAGS
            && manifest.roots
                == (Roots {
                    repository: self.repository.to_string(),
                    build: self.build.to_string(),
                })
            && manifest.compile_commands == self.entries.get(source).map_or(0, Vec::len)
            && manifest.compile_commands_sha256 == super::sha256_hex(identity.as_bytes())
            && verdict_key(lookup_key, &manifest.files).as_deref()
                == Some(manifest.verdict_key.as_str());
        if !consistent {
            return Err(INVALID);
        }
        let mut contains_source = false;
        for file in &manifest.files {
            let absolute = self.absolute(&file.root, &file.path).ok_or(INVALID)?;
            contains_source |= file.via == "source" && absolute.eq_ignore_ascii_case(source);
            if let Some(reason) = self.admitted(&absolute) {
                return Err(reason);
            }
            match self.current(&absolute) {
                Ok((sha256, target)) if sha256 == file.sha256 && target == file.target => {}
                _ => return Err("changed_input"),
            }
        }
        if !contains_source {
            return Err(INVALID);
        }
        Ok(manifest.verdict_key)
    }

    pub(super) fn finish(self) -> ManifestSummary {
        self.summary
    }

    /// Fails when any file read by a lookup or manifest changed since, so no
    /// verdict is stored or replayed under content that no longer holds.
    pub(super) fn revalidate(&self) -> anyhow::Result<()> {
        for (path, recorded) in &self.contents {
            anyhow::ensure!(
                read_content(path) == *recorded,
                "clang-tidy inputs changed or became unavailable during analysis: {path}"
            );
        }
        Ok(())
    }

    /// Records the manifest of a checkpointed unit and, when it is complete,
    /// stores `output` as its verdict. Compares the opened files with `ninja`,
    /// the unit's recorded dependencies. Never fails the analysis: an
    /// incomplete or unwritable manifest only leaves the verdict uncached.
    #[allow(clippy::too_many_arguments)]
    pub(super) fn record(
        &mut self,
        cache_dir: &Path,
        lookup_key: Option<&str>,
        source: &str,
        included: Option<Vec<String>>,
        ninja: Option<&[String]>,
        output: &str,
        exit_code: i32,
    ) {
        let started = Instant::now();
        let opened = match included {
            Some(included) => self.opened(source, &included),
            None => Err("not_observed"),
        };
        if let (Ok(opened), Some(ninja)) = (&opened, ninja) {
            self.compare(source, opened, ninja);
        }
        match lookup_key {
            None => self.summary.unkeyed += 1,
            Some(key) => match opened.and_then(|opened| self.manifest(key, source, &opened)) {
                Ok(manifest) => {
                    if super::store_cache_entry(
                        cache_dir,
                        &manifest.verdict_key,
                        source,
                        output,
                        exit_code,
                    ) && write_manifest(cache_dir, &manifest)
                    {
                        self.summary.recorded += 1;
                        self.summary.observed_files += manifest.files.len();
                    } else {
                        self.summary.write_failures += 1;
                    }
                }
                Err(reason) => {
                    *self.summary.incomplete.entry(reason).or_default() += 1;
                    super::timing::progress(serde_json::json!({
                        "event": "manifest-refused",
                        "reason": reason,
                        "unit": source,
                        "detail": self.refused.take(),
                    }));
                }
            },
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

    fn manifest(
        &mut self,
        lookup_key: &str,
        source: &str,
        opened: &Opened,
    ) -> Result<Manifest, &'static str> {
        let mut files = Vec::with_capacity(opened.len());
        for (path, via) in opened.values() {
            if let Some(reason) = self.admitted(path) {
                return Err(self.refuse(reason, path));
            }
            self.unchanged_since_analysis(path)?;
            let (sha256, target) = self.current(path)?;
            let (root, path) = self.locate(path);
            files.push(ManifestFile {
                root: root.to_string(),
                path,
                sha256,
                target,
                via: via.to_string(),
            });
        }
        self.lookups_covered(source, opened)?;
        Ok(Manifest {
            manifest_schema: MANIFEST_SCHEMA,
            cache_schema: super::CACHE_SCHEMA,
            lookup_key: lookup_key.to_string(),
            verdict_key: verdict_key(lookup_key, &files).ok_or("unserializable_manifest")?,
            source: source.to_string(),
            tool_version: self.tool_version.to_string(),
            observation: OBSERVATION_FLAGS
                .iter()
                .map(|flag| flag.to_string())
                .collect(),
            compile_commands: self.entries.get(source).map_or(0, Vec::len),
            compile_commands_sha256: super::sha256_hex(
                self.identities
                    .get(source)
                    .ok_or("no_compile_command")?
                    .as_bytes(),
            ),
            roots: Roots {
                repository: self.repository.to_string(),
                build: self.build.to_string(),
            },
            files,
        })
    }

    /// A file changed after analysis began may differ from what clang read.
    /// Its content at checkpoint time therefore cannot stand for the verdict.
    fn unchanged_since_analysis(&self, path: &str) -> Result<(), &'static str> {
        let modified = std::fs::metadata(path)
            .and_then(|metadata| metadata.modified())
            .map_err(|error| {
                if error.kind() == std::io::ErrorKind::NotFound {
                    "missing_file"
                } else {
                    "unreadable_file"
                }
            })?;
        if modified + FILE_TIME_GRANULARITY >= self.analysis_started {
            return Err("modified_during_analysis");
        }
        Ok(())
    }

    /// Notes the file or lookup behind a refusal for the progress stream.
    fn refuse(&mut self, reason: &'static str, detail: &str) -> &'static str {
        self.refused = Some(detail.to_string());
        reason
    }

    fn admitted(&mut self, path: &str) -> Option<&'static str> {
        if let Some(admission) = self.admissions.get(path) {
            return *admission;
        }
        let admission = if super::input_identity::is_build_artifact(path) {
            Some("artifact_input")
        } else if !(self.coverage.owned)(path) {
            Some("foreign_input")
        } else if !self.coverage.covers(path) {
            Some("uncovered_input")
        } else {
            None
        };
        self.admissions.insert(path.to_string(), admission);
        admission
    }

    /// Fails unless every location an opened file's include lookups could
    /// have probed lies in a covered root, whose membership the salt holds.
    /// A plain name stays below the directory it is searched in, which is
    /// covered. A parent-relative or absolute name can leave it. Clang
    /// searches a quoted name in the including file's directory first, so a
    /// name found there needs no other location.
    fn lookups_covered(&mut self, source: &str, opened: &Opened) -> Result<(), &'static str> {
        let mut search = Vec::new();
        let mut command_lines = Vec::new();
        for entry in self.entries.get(source).ok_or("no_compile_command")? {
            search.extend(search_directories(&entry.command_line)?);
            command_lines.push(entry.command_line.to_ascii_lowercase());
        }
        let includers: BTreeSet<String> = opened
            .values()
            .filter_map(|(path, _)| {
                path.rsplit_once('/')
                    .map(|(directory, _)| directory.to_string())
            })
            .collect();
        // A project macro can wrap `__has_include`, and another opened file
        // can invoke it, so every file's probes see every file's wrappers.
        let mut per_file = Vec::new();
        for (path, _) in opened.values() {
            per_file.push((path.as_str(), self.directives_of(path)?));
        }
        let wrappers = probe_wrappers(per_file.iter().flat_map(|(_, directives)| directives));
        for (path, directives) in &per_file {
            let trusted = within(&self.coverage.trusted, path);
            let mut lookups = include_directives(directives, &wrappers);
            if !trusted && lookups.iter().any(|(_, name)| name.is_none()) {
                let text = std::fs::read(path).map_err(|_| "unreadable_file")?;
                // A macro that no option, configuration file or other opened
                // file names is defined by nothing these inputs describe.
                // Precompiled headers and response files, which could define
                // one unseen, are refused before this point.
                let defined_elsewhere = |name: &str| {
                    let lower = name.to_ascii_lowercase();
                    self.coverage.invocation.contains(&lower)
                        || command_lines.iter().any(|line| line.contains(&lower))
                        || per_file.iter().any(|(other, directives)| {
                            other != path
                                && directives
                                    .iter()
                                    .any(|directive| mentions_definition(directive, name))
                        })
                };
                if let Some(reachable) = without_unreachable_computed_includes(
                    &String::from_utf8_lossy(&text),
                    &defined_elsewhere,
                ) {
                    lookups = include_directives(&preprocessor_lines(&reachable), &wrappers);
                }
            }
            for (quoted, name) in lookups {
                let Some(name) = name else {
                    if trusted {
                        continue;
                    }
                    return Err(self.refuse("computed_include", path));
                };
                let name = name.replace('\\', "/");
                // Lookup membership leaves build artifacts out, so a probe for
                // one could turn true without changing any key.
                if super::input_identity::is_build_artifact(&name) {
                    return Err(self.refuse("artifact_lookup", &format!("{path} -> {name}")));
                }
                if super::is_rooted(&name) {
                    if !self.coverage.covers(&super::normalize_segments(&name)) {
                        return Err(self.refuse("uncovered_lookup", &format!("{path} -> {name}")));
                    }
                    continue;
                }
                if !name.split('/').any(|segment| segment == "..") {
                    continue;
                }
                let candidate = |base: &str| super::normalize_segments(&format!("{base}/{name}"));
                if let Some((own, _)) = path.rsplit_once('/')
                    && quoted
                    && opened.contains_key(&candidate(own).to_ascii_lowercase())
                {
                    continue;
                }
                let covered = includers
                    .iter()
                    .filter(|_| quoted)
                    .chain(search.iter())
                    .chain(self.coverage.trusted.iter())
                    .all(|base| self.coverage.covers(&candidate(base.trim_end_matches('/'))));
                if !covered {
                    return Err(self.refuse("uncovered_lookup", &format!("{path} -> {name}")));
                }
            }
        }
        Ok(())
    }

    /// The preprocessor directives of `path`, without their `#`.
    fn directives_of(&mut self, path: &str) -> Result<Vec<String>, &'static str> {
        self.directives
            .entry(path.to_string())
            .or_insert_with(|| {
                std::fs::read(path)
                    .map(|bytes| preprocessor_lines(&String::from_utf8_lossy(&bytes)))
                    .map_err(|_| "unreadable_file")
            })
            .clone()
    }

    /// Content of `path` as first read in this run.
    fn current(&mut self, path: &str) -> Result<Content, &'static str> {
        self.contents
            .entry(path.to_string())
            .or_insert_with(|| read_content(path))
            .clone()
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

    /// Inverse of [`Recorder::locate`]. `None` for a path that a recorder
    /// could not have produced.
    fn absolute(&self, root: &str, path: &str) -> Option<String> {
        let joined = match root {
            "build" => format!("{}/{path}", self.build),
            "repository" => format!("{}/{path}", self.repository),
            "absolute" if super::is_rooted(path) => path.to_string(),
            _ => return None,
        };
        (super::normalize_segments(&joined) == joined).then_some(joined)
    }
}

fn read_content(path: &str) -> Result<Content, &'static str> {
    let bytes = std::fs::read(path).map_err(|error| {
        if error.kind() == std::io::ErrorKind::NotFound {
            "missing_file"
        } else {
            "unreadable_file"
        }
    })?;
    Ok((super::sha256_hex(&bytes), physical_target(path)?))
}

/// The physical path when a link or junction makes it differ from `path`.
/// Short 8.3 component names are expanded first, so they never count as a
/// different location.
fn physical_target(path: &str) -> Result<Option<String>, &'static str> {
    let physical =
        super::normalize_root(&std::fs::canonicalize(path).map_err(|_| "unreadable_file")?);
    let spelled = super::input_identity::long_path(Path::new(&path.replace('/', "\\")))
        .map_or_else(|| path.to_string(), |long| super::normalize_root(&long));
    Ok((!physical.eq_ignore_ascii_case(&spelled)).then_some(physical))
}

#[cfg(test)]
impl Coverage<'_> {
    /// Covers and owns everything below `root`, and trusts nothing.
    pub(super) fn under(root: &str) -> Self {
        Self {
            roots: vec![format!("{}/", root.to_ascii_lowercase())],
            trusted: Vec::new(),
            invocation: String::new(),
            owned: Box::new(|_| true),
        }
    }
}

/// Every preprocessor line, without its `#`. Lines inside comments or
/// inactive branches count as well, which can only refuse a manifest, never
/// admit one.
fn preprocessor_lines(text: &str) -> Vec<String> {
    // C/C++ translation phase 2 removes a backslash-newline before directives
    // and macro bodies are parsed. Looking at physical lines separately can
    // silently miss a wrapper whose __has_include is on the next line.
    let mut logical = String::new();
    let mut result = Vec::new();
    for physical in text.lines() {
        // CRLF and whitespace following a continuation slash are accepted by
        // clang; conservatively join both spellings for dependency admission.
        let trimmed = physical.trim_end_matches(['\r', ' ', '\t']);
        if let Some(part) = trimmed.strip_suffix('\\') {
            logical.push_str(part);
            continue;
        }
        logical.push_str(physical);
        push_directives(&logical, &mut result);
        logical.clear();
    }
    // A dangling backslash at EOF is malformed preprocessor input; clang-tidy
    // itself cannot produce a successful verdict from that input.
    push_directives(&logical, &mut result);
    result
}

/// A directive may follow a block comment on its line. Without tracking
/// comment state, every position after a `*/` is tried as a line start, which
/// can only add directives to scan.
fn push_directives(line: &str, result: &mut Vec<String>) {
    let after_comments = line.match_indices("*/").map(|(at, _)| &line[at + 2..]);
    for start in std::iter::once(line).chain(after_comments) {
        if let Some(directive) = start.trim_start().strip_prefix('#') {
            result.push(directive.trim_start().to_string());
        }
    }
}

/// One `#if`-family block that is open at a point of a file.
struct Conditional {
    /// The macro of an `#ifdef` or `#ifndef` and whether it is the negated
    /// form. `None` for every other condition.
    guard: Option<(String, bool)>,
    /// Byte offset of the line that opened the block.
    offset: usize,
    /// Whether an `#elif` or `#else` has started a later branch.
    later_branch: bool,
}

impl Conditional {
    /// True when no preprocessing run can enter the current branch: the
    /// first branch of `#ifdef NAME` or a later branch of `#ifndef NAME`,
    /// while `NAME` is never defined when the block is evaluated.
    fn is_unreachable(
        &self,
        text: &str,
        once: bool,
        defined_elsewhere: &dyn Fn(&str) -> bool,
    ) -> bool {
        let Some((name, negated)) = &self.guard else {
            return false;
        };
        // Clang's MSVC-target predefines are underscore-prefixed. Targets that
        // predefine an upper-case name, such as MinGW's `WIN32`, are not
        // analysed here, and the tool's version is part of the cache salt, so
        // an upper-case name starts out undefined and only a `#define` or an
        // option defines it.
        let project_name = name.starts_with(|first: char| first.is_ascii_uppercase())
            && name.chars().all(is_identifier_character);
        if self.later_branch != *negated || !project_name || defined_elsewhere(name) {
            return false;
        }
        // A definition after the block can only matter when the file is
        // entered again, which `#pragma once` rules out.
        !defines_macro(&text[..self.offset], name)
            && (once || !defines_macro(&text[self.offset..], name))
    }
}

fn is_identifier_character(character: char) -> bool {
    character.is_ascii_alphanumeric() || character == '_'
}

/// Whether a `#define` directive, without its `#`, mentions `name` as a whole
/// identifier anywhere. The parameter list and body count too, which can only
/// refuse a manifest.
fn mentions_definition(directive: &str, name: &str) -> bool {
    let Some(rest) = directive.trim_start().strip_prefix("define") else {
        return false;
    };
    !rest.starts_with(is_identifier_character)
        && rest
            .split(|character: char| !is_identifier_character(character))
            .any(|token| token == name)
}

/// Whether any `#define` in `text`, comments removed, mentions `name`. Text
/// whose comments cannot be followed counts as defining it.
fn defines_macro(text: &str, name: &str) -> bool {
    logical_code_lines(text).is_none_or(|lines| {
        lines.iter().any(|(_, _, code)| {
            code.trim_start()
                .strip_prefix('#')
                .is_some_and(|directive| mentions_definition(directive, name))
        })
    })
}

/// The file's lines joined across backslash-newlines, with comments replaced
/// by a space: start and end byte offsets of each logical line and its code.
/// `None` when comment or raw string state cannot be followed reliably.
fn logical_code_lines(text: &str) -> Option<Vec<(usize, usize, String)>> {
    fn strip_comments(line: &str, in_block: &mut bool) -> Option<String> {
        let chars: Vec<char> = line.chars().collect();
        let mut code = String::new();
        let mut at = 0;
        while at < chars.len() {
            let next = chars.get(at + 1).copied();
            if *in_block {
                if chars[at] == '*' && next == Some('/') {
                    *in_block = false;
                    code.push(' ');
                    at += 1;
                }
                at += 1;
                continue;
            }
            match chars[at] {
                '/' if next == Some('/') => break,
                '/' if next == Some('*') => {
                    *in_block = true;
                    at += 2;
                }
                quote @ ('"' | '\'') => {
                    if quote == '"' && at > 0 && chars[at - 1] == 'R' {
                        return None;
                    }
                    code.push(quote);
                    at += 1;
                    // A quote after a letter or digit is a digit separator
                    // or an encoding prefix, not the start of a literal.
                    if quote == '\'' && at > 1 && chars[at - 2].is_alphanumeric() {
                        continue;
                    }
                    while at < chars.len() {
                        let character = chars[at];
                        code.push(character);
                        at += 1;
                        if character == '\\' && at < chars.len() {
                            code.push(chars[at]);
                            at += 1;
                        } else if character == quote {
                            break;
                        }
                    }
                }
                character => {
                    code.push(character);
                    at += 1;
                }
            }
        }
        Some(code)
    }

    let mut lines = Vec::new();
    let mut in_block = false;
    let (mut start, mut offset) = (0, 0);
    let mut joined = String::new();
    for physical in text.split_inclusive('\n') {
        offset += physical.len();
        let body = physical.trim_end_matches(['\n', '\r']);
        if let Some(part) = body.trim_end_matches([' ', '\t']).strip_suffix('\\') {
            joined.push_str(part);
            continue;
        }
        joined.push_str(body);
        lines.push((start, offset, strip_comments(&joined, &mut in_block)?));
        joined.clear();
        start = offset;
    }
    if !joined.is_empty() {
        lines.push((start, text.len(), strip_comments(&joined, &mut in_block)?));
    }
    (!in_block).then_some(lines)
}

/// `text` with every `#include` or `#import` whose operand is a macro blanked
/// when it sits in a branch no preprocessing run can enter. Such a line looks
/// up no file in any run that the manifest's files and compile command
/// describe, because the branch is decided by exactly those inputs. `None`
/// when nothing is blanked or the conditional structure cannot be followed.
fn without_unreachable_computed_includes(
    text: &str,
    defined_elsewhere: &dyn Fn(&str) -> bool,
) -> Option<String> {
    let mut open: Vec<Conditional> = Vec::new();
    let mut once = false;
    let mut blanked = Vec::new();
    for (start, end, code) in logical_code_lines(text)? {
        // C++ also accepts digraph %: as '#' and, with trigraph support,
        // ??= as '#'. This scanner only models literal '#' directives.
        // Treat either spelling as unprovable rather than silently missing
        // a macro definition or a conditional branch.
        let trimmed = code.trim_start();
        if trimmed.starts_with("%:") || trimmed.starts_with("??=") {
            return None;
        }
        let Some(directive) = trimmed.strip_prefix('#') else {
            continue;
        };
        let directive = directive.trim_start();
        let keyword_end = directive
            .find(|character: char| !(character.is_ascii_alphanumeric() || character == '_'))
            .unwrap_or(directive.len());
        let (keyword, rest) = directive.split_at(keyword_end);
        match keyword {
            "ifdef" | "ifndef" => {
                let name: String = rest
                    .trim_start()
                    .chars()
                    .take_while(|&character| character.is_ascii_alphanumeric() || character == '_')
                    .collect();
                open.push(Conditional {
                    guard: (!name.is_empty()).then(|| (name, keyword == "ifndef")),
                    offset: start,
                    later_branch: false,
                });
            }
            "if" => open.push(Conditional {
                guard: None,
                offset: start,
                later_branch: false,
            }),
            "elif" | "elifdef" | "elifndef" | "else" => open.last_mut()?.later_branch = true,
            "endif" => {
                open.pop()?;
            }
            "pragma" if open.is_empty() && rest.trim() == "once" => once = true,
            "include" | "include_next" | "import"
                if !rest.trim_start().starts_with(['"', '<'])
                    && open
                        .iter()
                        .any(|block| block.is_unreachable(text, once, defined_elsewhere)) =>
            {
                blanked.push((start, end));
            }
            _ => {}
        }
    }
    if !open.is_empty() || blanked.is_empty() {
        return None;
    }
    let mut result = String::with_capacity(text.len());
    let mut copied = 0;
    for (start, end) in blanked {
        result.push_str(&text[copied..start]);
        result.extend(text[start..end].matches('\n').map(|_| '\n'));
        copied = end;
    }
    result.push_str(&text[copied..]);
    Some(result)
}

/// The macro name and the rest of a `#define` line.
fn definition(directive: &str) -> Option<(&str, &str)> {
    let rest = directive.strip_prefix("define")?;
    if !rest.starts_with([' ', '\t']) {
        return None;
    }
    let rest = rest.trim_start();
    let end = rest
        .find(|character: char| !(character.is_ascii_alphanumeric() || character == '_'))
        .unwrap_or(rest.len());
    Some((&rest[..end], &rest[end..]))
}

/// Macros whose definition probes with `__has_include`, such as
/// `#define HAS_FILE(x) __has_include(x)`, directly or through another
/// such macro.
fn probe_wrappers<'d>(directives: impl Iterator<Item = &'d String>) -> BTreeSet<String> {
    let definitions: Vec<(&str, &str)> = directives
        .filter_map(|directive| definition(directive))
        .filter(|(name, _)| !name.starts_with("__has_include"))
        .collect();
    let mut wrappers = BTreeSet::new();
    loop {
        let before = wrappers.len();
        for (name, body) in &definitions {
            if body.contains("__has_include")
                || wrappers
                    .iter()
                    .any(|wrapper: &String| body.contains(wrapper.as_str()))
            {
                wrappers.insert(name.to_string());
            }
        }
        if wrappers.len() == before {
            return wrappers;
        }
    }
}

/// Every include lookup the directives can perform: `#include`,
/// `#include_next`, `#import`, `__has_include`/`__has_include_next` probes and
/// invocations of probe `wrappers`. A wrapper's own parameter is not a lookup;
/// its invocations are.
fn include_directives(directives: &[String], wrappers: &BTreeSet<String>) -> Vec<Directive> {
    fn operand(rest: &str) -> Directive {
        let rest = rest.trim_start();
        for (open, close, quoted) in [('"', '"', true), ('<', '>', false)] {
            if let Some(inner) = rest.strip_prefix(open) {
                return (
                    quoted,
                    inner.split_once(close).map(|(name, _)| name.to_string()),
                );
            }
        }
        (false, None)
    }
    let mut lookups = Vec::new();
    for directive in directives {
        if directive.starts_with("undef") {
            continue;
        }
        let (body, parameters) = match definition(directive) {
            Some((name, _)) if name.starts_with("__has_include") => continue,
            Some((_, body)) => {
                let parameters: Vec<&str> = body
                    .strip_prefix('(')
                    .and_then(|list| list.split_once(')'))
                    .map(|(list, _)| list.split(',').map(str::trim).collect())
                    .unwrap_or_default();
                (body, Some(parameters))
            }
            None => {
                for keyword in ["include_next", "include", "import"] {
                    if let Some(rest) = directive.strip_prefix(keyword)
                        && rest.starts_with([' ', '\t', '"', '<'])
                    {
                        lookups.push(operand(rest));
                        break;
                    }
                }
                (directive.as_str(), None)
            }
        };
        let probes = std::iter::once("__has_include").chain(wrappers.iter().map(String::as_str));
        for probe in probes {
            let mut rest = body;
            while let Some(at) = rest.find(probe) {
                rest = &rest[at + probe.len()..];
                let after = rest.strip_prefix("_next").unwrap_or(rest).trim_start();
                let Some(argument) = after.strip_prefix('(') else {
                    continue;
                };
                let lookup = operand(argument);
                let forwards_parameter = lookup.1.is_none()
                    && parameters.as_ref().is_some_and(|parameters| {
                        let name = argument.split([')', ',']).next().unwrap_or("").trim();
                        parameters.contains(&name)
                    });
                if !forwards_parameter {
                    lookups.push(lookup);
                }
            }
        }
    }
    lookups
}

/// Written aside and moved into place, like verdict entries.
fn write_manifest(cache_dir: &Path, manifest: &Manifest) -> bool {
    let directory = manifest_directory(cache_dir, &manifest.lookup_key);
    let path = directory.join(format!("{}.deps.json", manifest.verdict_key));
    let temp = path.with_extension(format!("{}.tmp", std::process::id()));
    let Ok(bytes) = serde_json::to_vec(manifest) else {
        return false;
    };
    std::fs::create_dir_all(&directory).is_ok()
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

/// Options naming an include search directory, joined or followed by it.
/// Longer spellings precede their prefixes.
const SEARCH_OPTIONS: &[&str] = &[
    "--include-directory=",
    "--include-directory",
    "-external:I",
    "/external:I",
    "-idirafter",
    "-isystem",
    "-iquote",
    "-imsvc",
    "/imsvc",
    "-I",
    "/I",
];

/// Options that change where or how includes are found in ways the search
/// directories alone cannot describe.
const UNSUPPORTED_SEARCH_OPTIONS: &[&str] = &[
    "@",
    "-external:env:",
    "/external:env:",
    "/X",
    "-X",
    "-nostdinc",
    "-nobuiltininc",
    "--sysroot",
    "-isysroot",
    "-iprefix",
    "-iwithprefix",
    "-ivfsoverlay",
    "-imacros",
    "-include-pch",
    "/Yu",
    "-Yu",
    "-resource-dir",
    "/winsysroot",
    "-winsysroot",
    "/vctoolsdir",
    "-vctoolsdir",
    "/winsdkdir",
    "-winsdkdir",
    "/clang:",
    "-Xclang",
    "-fmodule",
];

/// Normalized absolute include search directories of a compile command.
/// Fails for an unparsable command or an option that changes include
/// lookup beyond its search directories.
pub(super) fn search_directories(command_line: &str) -> Result<Vec<String>, &'static str> {
    let (directory, arguments) = command_arguments(command_line).ok_or("unparsable_command")?;
    let base = BTreeSet::from([directory.as_str()]);
    let mut directories = Vec::new();
    let mut arguments = arguments.iter().skip(1);
    while let Some(argument) = arguments.next() {
        if UNSUPPORTED_SEARCH_OPTIONS
            .iter()
            .any(|option| argument.starts_with(option))
        {
            return Err("unsupported_search_option");
        }
        let Some(value) = SEARCH_OPTIONS
            .iter()
            .find_map(|option| argument.strip_prefix(option))
        else {
            continue;
        };
        let value = if value.is_empty() {
            arguments.next().ok_or("unparsable_command")?.as_str()
        } else {
            value
        };
        directories.push(resolve(value, &base).ok_or("unparsable_command")?);
    }
    Ok(directories)
}

#[cfg(test)]
mod tests {
    use super::*;

    const KEY: &str = "4b1d00c0ffee";
    const VERDICT: &str = "a.cpp:1:1: note: clean\n";

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
                ("repo/libs/unopened.h", "int unopened;\n"),
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
                Coverage::under(&super::super::normalize_root(&self.root)),
                started,
            )
        }

        fn record(&self, included: &[&str]) -> ManifestSummary {
            self.record_with_invocation(included, "")
        }

        /// As [`Fixture::record`], with `invocation` as the analysis options
        /// and configuration text that can define macros.
        fn record_with_invocation(&self, included: &[&str], invocation: &str) -> ManifestSummary {
            let mut coverage = Coverage::under(&super::super::normalize_root(&self.root));
            coverage.invocation = invocation.to_ascii_lowercase();
            let mut recorder = Recorder::new(
                &self.repository,
                &self.build,
                "19.1.5",
                &self.entries,
                &self.identities,
                coverage,
                SystemTime::now() + Duration::from_secs(60),
            );
            recorder.record(
                &self.cache,
                Some(KEY),
                &self.source,
                Some(included.iter().map(|path| self.path(path)).collect()),
                None,
                VERDICT,
                0,
            );
            recorder.finish()
        }

        /// A fresh run's lookup and its summary.
        fn lookup(&self) -> (Option<String>, ManifestSummary) {
            let mut recorder = self.recorder();
            let hit = recorder.lookup(&self.cache, KEY, &self.source);
            (hit, recorder.finish())
        }

        fn manifest_paths(&self) -> Vec<PathBuf> {
            std::fs::read_dir(manifest_directory(&self.cache, KEY))
                .into_iter()
                .flatten()
                .flatten()
                .map(|entry| entry.path())
                .collect()
        }

        fn manifest(&self) -> serde_json::Value {
            let paths = self.manifest_paths();
            assert_eq!(paths.len(), 1, "{paths:?}");
            serde_json::from_slice(&std::fs::read(&paths[0]).unwrap()).unwrap()
        }

        fn write(&self, relative: &str, text: &str) {
            std::fs::write(self.root.join(relative), text).unwrap();
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

    fn canonical(path: &Path) -> String {
        super::super::normalize_root(&std::fs::canonicalize(path).unwrap()).to_ascii_lowercase()
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
    fn observation_flags_stay_out_of_the_analysis_flags() {
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
            VERDICT,
            0,
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
        assert_eq!(manifest["lookup_key"], KEY);
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
                .all(|file| file.get("target").is_none()),
            "{manifest}"
        );

        let verdict = manifest["verdict_key"].as_str().unwrap();
        assert_ne!(verdict, KEY);
        assert_eq!(
            super::super::cached_verdicts(&fixture.cache),
            1,
            "a manifest is not a verdict"
        );
        assert_eq!(
            super::super::read_cache_entry(
                &super::super::cache_entry_path(&fixture.cache, verdict),
                verdict,
                &fixture.source
            )
            .as_deref(),
            Some(VERDICT)
        );
    }

    #[test]
    fn recorded_verdicts_replay_until_an_opened_file_changes() {
        let fixture = Fixture::new(&["/FI../libs/forced.h"]);
        let opened = [
            "repo/libs/a.h",
            "repo/build/generated/gen.h",
            "external/ext.h",
        ];
        assert_eq!(fixture.record(&opened).recorded, 1);
        let hit = |expected: bool, change: &str| {
            let (output, summary) = fixture.lookup();
            assert_eq!(output.is_some(), expected, "{change}: {summary:?}");
            if expected {
                assert_eq!(output.as_deref(), Some(VERDICT));
            } else {
                assert_eq!(
                    summary.lookup_rejected,
                    BTreeMap::from([("changed_input", 1)]),
                    "{change}"
                );
            }
        };
        hit(true, "unchanged");
        fixture.write("repo/libs/unopened.h", "int never_opened_edit;\n");
        hit(true, "a header clang never opened");
        for (path, original) in [
            ("repo/libs/a.h", "int a_h;\n"),
            ("repo/libs/a.cpp", "int a;\n"),
            ("repo/libs/forced.h", "int forced;\n"),
            ("repo/build/generated/gen.h", "int generated;\n"),
            ("external/ext.h", "int external;\n"),
        ] {
            fixture.write(path, "int edited;\n");
            hit(false, path);
            fixture.write(path, original);
            hit(true, &format!("{path} restored"));
        }
        std::fs::remove_file(fixture.root.join("external/ext.h")).unwrap();
        hit(false, "deleted external header");
    }

    /// clang opened `optional.h` because `__has_include` found it. The
    /// existence of the file is the salt's membership, its content is the
    /// manifest's, so an edit must miss although Ninja recorded nothing.
    #[test]
    fn unrecorded_conditional_header_edits_invalidate_cache_keys() {
        let fixture = Fixture::new(&["/I../libs"]);
        fixture.write(
            "repo/libs/a.cpp",
            "#if __has_include(\"optional.h\")\n#include \"optional.h\"\n#endif\n",
        );
        let mut recorder = fixture.recorder();
        recorder.record(
            &fixture.cache,
            Some(KEY),
            &fixture.source,
            Some(vec![fixture.path("repo/libs/optional.h")]),
            Some(&[]),
            VERDICT,
            0,
        );
        let summary = recorder.finish();
        assert_eq!(summary.recorded, 1);
        assert_eq!(summary.clang_only.repository, 1, "{summary:?}");
        assert!(fixture.lookup().0.is_some());
        fixture.write("repo/libs/optional.h", "int optional = 2;\n");
        assert!(fixture.lookup().0.is_none());
    }

    #[test]
    fn every_compile_command_contributes_its_conditional_includes() {
        // The second command defines VARIANT. One clang-tidy process runs both,
        // so their notes arrive concatenated. absent.h was only probed with
        // __has_include and never opened.
        let fixture = Fixture::new(&["/I../libs", "/I../libs /DVARIANT"]);
        let summary = fixture.record(&[
            "repo/libs/a.h",
            "repo/libs/optional.h",
            "repo/libs/a.h",
            "repo/libs/optional.h",
            "repo/libs/variant.h",
        ]);
        assert_eq!(summary.recorded, 1);
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
        assert!(fixture.lookup().0.is_some());
        fixture.write("repo/libs/variant.h", "int variant = 2;\n");
        assert!(
            fixture.lookup().0.is_none(),
            "a header only one command variant opened"
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
            recorder.record(
                &fixture.cache,
                Some(KEY),
                &fixture.source,
                included,
                None,
                VERDICT,
                0,
            );
            assert_eq!(recorder.summary.recorded, 0, "{reason}");
            assert_eq!(
                recorder.summary.incomplete,
                BTreeMap::from([(reason, 1)]),
                "{reason}"
            );
            assert!(fixture.manifest_paths().is_empty(), "{reason}");
            assert_eq!(
                super::super::cached_verdicts(&fixture.cache),
                0,
                "an incomplete manifest leaves the verdict uncached: {reason}"
            );
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
            Coverage::under(&super::super::normalize_root(&fixture.root)),
            SystemTime::now() + Duration::from_secs(60),
        );
        recorder.record(
            &fixture.cache,
            Some(KEY),
            &fixture.source,
            Some(vec!["a.h".into()]),
            None,
            VERDICT,
            0,
        );
        assert_eq!(
            recorder.summary.incomplete,
            BTreeMap::from([("ambiguous_relative_path", 1)])
        );
        assert!(fixture.manifest_paths().is_empty());
        recorder.record(
            &fixture.cache,
            Some(KEY),
            &fixture.source,
            Some(vec![fixture.path("repo/libs/a.h")]),
            None,
            VERDICT,
            0,
        );
        assert_eq!(
            recorder.summary.recorded, 1,
            "absolute notes stay unambiguous"
        );
    }

    #[test]
    fn inputs_outside_the_run_identity_are_neither_recorded_nor_replayed() {
        let fixture = Fixture::new(&[""]);
        let external = fixture.path("external/ext.h");
        let mut recorder = Recorder::new(
            &fixture.repository,
            &fixture.build,
            "19.1.5",
            &fixture.entries,
            &fixture.identities,
            Coverage::under(&fixture.repository),
            SystemTime::now() + Duration::from_secs(60),
        );
        recorder.record(
            &fixture.cache,
            Some(KEY),
            &fixture.source,
            Some(vec![external.clone()]),
            None,
            VERDICT,
            0,
        );
        assert_eq!(
            recorder.finish().incomplete,
            BTreeMap::from([("uncovered_input", 1)])
        );
        assert!(fixture.manifest_paths().is_empty());

        assert_eq!(fixture.record(&["external/ext.h"]).recorded, 1);
        let mut recorder = Recorder::new(
            &fixture.repository,
            &fixture.build,
            "19.1.5",
            &fixture.entries,
            &fixture.identities,
            Coverage::under(&fixture.repository),
            SystemTime::now() + Duration::from_secs(60),
        );
        assert!(
            recorder
                .lookup(&fixture.cache, KEY, &fixture.source)
                .is_none()
        );
        assert_eq!(
            recorder.finish().lookup_rejected,
            BTreeMap::from([("uncovered_input", 1)])
        );
    }

    type Damage = Box<dyn Fn(&mut serde_json::Value)>;

    #[test]
    fn damaged_or_foreign_manifests_are_misses() {
        let fixture = Fixture::new(&[""]);
        assert_eq!(fixture.record(&["repo/libs/a.h"]).recorded, 1);
        let path = fixture.manifest_paths().remove(0);
        let original: serde_json::Value =
            serde_json::from_slice(&std::fs::read(&path).unwrap()).unwrap();
        let verdict = original["verdict_key"].as_str().unwrap().to_string();
        let damage: Vec<(&str, Damage)> = vec![
            ("schema", Box::new(|m| m["manifest_schema"] = 1.into())),
            ("cache schema", Box::new(|m| m["cache_schema"] = 4.into())),
            ("lookup key", Box::new(|m| m["lookup_key"] = "other".into())),
            ("source", Box::new(|m| m["source"] = "C:/other.cpp".into())),
            ("tool", Box::new(|m| m["tool_version"] = "20.0.0".into())),
            (
                "observation",
                Box::new(|m| m["observation"] = serde_json::json!([])),
            ),
            (
                "roots",
                Box::new(|m| m["roots"]["build"] = "C:/other".into()),
            ),
            ("commands", Box::new(|m| m["compile_commands"] = 2.into())),
            (
                "command digest",
                Box::new(|m| m["compile_commands_sha256"] = "0".into()),
            ),
            (
                "file digest without a new verdict key",
                Box::new(|m| m["files"][1]["sha256"] = "0".into()),
            ),
            (
                "dropped file",
                Box::new(|m| {
                    m["files"].as_array_mut().unwrap().pop();
                }),
            ),
            (
                "escaping path",
                Box::new(|m| m["files"][1]["path"] = "libs/../../outside.h".into()),
            ),
            (
                "unknown root",
                Box::new(|m| m["files"][1]["root"] = "other".into()),
            ),
            ("unknown field", Box::new(|m| m["extra"] = true.into())),
            (
                "verdict key",
                Box::new(|m| m["verdict_key"] = "0".repeat(64).into()),
            ),
        ];
        for (case, change) in damage {
            let mut damaged = original.clone();
            change(&mut damaged);
            std::fs::write(&path, serde_json::to_vec(&damaged).unwrap()).unwrap();
            let (hit, summary) = fixture.lookup();
            assert!(hit.is_none(), "{case}");
            assert_eq!(
                summary.lookup_rejected.values().sum::<usize>(),
                1,
                "{case}: {summary:?}"
            );
        }
        std::fs::write(&path, b"{\"manifest_schema\":").unwrap();
        assert!(fixture.lookup().0.is_none(), "truncated manifest");
        std::fs::write(&path, serde_json::to_vec(&original).unwrap()).unwrap();
        assert!(fixture.lookup().0.is_some(), "restored manifest");

        std::fs::remove_file(super::super::cache_entry_path(&fixture.cache, &verdict)).unwrap();
        let (hit, summary) = fixture.lookup();
        assert!(hit.is_none());
        assert_eq!(
            summary.lookup_rejected,
            BTreeMap::from([("missing_verdict", 1)])
        );
    }

    #[test]
    fn earlier_cache_schemas_are_ignored_and_coexist() {
        let fixture = Fixture::new(&[""]);
        // A schema 4 verdict and its sidecar, stored under the lookup key
        // itself as earlier runs did.
        let shard = super::super::cache_entry_path(&fixture.cache, KEY);
        std::fs::create_dir_all(shard.parent().unwrap()).unwrap();
        std::fs::write(
            &shard,
            serde_json::json!({"schema": 4, "key": KEY, "source": fixture.source, "exit_code": 0, "output": "", "output_sha256": super::super::sha256_hex(b"")}).to_string(),
        )
        .unwrap();
        std::fs::write(
            shard.with_extension("deps.json"),
            serde_json::json!({"manifest_schema": 1, "verdict_key": KEY}).to_string(),
        )
        .unwrap();
        let (hit, summary) = fixture.lookup();
        assert!(hit.is_none());
        assert_eq!(summary.lookup_candidates, 0, "{summary:?}");
        assert_eq!(fixture.record(&["repo/libs/a.h"]).recorded, 1);
        assert_eq!(fixture.lookup().0.as_deref(), Some(VERDICT));
        assert_eq!(super::super::cached_verdicts(&fixture.cache), 2);
    }

    #[test]
    fn several_manifests_per_lookup_key_serve_different_header_states() {
        let fixture = Fixture::new(&[""]);
        assert_eq!(fixture.record(&["repo/libs/a.h"]).recorded, 1);
        fixture.write("repo/libs/a.h", "int a_h = 2;\n");
        assert!(fixture.lookup().0.is_none());
        assert_eq!(fixture.record(&["repo/libs/a.h"]).recorded, 1);
        assert_eq!(fixture.manifest_paths().len(), 2);
        for text in ["int a_h;\n", "int a_h = 2;\n"] {
            fixture.write("repo/libs/a.h", text);
            let (hit, summary) = fixture.lookup();
            assert!(hit.is_some(), "{text}: {summary:?}");
        }
    }

    #[test]
    fn inputs_read_by_lookups_or_manifests_are_revalidated() {
        let fixture = Fixture::new(&[""]);
        assert_eq!(fixture.record(&["repo/libs/a.h"]).recorded, 1);
        let mut recorder = fixture.recorder();
        assert!(
            recorder
                .lookup(&fixture.cache, KEY, &fixture.source)
                .is_some()
        );
        recorder.revalidate().unwrap();
        fixture.write("repo/libs/a.h", "int drifted;\n");
        assert!(recorder.revalidate().is_err());
        fixture.write("repo/libs/a.h", "int a_h;\n");
        recorder.revalidate().unwrap();
        std::fs::remove_file(fixture.root.join("repo/libs/a.h")).unwrap();
        assert!(recorder.revalidate().is_err());
    }

    #[test]
    fn include_directives_name_every_lookup_and_flag_computed_ones() {
        let text = "#include \"a.h\"\n  #  include <sys/b.h>\n#include_next <c>\n\
                    #import \"d.tlh\"\n#include MACRO_HEADER\n\
                    #if __has_include(\"e.inc\") && __has_include_next(<f>)\n\
                    #elif defined(__has_include) || __has_include( NAME )\n\
                    #define __has_include(x) 0\n#includes \"no.h\"\nint x; // #include \"no.h\"\n";
        let named = |quoted: bool, name: &str| (quoted, Some(name.to_string()));
        assert_eq!(
            include_directives(&preprocessor_lines(text), &BTreeSet::new()),
            [
                named(true, "a.h"),
                named(false, "sys/b.h"),
                named(false, "c"),
                named(true, "d.tlh"),
                (false, None),
                named(true, "e.inc"),
                named(false, "f"),
                (false, None),
            ]
        );
    }

    #[test]
    fn directives_after_block_comments_are_still_lookups() {
        let text = "/* note */ #include \"../a.h\"\n/* one */ /* two */ #if __has_include(<b>)\n\
                    /* open\n   still comment */ #include \"c.h\"\nint x = 1; /* #include \"no.h\" */\n";
        let names: Vec<String> = include_directives(&preprocessor_lines(text), &BTreeSet::new())
            .into_iter()
            .filter_map(|(_, name)| name)
            .collect();
        for expected in ["../a.h", "b", "c.h"] {
            assert!(
                names.iter().any(|name| name == expected),
                "{expected} missing from {names:?}"
            );
        }
    }

    #[test]
    fn lookups_that_can_leave_the_covered_roots_refuse_the_manifest() {
        for (case, header, recorded) in [
            ("plain name", "#include \"anything.def\"\n", true),
            (
                "parent-relative name found beside its includer",
                "#include \"../libs/a.h\"\n",
                true,
            ),
            (
                "parent-relative name inside the roots",
                "#if __has_include(\"../libs/none.h\")\n#endif\n",
                true,
            ),
            (
                "parent-relative name leaving the roots",
                "#include \"../../../outside.h\"\n",
                false,
            ),
            (
                "absolute name outside the roots",
                "#if __has_include(<C:/elsewhere/x.h>)\n#endif\n",
                false,
            ),
            ("computed name", "#include HEADER_NAME\n", false),
        ] {
            let fixture = Fixture::new(&["/I../libs"]);
            fixture.write("repo/libs/a.h", header);
            let summary = fixture.record(&["repo/libs/a.h"]);
            assert_eq!(summary.recorded == 1, recorded, "{case}: {summary:?}");
            if !recorded {
                assert_eq!(summary.incomplete.len(), 1, "{case}: {summary:?}");
                assert!(fixture.manifest_paths().is_empty(), "{case}");
            }
        }
    }

    #[test]
    fn toolchain_files_may_compute_include_names() {
        let fixture = Fixture::new(&[""]);
        fixture.write("external/ext.h", "#include _STL_INTRIN_HEADER\n");
        let record = |coverage: Coverage<'_>| {
            let mut recorder = Recorder::new(
                &fixture.repository,
                &fixture.build,
                "19.1.5",
                &fixture.entries,
                &fixture.identities,
                coverage,
                SystemTime::now() + Duration::from_secs(60),
            );
            recorder.record(
                &fixture.cache,
                Some(KEY),
                &fixture.source,
                Some(vec![fixture.path("external/ext.h")]),
                None,
                VERDICT,
                0,
            );
            recorder.finish()
        };
        let root = super::super::normalize_root(&fixture.root);
        assert_eq!(
            record(Coverage::under(&root)).incomplete,
            BTreeMap::from([("computed_include", 1)])
        );
        let mut trusting = Coverage::under(&root);
        trusting.trusted = vec![format!(
            "{}/",
            fixture.path("external").to_ascii_lowercase()
        )];
        assert_eq!(record(trusting).recorded, 1);
    }

    const DEAD_GUARD: &str =
        "#ifdef PROJECT_CONFIG_HEADER\n#include PROJECT_CONFIG_HEADER\n#endif\n";

    /// A computed include in a branch that no run enters needs no lookup. The
    /// header mirrors a library's optional user configuration include.
    #[test]
    fn computed_include_in_a_never_entered_branch_does_not_refuse_the_manifest() {
        for (case, a_h) in [
            (
                "macro defined only after the guard, file entered once",
                format!(
                    "#pragma once\n{DEAD_GUARD}#if DOCS\n#define PROJECT_CONFIG_HEADER\n#endif\n"
                ),
            ),
            (
                "later branch of ifndef",
                "#ifndef PROJECT_CONFIG_HEADER\n#else\n#include PROJECT_CONFIG_HEADER\n#endif\n"
                    .to_string(),
            ),
            (
                "nested inside another condition",
                format!("#if SOMETHING\n#if OTHER\n{DEAD_GUARD}#endif\n#endif\n"),
            ),
            (
                "comments and literals around the directives",
                "/* #define PROJECT_CONFIG_HEADER */ // #define PROJECT_CONFIG_HEADER\n\
                 const char* s = \"/*\"; int n = 1'000;\n\
                 /* x */ #ifdef PROJECT_CONFIG_HEADER\n\
                 #  include PROJECT_CONFIG_HEADER // computed\n#endif\n"
                    .to_string(),
            ),
        ] {
            let fixture = Fixture::new(&["/I../libs"]);
            fixture.write("repo/libs/a.h", &a_h);
            let summary = fixture.record(&["repo/libs/a.h"]);
            assert_eq!(summary.recorded, 1, "{case}: {summary:?}");
            assert!(fixture.lookup().0.is_some(), "{case}");
        }
    }

    /// Every way the macro could be defined, or the branch entered, keeps the
    /// refusal: the lookup then depends on a name the manifest cannot key.
    #[test]
    fn computed_include_that_a_run_could_enter_still_refuses_the_manifest() {
        for (case, flags, a_h, b_h) in [
            (
                "macro defined before the guard",
                "/I../libs",
                format!("#define PROJECT_CONFIG_HEADER \"x.h\"\n{DEAD_GUARD}"),
                String::new(),
            ),
            (
                "macro defined after the guard but the file can be entered again",
                "/I../libs",
                format!("{DEAD_GUARD}#define PROJECT_CONFIG_HEADER \"x.h\"\n"),
                String::new(),
            ),
            (
                "once pragma inside a condition does not stop re-entry",
                "/I../libs",
                format!("#if X\n#pragma once\n#endif\n{DEAD_GUARD}#define PROJECT_CONFIG_HEADER 1\n"),
                String::new(),
            ),
            (
                "macro defined by another opened file",
                "/I../libs",
                format!("#pragma once\n{DEAD_GUARD}"),
                "#define PROJECT_CONFIG_HEADER \"x.h\"\n".to_string(),
            ),
            (
                "macro defined on the command line",
                "/I../libs /DPROJECT_CONFIG_HEADER=x.h",
                format!("#pragma once\n{DEAD_GUARD}"),
                String::new(),
            ),
            (
                "macro undefined on the command line",
                "/I../libs /UPROJECT_CONFIG_HEADER",
                format!("#pragma once\n{DEAD_GUARD}"),
                String::new(),
            ),
            (
                "first branch of ifndef is entered",
                "/I../libs",
                "#ifndef PROJECT_CONFIG_HEADER\n#include PROJECT_CONFIG_HEADER\n#endif\n".to_string(),
                String::new(),
            ),
            (
                "else branch of ifdef is entered",
                "/I../libs",
                "#ifdef PROJECT_CONFIG_HEADER\n#else\n#include PROJECT_CONFIG_HEADER\n#endif\n"
                    .to_string(),
                String::new(),
            ),
            (
                "condition is an expression",
                "/I../libs",
                "#if defined(PROJECT_CONFIG_HEADER)\n#include PROJECT_CONFIG_HEADER\n#endif\n"
                    .to_string(),
                String::new(),
            ),
            (
                "compiler may predefine an underscore name",
                "/I../libs",
                "#ifdef _PROJECT_CONFIG_HEADER\n#include _PROJECT_CONFIG_HEADER\n#endif\n"
                    .to_string(),
                String::new(),
            ),
            (
                "compiler may predefine a lower case name",
                "/I../libs",
                "#ifdef project_config_header\n#include project_config_header\n#endif\n"
                    .to_string(),
                String::new(),
            ),
            (
                "computed include after the guarded block",
                "/I../libs",
                format!("#pragma once\n{DEAD_GUARD}#include OTHER_HEADER\n"),
                String::new(),
            ),
            (
                "guard hidden in a block comment",
                "/I../libs",
                "/*\n#ifdef PROJECT_CONFIG_HEADER\n*/\n#include PROJECT_CONFIG_HEADER\n".to_string(),
                String::new(),
            ),
            (
                "unbalanced conditional",
                "/I../libs",
                "#pragma once\n#endif\n#ifdef PROJECT_CONFIG_HEADER\n#include PROJECT_CONFIG_HEADER\n"
                    .to_string(),
                String::new(),
            ),
            (
                "raw string literal defeats the lexer",
                "/I../libs",
                format!("#pragma once\nconst char* s = R\"(\n#endif\n)\";\n{DEAD_GUARD}"),
                String::new(),
            ),
            (
                "computed probe argument in a guarded block",
                "/I../libs",
                "#pragma once\n#ifdef PROJECT_CONFIG_HEADER\n#if __has_include(PROJECT_CONFIG_HEADER)\n#endif\n#endif\n"
                    .to_string(),
                String::new(),
            ),
        ] {
            let fixture = Fixture::new(&[flags]);
            fixture.write("repo/libs/a.h", &a_h);
            fixture.write("repo/libs/b.h", &b_h);
            let summary = fixture.record(&["repo/libs/a.h", "repo/libs/b.h"]);
            assert_eq!(
                summary.incomplete,
                BTreeMap::from([("computed_include", 1)]),
                "{case}: {summary:?}"
            );
            assert_eq!(summary.recorded, 0, "{case}");
        }
    }

    /// Skipping an unreachable include does not skip the file's other
    /// lookups, and a macro appearing later invalidates the manifest.
    #[test]
    fn unreachable_computed_include_leaves_other_checks_in_force() {
        let fixture = Fixture::new(&["/I../libs"]);
        fixture.write(
            "repo/libs/a.h",
            &format!(
                "#pragma once\n{DEAD_GUARD}#if __has_include(\"../../../uncovered.h\")\n#endif\n"
            ),
        );
        let summary = fixture.record(&["repo/libs/a.h"]);
        assert_eq!(
            summary.incomplete,
            BTreeMap::from([("uncovered_lookup", 1)])
        );

        let fixture = Fixture::new(&["/I../libs"]);
        fixture.write("repo/libs/a.h", &format!("#pragma once\n{DEAD_GUARD}"));
        fixture.write("repo/libs/b.h", "int b;\n");
        assert_eq!(
            fixture.record(&["repo/libs/a.h", "repo/libs/b.h"]).recorded,
            1
        );
        assert!(fixture.lookup().0.is_some());
        fixture.write("repo/libs/b.h", "#define PROJECT_CONFIG_HEADER \"x.h\"\n");
        assert!(
            fixture.lookup().0.is_none(),
            "defining the macro in an opened file must miss the stored manifest"
        );
    }

    /// A macro defined by an analysis option or a configuration file's
    /// `ExtraArgs` is no more visible in the compile command than one from
    /// the environment, yet it enters the branch.
    #[test]
    fn macro_defined_by_the_invocation_keeps_the_refusal() {
        for invocation in [
            "--extra-arg-before=-DPROJECT_CONFIG_HEADER=x.h",
            "ExtraArgs:\n  - '-DPROJECT_CONFIG_HEADER=x.h'\n",
            "/DPROJECT_CONFIG_HEADER=x.h",
        ] {
            let fixture = Fixture::new(&["/I../libs"]);
            fixture.write("repo/libs/a.h", &format!("#pragma once\n{DEAD_GUARD}"));
            let summary = fixture.record_with_invocation(&["repo/libs/a.h"], invocation);
            assert_eq!(
                summary.incomplete,
                BTreeMap::from([("computed_include", 1)]),
                "{invocation}: {summary:?}"
            );
        }
        let fixture = Fixture::new(&["/I../libs"]);
        fixture.write("repo/libs/a.h", &format!("#pragma once\n{DEAD_GUARD}"));
        assert_eq!(
            fixture
                .record_with_invocation(&["repo/libs/a.h"], "-d_xm_no_intrinsics_")
                .recorded,
            1
        );
    }

    #[test]
    fn a_definition_hidden_by_comments_still_defines_the_macro() {
        for text in [
            "#define/**/PROJECT_CONFIG_HEADER 1\n",
            "/**/ #  define PROJECT_CONFIG_HEADER\n",
            "#define \\\n  PROJECT_CONFIG_HEADER 1\n",
            "#define OTHER(PROJECT_CONFIG_HEADER) 1\n",
        ] {
            assert!(defines_macro(text, "PROJECT_CONFIG_HEADER"), "{text}");
        }
        for text in [
            "// #define PROJECT_CONFIG_HEADER\n",
            "/* #define PROJECT_CONFIG_HEADER */\n",
            "#define PROJECT_CONFIG_HEADER_2 1\n",
            "#undef PROJECT_CONFIG_HEADER\n",
        ] {
            assert!(!defines_macro(text, "PROJECT_CONFIG_HEADER"), "{text}");
        }
    }

    #[test]
    fn alternative_preprocessor_directives_do_not_prove_unreachability() {
        // Digraphs are standard C++ preprocessing tokens; trigraphs can
        // also spell '#' when enabled. A macro from either spelling can
        // activate the apparently dead computed include.
        for directive in [
            "%:define PROJECT_CONFIG_HEADER \"x.h\"\n",
            "??=define PROJECT_CONFIG_HEADER \"x.h\"\n",
            "%:if 1\n%:endif\n",
        ] {
            let fixture = Fixture::new(&["/I../libs"]);
            fixture.write("repo/libs/a.h", &format!("#pragma once\n{directive}{DEAD_GUARD}"));
            let summary = fixture.record(&["repo/libs/a.h"]);
            assert_eq!(
                summary.incomplete,
                BTreeMap::from([("computed_include", 1)]),
                "{directive}: {summary:?}"
            );
            assert_eq!(summary.recorded, 0, "{directive}");
        }
    }

    #[test]
    fn blanking_unreachable_includes_keeps_every_other_line() {
        let text = "#pragma once\n#ifdef PROJECT_CONFIG_HEADER\r\n#include \\\n  PROJECT_CONFIG_HEADER\n#endif\nint kept;\n";
        let blanked = without_unreachable_computed_includes(text, &|_| false).unwrap();
        assert_eq!(
            blanked,
            "#pragma once\n#ifdef PROJECT_CONFIG_HEADER\r\n\n\n#endif\nint kept;\n"
        );
        assert_eq!(blanked.lines().count(), text.lines().count());
        assert!(
            without_unreachable_computed_includes(text, &|name| name == "PROJECT_CONFIG_HEADER")
                .is_none()
        );
    }

    /// Lookup membership leaves build artifacts out, so a probe for one must
    /// refuse the manifest: the probed file appearing later changes no key.
    #[test]
    fn probes_for_build_artifact_names_refuse_the_manifest() {
        let fixture = Fixture::new(&["/I../libs"]);
        fixture.write(
            "repo/libs/a.h",
            "#if __has_include(\"marker.obj\")\nint marker;\n#endif\n",
        );
        let summary = fixture.record(&["repo/libs/a.h"]);
        assert_eq!(summary.incomplete, BTreeMap::from([("artifact_lookup", 1)]));
        assert_eq!(super::super::cached_verdicts(&fixture.cache), 0);
        fixture.write("repo/libs/marker.obj", "now present");
        assert!(fixture.lookup().0.is_none());
    }

    /// A project macro can wrap `__has_include`. Its invocations are probes
    /// like the builtin's, and a forwarded or computed argument is unknown.
    #[test]
    fn probe_wrapper_macros_are_followed_to_their_invocations() {
        for (case, a_h, b_h, refusal) in [
            (
                "wrapper probing a covered name",
                "#define HAS_FILE(x) __has_include(x)\n#if HAS_FILE(\"inside.h\")\n#endif\n",
                "",
                None,
            ),
            (
                "wrapper probing outside the roots",
                "#define HAS_FILE(x) __has_include(x)\n#if HAS_FILE(\"../../../uncovered.h\")\n#endif\n",
                "",
                Some("uncovered_lookup"),
            ),
            (
                "wrapper body joined across a backslash newline",
                concat!(
                    "#define HAS_FILE(x) \\\n",
                    "  __has_include(x)\n",
                    "#if HAS_FILE(\"../../../uncovered.h\")\n#endif\n",
                ),
                "",
                Some("uncovered_lookup"),
            ),
            (
                "probe name joined across a backslash newline",
                concat!(
                    "#define HAS_FILE(x) __has_incl\\\n",
                    "ude(x)\n",
                    "#if HAS_FILE(\"../../../uncovered.h\")\n#endif\n",
                ),
                "",
                Some("uncovered_lookup"),
            ),
            (
                "wrapper defined in one file, invoked in another",
                "#define HAS_FILE(x) __has_include(x)\n",
                "#if HAS_FILE(<../../../uncovered.h>)\n#endif\n",
                Some("uncovered_lookup"),
            ),
            (
                "wrapper of a wrapper",
                "#define HAS_FILE(x) __has_include(x)\n#define HAS(x) HAS_FILE(x)\n",
                "#if HAS(\"../../../uncovered.h\")\n#endif\n",
                Some("uncovered_lookup"),
            ),
            (
                "wrapper probing an artifact name",
                "#define HAS_FILE(x) __has_include(x)\n#if HAS_FILE(\"marker.obj\")\n#endif\n",
                "",
                Some("artifact_lookup"),
            ),
            (
                "wrapper invoked with a macro",
                "#define HAS_FILE(x) __has_include(x)\n#if HAS_FILE(HEADER)\n#endif\n",
                "",
                Some("computed_include"),
            ),
            (
                "object-like macro probing a computed name",
                "#define HAS_HEADER __has_include(HEADER)\n",
                "",
                Some("computed_include"),
            ),
            (
                "object-like macro probing outside the roots",
                "#define HAS_UNCOVERED __has_include(\"../../../uncovered.h\")\n",
                "",
                Some("uncovered_lookup"),
            ),
            (
                "redefining the builtin",
                "#define __has_include(x) 0\n",
                "",
                None,
            ),
        ] {
            let fixture = Fixture::new(&["/I../libs"]);
            fixture.write("repo/libs/a.h", a_h);
            fixture.write("repo/libs/b.h", b_h);
            let summary = fixture.record(&["repo/libs/a.h", "repo/libs/b.h"]);
            match refusal {
                None => assert_eq!(summary.recorded, 1, "{case}: {summary:?}"),
                Some(reason) => {
                    assert_eq!(summary.incomplete, BTreeMap::from([(reason, 1)]), "{case}");
                    // Absent before, present now: nothing was stored to replay.
                    fixture.write("uncovered.h", "int uncovered;\n");
                    assert!(fixture.lookup().0.is_none(), "{case}");
                }
            }
        }
    }

    #[test]
    fn opened_build_artifacts_refuse_the_manifest() {
        let fixture = Fixture::new(&[""]);
        fixture.write("repo/build/generated/table.obj", "object");
        let summary = fixture.record(&["repo/build/generated/table.obj"]);
        assert_eq!(summary.incomplete, BTreeMap::from([("artifact_input", 1)]));
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
            VERDICT,
            0,
        );
        let summary = &recorder.summary;
        assert_eq!((summary.unkeyed, summary.recorded), (1, 0));
        assert!(fixture.manifest_paths().is_empty());
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
        crate::test_support::create_junction(&link, &fixture.root.join("first"));
        assert_eq!(fixture.record(&["repo/libs/linked/linked.h"]).recorded, 1);
        let recorded = fixture.manifest()["files"]
            .as_array()
            .unwrap()
            .iter()
            .find(|file| file["path"] == "libs/linked/linked.h")
            .cloned()
            .unwrap();
        assert_eq!(
            recorded["target"].as_str().unwrap().to_ascii_lowercase(),
            canonical(&fixture.root.join("first/linked.h"))
        );
        assert!(fixture.lookup().0.is_some());

        // Identical bytes behind a different directory: siblings and relative
        // includes now resolve elsewhere, so the stored verdict cannot apply.
        std::fs::remove_dir(&link).unwrap();
        crate::test_support::create_junction(&link, &fixture.root.join("second"));
        let (hit, summary) = fixture.lookup();
        assert!(hit.is_none());
        assert_eq!(
            summary.lookup_rejected,
            BTreeMap::from([("changed_input", 1)])
        );
    }

    /// 8.3 spellings, such as a short `TEMP` directory, name the same file
    /// and must not read as a link.
    #[cfg(windows)]
    #[test]
    fn short_path_spellings_have_no_physical_target() {
        let dir = tempfile::tempdir().unwrap();
        let long = dir.path().join("a directory with a long name");
        std::fs::create_dir(&long).unwrap();
        let file = long.join("header.h");
        std::fs::write(&file, "int header;\n").unwrap();
        let path = super::super::normalize_root(&file);
        assert_eq!(physical_target(&path), Ok(None));
        let Some(short) = super::super::input_identity::short_root(&file) else {
            return;
        };
        let short = super::super::normalize_root(&short);
        if short.eq_ignore_ascii_case(&path) {
            eprintln!("SKIP short-name spelling: 8.3 names are disabled on this volume");
            return;
        }
        assert_eq!(physical_target(&short), Ok(None), "{short}");
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

    #[test]
    fn search_directories_cover_every_spelling_and_refuse_unknown_lookup() {
        let command = |flags: &str| {
            compile_entries("C:/repo/libs/a.cpp", &["C:/repo/build"], &[flags])
                .remove(0)
                .command_line
        };
        assert_eq!(
            search_directories(&command(
                "-Iinc /I C:\\abs -external:IC:/ext /external:I \"C:/sp ace\" -isystem sys \
                 -imsvc C:/msvc -iquote q -idirafter after --include-directory=dir \
                 /FIforced.h -DNAME=@value /external:W0 -external:W0"
            ))
            .unwrap(),
            [
                "C:/repo/build/inc",
                "C:/abs",
                "C:/ext",
                "C:/sp ace",
                "C:/repo/build/sys",
                "C:/msvc",
                "C:/repo/build/q",
                "C:/repo/build/after",
                "C:/repo/build/dir",
            ]
        );
        for flags in [
            "@flags.rsp",
            "/X",
            "-nostdinc",
            "/external:env:INCLUDE",
            "-ivfsoverlay overlay.yaml",
            "/clang:-isystem/x",
            "-Xclang -I -Xclang x",
            "/winsysroot C:/sdk",
            "-imacros macros.h",
        ] {
            assert!(search_directories(&command(flags)).is_err(), "{flags}");
        }
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
            Coverage::under(&root),
            SystemTime::now() + Duration::from_secs(60),
        );
        recorder.record(
            &cache,
            Some(KEY),
            &source,
            observed.take(&source),
            None,
            &output,
            code,
        );
        assert_eq!(recorder.summary.recorded, 1, "{:?}", recorder.summary);
        assert_eq!(
            recorder.lookup(&cache, KEY, &source).as_deref(),
            Some(output.as_str()),
            "the recorded verdict replays through its manifest"
        );
        let manifests: Vec<PathBuf> = std::fs::read_dir(manifest_directory(&cache, KEY))
            .unwrap()
            .flatten()
            .map(|entry| entry.path())
            .collect();
        assert_eq!(manifests.len(), 1);
        let manifest: serde_json::Value =
            serde_json::from_slice(&std::fs::read(&manifests[0]).unwrap()).unwrap();
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
            canonical(&dir.path().join("real/used.h"))
        );
        assert!(
            opened
                .iter()
                .filter(|(path, _)| *path != "linked/used.h")
                .all(|(_, file)| file.get("target").is_none()),
            "{manifest}"
        );
    }
}
