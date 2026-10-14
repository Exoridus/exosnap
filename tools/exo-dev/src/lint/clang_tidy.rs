//! The curated, BLOCKING clang-tidy check set over the project's own sources.
//!
//! The repository-wide `.clang-tidy` configuration enables a broad check set
//! that stays advisory (see the advisory-unused-checks job in
//! `.github/workflows/advisory-checks.yml`). This module runs only
//! [`crate::lint::canaries::BLOCKING_CHECKS`], each measured at zero findings
//! in repository-owned files across every project translation unit before it
//! was promoted, which is what makes it safe to fail a pull request on.
//! `.clang-tidy` carries the same list plus the candidates that did not
//! qualify and why.
//!
//! Scope. Analysed are the translation units in the Ninja build's
//! `compile_commands.json` that live inside the repository but outside the
//! build tree and outside `third_party/` (`app/`, `apps/`, `libs/`, `tools/`,
//! `tests/`).
//!
//! `base` restricts that set to what a change actually affects: the changed
//! translation units themselves, plus every translation unit that includes a
//! changed header (resolved from Ninja's recorded dependency graph, so a
//! header edit still reaches its consumers), plus the canary units below when
//! the analysis configuration itself is what changed. A full-tree pass
//! measures hours of CPU time, nearly all of it clang re-parsing Qt headers,
//! which is why CI runs the change-scoped form and the full form is a local /
//! on-demand operation.
//!
//! CMake and preset changes select the full tree because compiler definitions,
//! options and include paths can change findings without changing sources.
//!
//! Analyser note. clang-tidy registers the entire clang-analyzer core package
//! as soon as any `clang-analyzer-*` check is requested;
//! `-clang-analyzer-core.X` cannot turn the extras back off. Only the checks
//! named in `BLOCKING_CHECKS` count as a violation here, so the extras stay
//! out of the verdict.

use std::collections::{BTreeSet, HashMap, HashSet, VecDeque};
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::time::Instant;

mod freshness;
mod input_identity;
#[cfg(test)]
mod measurement;
mod timing;
mod toolchain_identity;
pub use timing::{CompilerPolicySummary, TidyTiming, UnitTiming};

use anyhow::Context as _;
use regex::Regex;
use sha2::{Digest, Sha256};

use crate::executor::ToolMissing;
use crate::lint::{
    self,
    canaries::{BLOCKING_CHECKS, BLOCKING_RULES},
};

/// Version of the per-translation-unit cache key composition. There is no
/// content hash of this module's own source to fold into the key, so this
/// stands in for it. Bump by hand whenever a stored diagnostic could stop
/// describing the same verdict for the same inputs: the clang-tidy
/// invocation's own literal flags change (a new `--extra-arg`, a different
/// `-p`/`--quiet` form), the cache entry's stored format changes, or its
/// producer's required input-validation contract becomes stricter.
/// Scoping (which translation units get analysed) does not affect what a
/// stored entry means and is not a reason to bump this. Nothing else
/// enforces the bump.
pub const CACHE_SCHEMA: u32 = 4;

/// Canary translation units, analysed whenever the analysis configuration
/// itself changes rather than the code it inspects. A change to `.clang-tidy`
/// or to this module alters what every future run means but touches no
/// `.cpp` and no header, so the change-scoped selection would otherwise pick
/// nothing and report green. One representative unit per compilation flavour
/// keeps that class of change honest at a cost of a couple of minutes:
///   * engine, no Qt, heavy Win32/D3D interop
///   * Qt widget code, the expensive parse
///   * a gtest unit, where the test-only idioms live
const CANARY_TRIGGERS: &[&str] = &[
    ".clang-tidy",
    "tools/exo-dev/src/lint/clang_tidy.rs",
    "tools/exo-dev/src/lint/canaries.rs",
];
const CANARY_SOURCES: &[&str] = &[
    "libs/engine/src/audio_thread.cpp",
    "app/quick/ExoSnap/Quick/QuickApplication.cpp",
    "libs/engine/tests/test_split_sentinel_policy.cpp",
];

/// Positive header allowlist: the project's own headers only, never the build
/// tree or `third_party/`. clang-tidy's regex engine has no lookahead, so an
/// exclusion pattern is not expressible here.
const HEADER_FILTER: &str = r"[/\\](app|apps|libs|tools|tests)[/\\]";

#[derive(Debug)]
pub struct ClangTidyReport {
    pub tool_path: PathBuf,
    pub tool_version: String,
    pub compile_db: PathBuf,
    pub scope: String,
    pub analyzed: usize,
    pub cache_enabled: bool,
    pub cache_dir: PathBuf,
    pub replayed: usize,
    pub violations: Vec<String>,
    pub timing: TidyTiming,
    pub compiler_policy: Option<CompilerPolicySummary>,
}

impl ClangTidyReport {
    pub fn ok(&self) -> bool {
        self.violations.is_empty()
    }

    fn skipped(scope: &str, cache_dir: &Path) -> ClangTidyReport {
        ClangTidyReport {
            tool_path: PathBuf::new(),
            tool_version: String::new(),
            compile_db: PathBuf::new(),
            scope: scope.to_string(),
            analyzed: 0,
            cache_enabled: false,
            cache_dir: cache_dir.to_path_buf(),
            replayed: 0,
            violations: Vec::new(),
            timing: TidyTiming::default(),
            compiler_policy: None,
        }
    }
}

/// Runs the blocking clang-tidy check set. `require_version` and
/// `clang_tidy_path` mirror the original script's `-RequireVersion` and
/// `-ClangTidy` (pin or override tool discovery); `list_checks` mirrors
/// `-ListChecks` (print the resolved check list and return without touching
/// the build tree).
///
/// A translation unit whose recorded inputs hash to an entry already in
/// `cache_dir` is not analysed again; its stored diagnostics are replayed
/// instead. The cache key covers every input that can change a verdict: the
/// translation unit, the union of headers recorded for every compile command,
/// the complete command identities, `.clang-tidy`, [`CACHE_SCHEMA`], the check
/// list and the tool version banner. Reuse requires complete, current Ninja
/// records from the configured build. Unhashable inputs are cache misses.
/// Keep inputs stable throughout analysis. Persistent drift aborts before
/// fresh cache writes; before/after checks cannot detect edit-and-restore.
#[allow(clippy::too_many_arguments)]
pub fn run_blocking(
    repo_root: &Path,
    build_dir: &Path,
    base: Option<&str>,
    cache_dir: &Path,
    jobs: usize,
    require_version: Option<&str>,
    list_checks: bool,
    clang_tidy_path: Option<&Path>,
) -> anyhow::Result<ClangTidyReport> {
    if list_checks {
        for check in BLOCKING_CHECKS {
            println!("{check}");
        }
        return Ok(ClangTidyReport::skipped("list-checks", cache_dir));
    }

    let started = Instant::now();
    timing::progress_phase("compile_database", started);
    let mut timing = TidyTiming::default();
    let phase = Instant::now();
    let compile_db_path = build_dir.join("compile_commands.json");
    let compile_db_text = std::fs::read_to_string(&compile_db_path).with_context(|| {
        format!(
            "compile_commands.json not found at {}. Configure the Ninja preset first: cmake --preset windows-x64-ninja-debug",
            compile_db_path.display()
        )
    })?;
    let entries = parse_compile_db(&compile_db_text)?;

    let repo_root_norm = normalize_root(repo_root);
    let build_dir_norm = normalize_root(build_dir);

    let all_sources = project_sources(&entries, &repo_root_norm, &build_dir_norm);
    if all_sources.is_empty() {
        anyhow::bail!(
            "No project translation units found in {}",
            compile_db_path.display()
        );
    }

    timing.compile_database_seconds = phase.elapsed().as_secs_f64();
    timing::progress_phase("tool_discovery", started);
    let phase = Instant::now();
    let tool = clang_tidy_path
        .map(Path::to_path_buf)
        .or_else(|| build_toolchain_clang_tidy(&entries))
        .or_else(discover_vs2022_clang_tidy)
        .or_else(|| lint::find_tool(&["clang-tidy"]))
        .ok_or_else(|| {
            ToolMissing(
                "clang-tidy.exe not found (Visual Studio LLVM toolset or PATH).".to_string(),
            )
        })?;
    if !tool.is_file() {
        return Err(ToolMissing(format!(
            "clang-tidy path '{}' does not exist.",
            tool.display()
        ))
        .into());
    }

    timing.tool_discovery_seconds = phase.elapsed().as_secs_f64();
    timing::progress_phase("tool_version", started);
    let phase = Instant::now();
    let banner = clang_tidy_version_banner(&tool)?;
    let version = parse_version(&banner).unwrap_or_else(|| "unknown".to_string());
    if let Some(expected) = require_version {
        let expected = expected.trim();
        if version != expected && !version.starts_with(&format!("{expected}.")) {
            anyhow::bail!(
                "clang-tidy {version} does not satisfy the required version '{expected}' ({}).",
                tool.display()
            );
        }
    }

    timing.tool_version_seconds = phase.elapsed().as_secs_f64();
    timing::progress(
        serde_json::json!({"event": "tool-ready", "version": version, "elapsed_seconds": started.elapsed().as_secs_f64()}),
    );
    timing::progress_phase("scope", started);
    let phase = Instant::now();
    let (obj_to_source, source_to_obj) = object_maps(&entries, &build_dir_norm);
    let ninja = RealNinja {
        build_dir: build_dir.to_path_buf(),
        extraction_seconds: std::cell::Cell::new(0.0),
        parse_metrics: std::cell::Cell::new((0.0, 0, 0, 0)),
        freshness_seconds: std::cell::Cell::new(0.0),
    };

    let mut already_read_graph: Option<HashMap<String, Vec<String>>> = None;
    let (sources, scope_desc) = match base {
        None => (all_sources.clone(), "full tree".to_string()),
        Some(base) => {
            match scope_for_base(
                repo_root,
                &repo_root_norm,
                &build_dir_norm,
                base,
                &all_sources,
                &obj_to_source,
                &ninja,
                &mut timing,
            )? {
                BaseScope::Nothing(message) => {
                    timing.scope_seconds = phase.elapsed().as_secs_f64();
                    timing.wall_seconds = started.elapsed().as_secs_f64();
                    let mut report = ClangTidyReport::skipped(&message, cache_dir);
                    report.timing = timing;
                    return Ok(report);
                }
                BaseScope::Scoped {
                    sources,
                    description,
                    dep_graph,
                } => {
                    already_read_graph = dep_graph;
                    (sources, description)
                }
            }
        }
    };

    timing.scope_seconds = phase.elapsed().as_secs_f64();
    let jobs = if jobs == 0 {
        std::thread::available_parallelism()
            .map(std::num::NonZeroUsize::get)
            .unwrap_or(4)
    } else {
        jobs
    };

    let checks_arg = format!("-*,{}", BLOCKING_CHECKS.join(","));
    let flags = invocation_flags(&checks_arg, HEADER_FILTER);

    let configuration_started = Instant::now();
    timing::progress_phase("configuration_identity", started);
    let commands = command_identities(&entries);
    let foreign_roots = input_identity::foreign_roots(repo_root);
    let inventory = input_identity::HeaderInventory::read(repo_root, build_dir);

    let configuration_hash = inventory
        .as_ref()
        .and_then(|inventory| inventory.configuration_hash(repo_root));
    let toolchain_started = Instant::now();
    let toolchain_hash = toolchain_identity::hash(build_dir);
    timing.toolchain_identity_seconds = toolchain_started.elapsed().as_secs_f64();
    let configuration_hash = combine_configuration_identity(configuration_hash, toolchain_hash);
    let salt = compute_salt(&banner, &flags, configuration_hash.as_deref());
    timing.configuration_identity_seconds = configuration_started.elapsed().as_secs_f64();

    timing::progress_phase("dependencies", started);
    let phase = Instant::now();
    let mut cache_dependencies = None;
    let (cache_enabled, cache_keys) = if salt.is_none() {
        (false, HashMap::new())
    } else {
        match deps_for_sources(
            &ninja,
            &sources,
            &source_to_obj,
            &obj_to_source,
            &build_dir_norm,
            already_read_graph.as_ref(),
        ) {
            None => (false, HashMap::new()),
            Some(mut dep_graph) => {
                input_identity::retain_dependencies(
                    &mut dep_graph,
                    foreign_roots.as_deref(),
                    inventory.as_ref(),
                );
                timing.dependencies_seconds = phase.elapsed().as_secs_f64();
                let hashing = Instant::now();
                timing::progress_phase("hashing", started);
                let keys =
                    build_cache_keys(&sources, &dep_graph, &commands, salt.as_deref().unwrap());
                timing.hashing_seconds = hashing.elapsed().as_secs_f64();
                cache_dependencies = Some(dep_graph);
                (!keys.is_empty(), keys)
            }
        }
    };

    if cache_enabled {
        std::fs::create_dir_all(cache_dir)
            .with_context(|| format!("could not create {}", cache_dir.display()))?;
    }

    timing.ninja_extraction_seconds = ninja.extraction_seconds.get();
    let (seconds, objects, inputs, sources_count) = ninja.parse_metrics.get();
    timing.dependency_parse_seconds = seconds;
    timing.dependency_objects = objects;
    timing.dependency_inputs = inputs;
    timing.dependency_sources = sources_count;
    timing.freshness_seconds = ninja.freshness_seconds.get();
    let phase = Instant::now();
    let mut results: Vec<(String, String)> = Vec::new();
    timing::progress_phase("cache_lookup", started);
    let mut pending: Vec<String> = Vec::new();
    let mut replayed = 0;
    for src in &sources {
        if let Some(key) = cache_keys.get(src) {
            let entry_path = cache_entry_path(cache_dir, key);
            if let Some(text) = read_cache_entry(&entry_path, key, src) {
                results.push((src.clone(), text));
                replayed += 1;
                continue;
            }
        }
        pending.push(src.clone());
    }

    timing.cache_lookup_seconds = phase.elapsed().as_secs_f64();
    timing.workers = jobs.max(1).min(pending.len());
    timing::progress(
        serde_json::json!({"event": "input-ready", "selected": sources.len(), "hits": replayed, "fresh": pending.len(), "jobs": jobs, "workers": timing.workers, "cache_available": cache_enabled, "cache_keyed": cache_keys.len(), "elapsed_seconds": started.elapsed().as_secs_f64(), "timing": timing}),
    );
    let phase = Instant::now();
    let fresh = analyze_pending(&tool, build_dir, &flags, &pending, jobs);
    timing.analysis_wall_seconds = phase.elapsed().as_secs_f64();
    if cache_enabled && !fresh.is_empty() {
        timing::progress_phase("input_revalidation", started);
        let phase = Instant::now();
        let current_configuration = configuration_identity(repo_root, build_dir);
        // Validate the complete batch before any write; an earlier clean
        // result must not survive a later source's input drift.
        revalidate_cache_inputs(
            &compile_db_path,
            &compile_db_text,
            configuration_hash
                .as_deref()
                .context("missing captured cache configuration")?,
            current_configuration.as_deref(),
            &sources,
            cache_dependencies
                .as_ref()
                .context("missing captured cache dependencies")?,
            &commands,
            salt.as_deref().context("missing captured cache salt")?,
            &cache_keys,
        )?;
        timing.input_revalidation_seconds = phase.elapsed().as_secs_f64();
    }
    let phase = Instant::now();
    timing::progress_phase("validation_storage", started);
    for (src, output, code, unit_timing) in &fresh {
        timing.units.push(unit_timing.clone());
        require_analysis(src, output, *code)?;
        results.push((src.clone(), output.clone()));
        if !cache_enabled {
            continue;
        }
        let Some(key) = cache_keys.get(src) else {
            continue;
        };
        store_cache_entry(cache_dir, key, src, output, *code);
    }

    timing.validation_and_storage_seconds = phase.elapsed().as_secs_f64();
    timing::progress_phase("reporting", started);
    let phase = Instant::now();
    let violations = extract_violations(&results, &repo_root_norm, &build_dir_norm);
    timing.reporting_seconds = phase.elapsed().as_secs_f64();
    timing.wall_seconds = started.elapsed().as_secs_f64();

    Ok(ClangTidyReport {
        tool_path: tool,
        tool_version: version,
        compile_db: compile_db_path,
        scope: scope_desc,
        analyzed: sources.len(),
        cache_enabled,
        cache_dir: cache_dir.to_path_buf(),
        replayed,
        violations: violations.into_iter().collect(),
        timing,
        compiler_policy: Some(timing::compiler_policy(
            &entries,
            &repo_root_norm,
            &build_dir_norm,
        )),
    })
}

// ---------------------------------------------------------------------------
// compile_commands.json
// ---------------------------------------------------------------------------

#[derive(Debug, Clone)]
struct CompileEntry {
    /// Absolute source path, forward slashes.
    file: String,
    /// Absolute object output path, forward slashes.
    /// `None` when the entry carries no `output` field.
    output: Option<String>,
    /// `directory|command arguments`, the entry's contribution to the cache
    /// key: defines and include paths steer the analysis as much as the
    /// source does.
    command_line: String,
}

fn command_identities(entries: &[CompileEntry]) -> HashMap<String, String> {
    let mut grouped: HashMap<String, Vec<&str>> = HashMap::new();
    for entry in entries {
        grouped
            .entry(entry.file.clone())
            .or_default()
            .push(&entry.command_line);
    }
    grouped
        .into_iter()
        .map(|(source, mut commands)| {
            commands.sort_unstable();
            (
                source,
                serde_json::to_string(&commands).expect("compile commands"),
            )
        })
        .collect()
}

#[derive(serde::Deserialize)]
#[serde(deny_unknown_fields)]
struct RawCompileEntry {
    file: String,
    directory: String,
    #[serde(default, deserialize_with = "present_value")]
    command: Option<String>,
    #[serde(default, deserialize_with = "present_value")]
    arguments: Option<Vec<String>>,
    #[serde(default, deserialize_with = "present_value")]
    output: Option<String>,
}

fn present_value<'de, T: serde::Deserialize<'de>, D: serde::Deserializer<'de>>(
    deserializer: D,
) -> Result<Option<T>, D::Error> {
    T::deserialize(deserializer).map(Some)
}

fn parse_compile_db(text: &str) -> anyhow::Result<Vec<CompileEntry>> {
    let records: Vec<RawCompileEntry> = serde_json::from_str(text)
        .context("compile_commands.json is not a valid compilation database")?;
    let mut entries = Vec::with_capacity(records.len());
    for (index, entry) in records.into_iter().enumerate() {
        anyhow::ensure!(
            !entry.file.is_empty(),
            "compile command {index} has no file"
        );
        anyhow::ensure!(
            is_rooted(&entry.directory),
            "compile command {index} requires an absolute directory"
        );
        let directory = normalize_segments(&entry.directory.replace('\\', "/"));
        let file = resolve_against_build_dir(&entry.file, &directory);
        let command = entry.command.unwrap_or_default();
        let arguments = entry.arguments.unwrap_or_default();
        anyhow::ensure!(
            !command.is_empty() || !arguments.is_empty(),
            "compile command {index} has no command or arguments"
        );
        let output = match entry.output {
            Some(output) => {
                anyhow::ensure!(
                    !output.is_empty(),
                    "compile command {index} has an empty output"
                );
                Some(resolve_against_build_dir(&output, &directory))
            }
            None => None,
        };
        entries.push(CompileEntry {
            file,
            output,
            command_line: format!(
                "{directory}|{command}{}\nidentity={}",
                arguments.join(" "),
                serde_json::to_string(&(&directory, &command, &arguments))
                    .expect("compile command")
            ),
        });
    }
    Ok(entries)
}

/// The translation units this run may ever analyse: in the repository,
/// outside the build tree, outside `third_party/`, with a C/C++ source
/// extension. Sorted and deduplicated, matching `Sort-Object -Unique`.
fn project_sources(
    entries: &[CompileEntry],
    repo_root_norm: &str,
    build_dir_norm: &str,
) -> Vec<String> {
    let mut set: BTreeSet<String> = BTreeSet::new();
    for entry in entries {
        if is_project_source(&entry.file, repo_root_norm, build_dir_norm) {
            set.insert(entry.file.clone());
        }
    }
    set.into_iter().collect()
}

fn is_in_repo(path: &str, repo_root_norm: &str, build_dir_norm: &str) -> bool {
    let lower = path.to_ascii_lowercase();
    let root_prefix = format!("{}/", repo_root_norm.to_ascii_lowercase());
    let build_prefix = format!("{}/", build_dir_norm.to_ascii_lowercase());
    lower.starts_with(&root_prefix)
        && !lower.starts_with(&build_prefix)
        && !lower.contains("/third_party/")
}

fn is_project_source(path: &str, repo_root_norm: &str, build_dir_norm: &str) -> bool {
    let lower = path.to_ascii_lowercase();
    (lower.ends_with(".cpp")
        || lower.ends_with(".cxx")
        || lower.ends_with(".cc")
        || lower.ends_with(".c"))
        && is_in_repo(path, repo_root_norm, build_dir_norm)
}

fn is_header(path: &str) -> bool {
    let lower = path.to_ascii_lowercase();
    lower.ends_with(".h")
        || lower.ends_with(".hpp")
        || lower.ends_with(".hxx")
        || lower.ends_with(".inl")
}

fn is_source_or_header(path: &str) -> bool {
    let lower = path.to_ascii_lowercase();
    lower.ends_with(".cpp")
        || lower.ends_with(".cxx")
        || lower.ends_with(".cc")
        || lower.ends_with(".c")
        || is_header(&lower)
}

/// `output` (relative to the build dir) to `file` (absolute), and back. Two
/// consumers need this: the `-Base` scope, to map a changed object's deps
/// back to a source, and the result cache, to ask Ninja for a scoped set of
/// objects by name.
fn object_maps(
    entries: &[CompileEntry],
    build_dir_norm: &str,
) -> (HashMap<String, String>, HashMap<String, Vec<String>>) {
    let mut obj_to_source = HashMap::new();
    let mut source_to_obj: HashMap<String, Vec<String>> = HashMap::new();
    let prefix = format!("{}/", build_dir_norm.to_ascii_lowercase());
    for entry in entries {
        let Some(out) = &entry.output else { continue };
        let stripped = if out.to_ascii_lowercase().starts_with(&prefix) {
            out[prefix.len()..].to_string()
        } else {
            out.clone()
        };
        obj_to_source.insert(stripped.clone(), entry.file.clone());
        source_to_obj
            .entry(entry.file.clone())
            .or_default()
            .push(stripped);
    }
    for objects in source_to_obj.values_mut() {
        objects.sort_unstable();
        objects.dedup();
    }
    let uncovered: HashSet<&str> = entries
        .iter()
        .filter(|entry| entry.output.is_none())
        .map(|entry| entry.file.as_str())
        .collect();
    obj_to_source.retain(|_, source| !uncovered.contains(source.as_str()));
    source_to_obj.retain(|source, _| !uncovered.contains(source.as_str()));
    (obj_to_source, source_to_obj)
}

// ---------------------------------------------------------------------------
// Path normalization
// ---------------------------------------------------------------------------

fn normalize_root(path: &Path) -> String {
    path.to_string_lossy()
        .replace('\\', "/")
        .trim_end_matches('/')
        .to_string()
}

fn is_rooted(path: &str) -> bool {
    path.starts_with('/')
        || (path.len() >= 2
            && path.as_bytes()[1] == b':'
            && path.as_bytes()[0].is_ascii_alphabetic())
}

/// Resolves `path` (as Ninja or clang-tidy may print it, possibly relative
/// and using `../`) against `build_dir_norm`, without touching the
/// filesystem. Both Ninja's dependency lists and clang-tidy's diagnostics can
/// name a file relative to the build directory; those have to be resolved
/// before any prefix test, or a real finding in a repository header would be
/// mistaken for a third-party one and dropped.
fn resolve_against_build_dir(path: &str, build_dir_norm: &str) -> String {
    let forward = path.replace('\\', "/");
    let combined = if is_rooted(&forward) {
        forward
    } else {
        format!("{build_dir_norm}/{forward}")
    };
    normalize_segments(&combined)
}

fn normalize_segments(path: &str) -> String {
    let (prefix, rest) = if let Some(stripped) = path.strip_prefix('/') {
        ("/".to_string(), stripped)
    } else if path.len() >= 2 && path.as_bytes()[1] == b':' {
        (path[..2].to_string() + "/", path.get(3..).unwrap_or(""))
    } else {
        (String::new(), path)
    };
    let mut stack: Vec<&str> = Vec::new();
    for segment in rest.split('/') {
        match segment {
            "" | "." => {}
            ".." => {
                stack.pop();
            }
            other => stack.push(other),
        }
    }
    format!("{prefix}{}", stack.join("/"))
}

// ---------------------------------------------------------------------------
// Ninja's recorded dependency graph
// ---------------------------------------------------------------------------

/// Ninja recorded, during the build, every header each object file actually
/// opened (`/showIncludes` -> `.ninja_deps`). Injected so tests can supply
/// canned output instead of a real Ninja invocation; the production
/// implementation ([`RealNinja`]) always shells out to the real `ninja -t
/// deps`, never a reimplementation of its dependency format.
trait NinjaDeps {
    /// The whole recorded graph, or `None` when it could not be read.
    fn whole_graph(&self) -> Option<Vec<String>>;
    /// The graph for a batch of object files by name, or `None` when it could
    /// not be read.
    fn for_objects(&self, objects: &[String]) -> Option<Vec<String>>;
    fn objects_current(&self, objects: &[String]) -> bool;
    fn current_objects(&self, objects: &[String]) -> HashSet<String> {
        if self.objects_current(objects) {
            objects.iter().cloned().collect()
        } else {
            HashSet::new()
        }
    }
    fn record_parse(&self, _seconds: f64, _objects: usize, _inputs: usize, _sources: usize) {}
}

struct RealNinja {
    build_dir: PathBuf,
    extraction_seconds: std::cell::Cell<f64>,
    parse_metrics: std::cell::Cell<(f64, usize, usize, usize)>,
    freshness_seconds: std::cell::Cell<f64>,
}

impl NinjaDeps for RealNinja {
    fn whole_graph(&self) -> Option<Vec<String>> {
        let started = Instant::now();
        let result = invoke_ninja_deps(&self.build_dir, &[]);
        self.extraction_seconds
            .set(self.extraction_seconds.get() + started.elapsed().as_secs_f64());
        result
    }

    fn for_objects(&self, objects: &[String]) -> Option<Vec<String>> {
        let started = Instant::now();
        let result = invoke_ninja_deps(&self.build_dir, objects);
        self.extraction_seconds
            .set(self.extraction_seconds.get() + started.elapsed().as_secs_f64());
        result
    }

    fn objects_current(&self, objects: &[String]) -> bool {
        let started = Instant::now();
        let current = freshness::objects_current(&self.build_dir, objects);
        self.freshness_seconds
            .set(self.freshness_seconds.get() + started.elapsed().as_secs_f64());
        current
    }

    fn current_objects(&self, objects: &[String]) -> HashSet<String> {
        let started = Instant::now();
        let current = freshness::current_objects(&self.build_dir, objects);
        self.freshness_seconds
            .set(self.freshness_seconds.get() + started.elapsed().as_secs_f64());
        current
    }

    fn record_parse(&self, seconds: f64, objects: usize, inputs: usize, sources: usize) {
        let old = self.parse_metrics.get();
        self.parse_metrics.set((
            old.0 + seconds,
            old.1 + objects,
            old.2 + inputs,
            old.3 + sources,
        ));
    }
}

fn invoke_ninja_deps(build_dir: &Path, args: &[String]) -> Option<Vec<String>> {
    let mut command = crate::process::command("ninja");
    command.current_dir(build_dir);
    command.arg("-t").arg("deps");
    command.args(args);
    let (code, stdout) = crate::process::query(command).ok()?;
    if code != 0 {
        return None;
    }
    let lines: Vec<String> = stdout.lines().map(str::to_string).collect();
    if lines.is_empty() { None } else { Some(lines) }
}

fn deps_header_regex() -> &'static Regex {
    static RE: std::sync::OnceLock<Regex> = std::sync::OnceLock::new();
    RE.get_or_init(|| {
        Regex::new(r"^(\S.*?):\s+#deps (\d+), deps mtime -?\d+ \((VALID|STALE)\)$").unwrap()
    })
}

/// Parses `ninja -t deps` output into a source-keyed dependency graph.
/// Dependency lines are indented and outnumber the object headers by roughly
/// three orders of magnitude, so they are recognised by their first
/// character rather than by running the header regex on each one.
fn parse_ninja_deps(
    lines: &[String],
    obj_to_source: &HashMap<String, String>,
    build_dir_norm: &str,
) -> HashMap<String, Vec<String>> {
    let mut records: HashMap<String, Option<Vec<String>>> = HashMap::new();
    let mut current: Option<(String, usize, bool, Vec<String>)> = None;
    for line in lines.iter().map(String::as_str).chain(std::iter::once("")) {
        if line.starts_with(' ') || line.starts_with('\t') {
            if let Some((_, _, _, deps)) = &mut current {
                let dep = resolve_against_build_dir(line.trim(), build_dir_norm);
                deps.push(dep);
            }
            continue;
        }
        if let Some((object, count, valid, deps)) = current.take() {
            let complete = (valid && deps.len() == count).then_some(deps);
            records
                .entry(object)
                .and_modify(|value| *value = None)
                .or_insert(complete);
        }
        if let Some(caps) = deps_header_regex().captures(line) {
            let obj = caps[1].replace('\\', "/");
            if obj_to_source.contains_key(&obj) {
                if let Ok(count) = caps[2].parse() {
                    current = Some((obj, count, &caps[3] == "VALID", Vec::new()));
                }
            }
        }
    }
    let mut graph: HashMap<String, BTreeSet<String>> = HashMap::new();
    let mut incomplete = HashSet::new();
    for (object, source) in obj_to_source {
        match records.get(object).and_then(Option::as_ref) {
            Some(deps) => graph
                .entry(source.clone())
                .or_default()
                .extend(deps.iter().cloned()),
            None => {
                incomplete.insert(source.clone());
            }
        }
    }
    graph
        .into_iter()
        .filter(|(source, _)| !incomplete.contains(source))
        .map(|(source, deps)| (source, deps.into_iter().collect()))
        .collect()
}

fn timed_parse_ninja_deps(
    ninja: &dyn NinjaDeps,
    lines: &[String],
    objects: &HashMap<String, String>,
    build_dir_norm: &str,
) -> HashMap<String, Vec<String>> {
    let started = Instant::now();
    let graph = parse_ninja_deps(lines, objects, build_dir_norm);
    let object_count = objects
        .values()
        .filter(|source| graph.contains_key(*source))
        .count();
    ninja.record_parse(
        started.elapsed().as_secs_f64(),
        object_count,
        graph.values().map(Vec::len).sum(),
        graph.len(),
    );
    graph
}

fn deps_for_sources(
    ninja: &dyn NinjaDeps,
    sources: &[String],
    source_to_obj: &HashMap<String, Vec<String>>,
    obj_to_source: &HashMap<String, String>,
    build_dir_norm: &str,
    already: Option<&HashMap<String, Vec<String>>>,
) -> Option<HashMap<String, Vec<String>>> {
    if let Some(graph) = already {
        return Some(graph.clone());
    }
    let objects: Vec<String> = sources
        .iter()
        .filter_map(|s| source_to_obj.get(s))
        .flatten()
        .cloned()
        .collect();
    if objects.is_empty() {
        return Some(HashMap::new());
    }
    // Batched: a whole-tree selection would otherwise build a command line
    // past the Windows limit and fail as a malformed invocation rather than
    // as a missing graph.
    let current = ninja.current_objects(&objects);
    if current.is_empty() {
        return None;
    }
    let selected_objects: HashMap<String, String> = objects
        .iter()
        .filter_map(|object| {
            obj_to_source
                .get(object)
                .map(|source| (object.clone(), source.clone()))
        })
        .collect();
    let stale_sources: HashSet<&str> = selected_objects
        .iter()
        .filter(|(object, _)| !current.contains(*object))
        .map(|(_, source)| source.as_str())
        .collect();
    let current: Vec<String> = objects
        .into_iter()
        .filter(|object| current.contains(object))
        .collect();
    // Ninja reloads the manifest and dependency log for every invocation.
    // The conservative switch point matches the measured large workload.
    let lines = if current.len() >= 1000 {
        ninja.whole_graph()?
    } else {
        let mut lines = Vec::new();
        for chunk in current.chunks(100) {
            lines.extend(ninja.for_objects(chunk)?);
        }
        lines
    };
    let mut graph = timed_parse_ninja_deps(ninja, &lines, &selected_objects, build_dir_norm);
    graph.retain(|source, _| !stale_sources.contains(source.as_str()));
    Some(graph)
}

// ---------------------------------------------------------------------------
// `-Base` scoping
// ---------------------------------------------------------------------------

#[derive(Debug)]
enum BaseScope {
    /// Nothing to analyse; carries the message to report.
    Nothing(String),
    Scoped {
        sources: Vec<String>,
        description: String,
        /// The whole dependency graph, when this path already read it, so the
        /// cache-key step does not pay to read it again.
        dep_graph: Option<HashMap<String, Vec<String>>>,
    },
}

#[allow(clippy::too_many_arguments)]
fn scope_for_base(
    repo_root: &Path,
    repo_root_norm: &str,
    build_dir_norm: &str,
    base: &str,
    all_sources: &[String],
    obj_to_source: &HashMap<String, String>,
    ninja: &dyn NinjaDeps,
    timing: &mut TidyTiming,
) -> anyhow::Result<BaseScope> {
    let git_started = Instant::now();
    let git = crate::git::Git::new(repo_root);
    let (code, stdout) = git.run(&[
        "diff",
        "--name-status",
        "--diff-filter=ACMRD",
        base,
        "--",
        ".",
    ]);
    anyhow::ensure!(
        code == 0,
        "git diff against '{base}' failed - is the base revision fetched?"
    );
    let changes: Vec<(String, String)> = stdout
        .lines()
        .flat_map(|line| {
            line.split_once('\t')
                .into_iter()
                .flat_map(|(status, paths)| {
                    paths
                        .split('\t')
                        .map(move |path| (status.to_string(), path.replace('\\', "/")))
                })
        })
        .collect();
    let changed_all: Vec<String> = changes.iter().map(|(_, path)| path.clone()).collect();
    timing.git_scope_seconds = git_started.elapsed().as_secs_f64();
    let compile_configuration_changed = changed_all.iter().any(|path| {
        path.ends_with("CMakeLists.txt")
            || path.ends_with(".cmake")
            || matches!(path.as_str(), "CMakePresets.json" | "CMakeUserPresets.json")
    });
    // File appearance can change include search or __has_include without
    // being represented in the prior compiler dependency list.
    if changes.iter().any(|(status, path)| {
        is_header(path) && (matches!(status.as_str(), "A" | "D") || status.starts_with('R'))
    }) {
        return Ok(BaseScope::Scoped {
            sources: all_sources.to_vec(),
            description: format!("full tree: header added or removed since {base}"),
            dep_graph: Some(HashMap::new()),
        });
    }
    if compile_configuration_changed {
        return Ok(BaseScope::Scoped {
            sources: all_sources.to_vec(),
            description: format!("full tree: compile configuration changed since {base}"),
            dep_graph: None,
        });
    }
    let changed: Vec<String> = changed_all
        .iter()
        .filter(|f| is_source_or_header(f))
        .cloned()
        .collect();
    let canary_hit: Vec<String> = changed_all
        .iter()
        .filter(|f| {
            CANARY_TRIGGERS.contains(&f.as_str())
                || f.starts_with("tools/exo-dev/src/lint/clang_tidy/")
        })
        .cloned()
        .collect();

    if changed.is_empty() && canary_hit.is_empty() {
        return Ok(BaseScope::Nothing(format!(
            "No C/C++ sources or headers changed since {base} - nothing to analyse."
        )));
    }

    let changed_abs: HashSet<String> = changed
        .iter()
        .map(|c| format!("{repo_root_norm}/{c}").to_ascii_lowercase())
        .collect();

    let mut affected: BTreeSet<String> = all_sources
        .iter()
        .filter(|s| changed_abs.contains(&s.to_ascii_lowercase()))
        .cloned()
        .collect();

    if !canary_hit.is_empty() {
        let all_sources_lower: HashSet<String> =
            all_sources.iter().map(|s| s.to_ascii_lowercase()).collect();
        for canary in CANARY_SOURCES {
            let abs = format!("{repo_root_norm}/{canary}");
            if all_sources_lower.contains(&abs.to_ascii_lowercase()) {
                affected.insert(abs);
            } else {
                anyhow::bail!(
                    "Canary translation unit '{canary}' is not in the compile database. \
                     Update CANARY_SOURCES in tools/exo-dev/src/lint/clang_tidy.rs to name files that still exist."
                );
            }
        }
    }

    let changed_headers: Vec<&String> = changed.iter().filter(|f| is_header(f)).collect();
    let mut dep_graph_read: Option<HashMap<String, Vec<String>>> = None;
    if !changed_headers.is_empty() {
        let expansion_started = Instant::now();
        // Fail closed. Without the dependency graph the consumers of a
        // changed header are unknown, and a header-only change would then
        // analyse nothing and report green: the gate would be silently
        // absent exactly where it matters most.
        let Some(lines) = ninja.whole_graph() else {
            anyhow::bail!(
                "'ninja -t deps' returned nothing for the build directory. The changed \
                 header(s) ({}) cannot be mapped to the translation units that include them, \
                 so this run cannot prove anything. Rebuild the Ninja preset so .ninja_deps \
                 exists, or run the full pass without a base.",
                changed_headers.len()
            );
        };
        let mut graph = timed_parse_ninja_deps(ninja, &lines, obj_to_source, build_dir_norm);
        let foreign_roots = input_identity::foreign_roots(repo_root);
        let inventory = input_identity::HeaderInventory::read(repo_root, Path::new(build_dir_norm));
        input_identity::retain_dependencies(
            &mut graph,
            foreign_roots.as_deref(),
            inventory.as_ref(),
        );
        let changed_header_names: HashSet<String> = changed_headers
            .iter()
            .map(|header| header.to_ascii_lowercase())
            .collect();
        let objects: Vec<String> = obj_to_source
            .iter()
            .filter(|(_, source)| graph.contains_key(*source))
            .map(|(object, _)| object.clone())
            .collect();
        // Ninja's VALID marker only compares the dependency record with the
        // object mtime. A dry run also checks source changes and compiler flags.
        let current = ninja.current_objects(&objects);
        let stale_sources: HashSet<&str> = obj_to_source
            .iter()
            .filter(|(object, _)| !current.contains(*object))
            .map(|(_, source)| source.as_str())
            .collect();
        graph.retain(|source, _| !stale_sources.contains(source.as_str()));
        affected.extend(
            all_sources
                .iter()
                .filter(|source| !graph.contains_key(*source))
                .cloned(),
        );
        for (src, deps) in &graph {
            if !is_project_source(src, repo_root_norm, build_dir_norm) {
                continue;
            }
            if deps.iter().any(|dep| {
                inventory
                    .as_ref()
                    .and_then(|inventory| inventory.repository_relative(dep))
                    .is_some_and(|relative| changed_header_names.contains(&relative))
            }) {
                affected.insert(src.clone());
            }
        }
        dep_graph_read = Some(graph);
        timing.header_expansion_seconds = expansion_started.elapsed().as_secs_f64();
    }

    if affected.is_empty() {
        return Ok(BaseScope::Nothing(format!(
            "No analysable translation unit is affected by the changes since {base}."
        )));
    }

    Ok(BaseScope::Scoped {
        sources: affected.into_iter().collect(),
        description: format!("changed since {base}"),
        dep_graph: dep_graph_read,
    })
}

// ---------------------------------------------------------------------------
// Result cache
// ---------------------------------------------------------------------------

/// Analysing one Qt-heavy translation unit costs tens of seconds, almost all
/// of it clang re-parsing the same Qt headers; hashing that unit's entire
/// recorded input set costs a fraction of that. So a unit whose inputs are
/// unchanged replays its stored diagnostics instead of being analysed again.
///
/// The danger of a cache in front of a blocking gate is a stale hit
/// reporting a green that was never established. Reuse requires complete,
/// current recorded inputs and compiler commands. Unhashable files are misses.
fn compute_salt(
    clang_tidy_banner: &str,
    invocation_flags: &[String],
    configuration_hash: Option<&str>,
) -> Option<String> {
    let config_hash = configuration_hash?;
    Some(format!(
        "{clang_tidy_banner}\n{}\nCACHE_SCHEMA={CACHE_SCHEMA}\nconfiguration={config_hash}",
        invocation_flags.join("\n")
    ))
}

fn combine_configuration_identity(
    configuration: Option<String>,
    toolchain: Option<String>,
) -> Option<String> {
    configuration
        .zip(toolchain)
        .map(|(configuration, toolchain)| {
            sha256_hex(format!("configuration={configuration}\ntoolchain={toolchain}").as_bytes())
        })
}

fn configuration_identity(repo: &Path, build: &Path) -> Option<String> {
    let configuration =
        input_identity::HeaderInventory::read(repo, build)?.configuration_hash(repo);
    combine_configuration_identity(configuration, toolchain_identity::hash(build))
}

fn build_cache_keys(
    sources: &[String],
    dep_graph: &HashMap<String, Vec<String>>,
    commands: &HashMap<String, String>,
    salt: &str,
) -> HashMap<String, String> {
    let mut hash_cache: HashMap<String, Option<String>> = HashMap::new();
    let mut keys = HashMap::new();
    for src in sources {
        // No recorded dependency list means the unit's true input set is
        // unknown; hashing only the source would key a hit on a stale header.
        let Some(deps) = dep_graph.get(src) else {
            continue;
        };
        let Some(command_line) = commands.get(src) else {
            continue;
        };
        let mut parts = vec![salt.to_string(), command_line.clone()];
        let mut usable = true;
        for input in std::iter::once(src.clone()).chain(deps.iter().cloned()) {
            let hash = hash_cache
                .entry(input.clone())
                .or_insert_with(|| hash_file(&input))
                .clone();
            match hash {
                Some(h) => parts.push(format!("{input}={h}")),
                None => {
                    usable = false;
                    break;
                }
            }
        }
        if !usable {
            continue;
        }
        keys.insert(src.clone(), sha256_hex(parts.join("\n").as_bytes()));
    }
    keys
}

#[allow(clippy::too_many_arguments)]
fn revalidate_cache_inputs(
    compile_db_path: &Path,
    compile_db_text: &str,
    expected_configuration: &str,
    configuration: Option<&str>,
    sources: &[String],
    dependencies: &HashMap<String, Vec<String>>,
    commands: &HashMap<String, String>,
    salt: &str,
    keys: &HashMap<String, String>,
) -> anyhow::Result<()> {
    anyhow::ensure!(
        std::fs::read(compile_db_path).ok().as_deref() == Some(compile_db_text.as_bytes()),
        "clang-tidy inputs changed or became unavailable during analysis: compilation database"
    );
    anyhow::ensure!(
        configuration == Some(expected_configuration),
        "clang-tidy inputs changed or became unavailable during analysis: configuration or toolchain"
    );
    anyhow::ensure!(
        build_cache_keys(sources, dependencies, commands, salt) == *keys,
        "clang-tidy inputs changed or became unavailable during analysis: source or dependency content"
    );
    Ok(())
}

fn hash_file(path: &str) -> Option<String> {
    let bytes = std::fs::read(path).ok()?;
    Some(sha256_hex(&bytes))
}

fn sha256_hex(bytes: &[u8]) -> String {
    let digest = Sha256::digest(bytes);
    hex::encode(digest)
}

fn cache_entry_path(cache_dir: &Path, key: &str) -> PathBuf {
    cache_dir.join(&key[..2]).join(format!("{key}.txt"))
}

#[derive(serde::Serialize, serde::Deserialize)]
#[serde(deny_unknown_fields)]
struct CacheEntry {
    schema: u32,
    key: String,
    source: String,
    exit_code: i32,
    output: String,
    output_sha256: String,
}

fn read_cache_entry(path: &Path, key: &str, source: &str) -> Option<String> {
    let bytes = std::fs::read(path).ok()?;
    let entry: CacheEntry = serde_json::from_slice(&bytes).ok()?;
    if entry.schema != CACHE_SCHEMA
        || entry.key != key
        || entry.source != source
        || entry.output_sha256 != sha256_hex(entry.output.as_bytes())
        || require_analysis(source, &entry.output, entry.exit_code).is_err()
    {
        return None;
    }
    Some(entry.output)
}

/// Written aside and moved into place: a run cancelled mid-write would
/// otherwise leave a truncated entry that later reads as a complete verdict.
fn store_cache_entry(cache_dir: &Path, key: &str, source: &str, output: &str, exit_code: i32) {
    if require_analysis(source, output, exit_code).is_err() {
        return;
    }
    let record = CacheEntry {
        schema: CACHE_SCHEMA,
        key: key.to_string(),
        source: source.to_string(),
        exit_code,
        output: output.to_string(),
        output_sha256: sha256_hex(output.as_bytes()),
    };
    let Ok(bytes) = serde_json::to_vec(&record) else {
        return;
    };
    let entry = cache_entry_path(cache_dir, key);
    let Some(parent) = entry.parent() else { return };
    if std::fs::create_dir_all(parent).is_err() {
        return;
    }
    let temp = entry.with_extension(format!("{}.tmp", std::process::id()));
    if std::fs::write(&temp, bytes).is_err() {
        return;
    }
    let _ = std::fs::rename(&temp, &entry);
}

// ---------------------------------------------------------------------------
// clang-tidy invocation
// ---------------------------------------------------------------------------

/// clang-tidy always sits at a fixed offset below the VS 2022 installation
/// this tree is built against, so this checks that one path directly rather
/// than going through `lint::find_tool`'s recursive directory walk: a
/// `-Recurse`-equivalent sweep of a Visual Studio installation costs minutes
/// on a cold file-system cache (the original script's own reason for the
/// same fixed-offset check), which would dwarf a scoped clang-tidy run. Only
/// when this fails does discovery fall back to `find_tool`'s three-tier
/// search (standalone LLVM, then PATH; it also re-checks this same VS-LLVM
/// tier, just via the slower walk).
fn discover_vs2022_clang_tidy() -> Option<PathBuf> {
    let root = crate::msvc::find_installation_vs2022()?;
    let candidate = root.join("VC/Tools/Llvm/x64/bin/clang-tidy.exe");
    candidate.is_file().then_some(candidate)
}

/// The clang-tidy bundled with the Visual Studio installation whose compiler
/// produced the compile database. clang-tidy parses that installation's STL
/// headers, and each MSVC STL refuses Clang versions older than the one it was
/// released with, so a clang-tidy from another installation can fail on every
/// translation unit. This differs from the VS-2022 pin the formatter uses:
/// with VS 2022 and a newer VS line side by side, the build imports the newer
/// toolchain while that pin would still pick VS 2022's older LLVM.
fn build_toolchain_clang_tidy(entries: &[CompileEntry]) -> Option<PathBuf> {
    let root = entries
        .iter()
        .find_map(|entry| compiler_installation(&entry.command_line))?;
    let candidate = root.join("VC/Tools/Llvm/x64/bin/clang-tidy.exe");
    candidate.is_file().then_some(candidate)
}

/// The Visual Studio installation root of the compiler an entry's
/// `directory|command` line invokes: everything before `\VC\Tools\MSVC\` in
/// the compiler path. `None` for a compiler outside a Visual Studio layout.
fn compiler_installation(command_line: &str) -> Option<PathBuf> {
    let command = command_line
        .split_once('|')
        .map_or(command_line, |(_, command)| command)
        .trim_start();
    let compiler = match command.strip_prefix('"') {
        Some(quoted) => quoted.split('"').next()?,
        None => command.split_whitespace().next()?,
    };
    let normalized = compiler.replace('/', "\\");
    let marker = normalized
        .to_ascii_lowercase()
        .find("\\vc\\tools\\msvc\\")?;
    Some(PathBuf::from(&normalized[..marker]))
}

fn clang_tidy_version_banner(tool: &Path) -> anyhow::Result<String> {
    let mut command = crate::process::command(&tool.to_string_lossy());
    command.arg("--version");
    let (code, stdout) = crate::process::query(command)
        .with_context(|| format!("could not run {}", tool.display()))?;
    version_query_result(tool, code, stdout)
}

fn version_query_result(tool: &Path, code: i32, stdout: String) -> anyhow::Result<String> {
    anyhow::ensure!(
        code == 0,
        "{} --version failed (exit {code})",
        tool.display()
    );
    Ok(stdout)
}

fn version_regex() -> &'static Regex {
    static RE: std::sync::OnceLock<Regex> = std::sync::OnceLock::new();
    RE.get_or_init(|| Regex::new(r"version\s+(\d+\.\d+\.\d+)").unwrap())
}

fn parse_version(banner: &str) -> Option<String> {
    version_regex()
        .captures(banner)
        .map(|caps| caps[1].to_string())
}

/// The clang-tidy invocation's literal flags, excluding `-p <build dir>`
/// (the build directory is a path, not a verdict-relevant setting: the same
/// compile database reached through a different path must not miss the
/// cache) and the per-TU source path (already part of each entry's own
/// hashed input set). Built once so the cache salt in [`compute_salt`] and
/// the real invocation in [`invoke_clang_tidy`] cannot drift apart if a flag
/// is ever added here.
fn invocation_flags(checks_arg: &str, header_filter: &str) -> Vec<String> {
    let flags = vec![
        "--quiet".to_string(),
        format!("--checks={checks_arg}"),
        format!("--header-filter={header_filter}"),
    ];
    #[cfg(windows)]
    let flags = {
        let mut flags = flags;
        // Older Clang lacks MSVC's macro selecting UCRT's constexpr offsetof.
        flags.push("--extra-arg-before=-D_CRT_USE_BUILTIN_OFFSETOF=1".to_string());
        flags
    };
    flags
}

fn invoke_clang_tidy(
    tool: &Path,
    build_dir: &Path,
    flags: &[String],
    source: &str,
) -> (String, i32, UnitTiming) {
    let mut command = crate::process::command(&tool.to_string_lossy());
    command.arg("-p").arg(build_dir);
    command.args(flags);
    command.arg(source);
    timing::invoke(&mut command, source)
}

/// Runs clang-tidy over `pending` with up to `jobs` processes in flight at
/// once.
fn analyze_pending(
    tool: &Path,
    build_dir: &Path,
    flags: &[String],
    pending: &[String],
    jobs: usize,
) -> Vec<(String, String, i32, UnitTiming)> {
    if pending.is_empty() {
        return Vec::new();
    }
    let jobs = jobs.max(1).min(pending.len());
    let started = Instant::now();
    let queue: Mutex<VecDeque<(usize, String)>> =
        Mutex::new(pending.iter().cloned().enumerate().collect());
    let results = Mutex::new(Vec::new());
    std::thread::scope(|scope| {
        for _ in 0..jobs {
            scope.spawn(|| {
                loop {
                    let next = queue.lock().unwrap().pop_front();
                    let Some((index, src)) = next else { break };
                    timing::progress(serde_json::json!({"event": "tu-start", "unit": index + 1, "total": pending.len(), "analysis_elapsed_seconds": started.elapsed().as_secs_f64()}));
                    let (output, code, mut unit_timing) =
                        invoke_clang_tidy(tool, build_dir, flags, &src);
                    unit_timing.unit = Some(index + 1);
                    let mut results = results.lock().unwrap();
                    timing::progress(serde_json::json!({"event": "tu-complete", "unit": index + 1, "completed": results.len() + 1, "total": pending.len(), "analysis_elapsed_seconds": started.elapsed().as_secs_f64(), "exit_code": code, "wall_seconds": unit_timing.wall_seconds, "cpu_seconds": unit_timing.cpu_seconds, "peak_working_set_bytes": unit_timing.peak_working_set_bytes}));
                    results.push((src, output, code, unit_timing));
                }
            });
        }
    });
    results.into_inner().unwrap()
}

// ---------------------------------------------------------------------------
// Violations
// ---------------------------------------------------------------------------

fn diagnostic_regex() -> &'static Regex {
    static RE: std::sync::OnceLock<Regex> = std::sync::OnceLock::new();
    RE.get_or_init(|| {
        Regex::new(r"^(.*?):(\d+):(\d+):\s+(?:warning|error):\s+(.*?)\s+\[([^\]]+)\]\s*$").unwrap()
    })
}

/// A blocking check matches `check` either exactly, or by prefix when
/// `pattern` ends in a wildcard (`BLOCKING_CHECKS`' only two wildcard entries
/// both trail, e.g. `clang-analyzer-cplusplus.NewDelete*`).
fn check_matches(check: &str, pattern: &str) -> bool {
    match pattern.strip_suffix('*') {
        Some(prefix) => check.starts_with(prefix),
        None => check == pattern,
    }
}

/// Infrastructure failures are rejected before extracting findings. A check
/// blocks only in its repository-owned scope, excluding external SDK findings.
fn extract_violations(
    results: &[(String, String)],
    repo_root_norm: &str,
    build_dir_norm: &str,
) -> BTreeSet<String> {
    let mut violations = BTreeSet::new();
    let root_prefix = format!("{repo_root_norm}/");
    for (_, output) in results {
        for line in output.lines() {
            let Some(caps) = diagnostic_regex().captures(line) else {
                continue;
            };
            let where_ = resolve_against_build_dir(&caps[1], build_dir_norm);
            if !is_in_repo(&where_, repo_root_norm, build_dir_norm) {
                continue;
            }
            let row = &caps[2];
            let col = &caps[3];
            let message = &caps[4];
            let checks_field = caps[5].replace(",-warnings-as-errors", "");
            // Ownership was checked case-insensitively above. ASCII case
            // folding preserves the prefix length used to retain display case.
            let rel = &where_[root_prefix.len()..];
            for check in checks_field.split(',') {
                let check = check.trim();
                if BLOCKING_RULES
                    .iter()
                    .any(|rule| rule.applies_to(rel) && check_matches(check, rule.check))
                {
                    violations.insert(format!("{rel}:{row}:{col} [{check}] {message}"));
                }
            }
        }
    }
    violations
}

fn require_analysis(source: &str, output: &str, code: i32) -> anyhow::Result<()> {
    anyhow::ensure!(
        matches!(code, 0 | 1),
        "clang-tidy failed to analyze {source} (exit {code})"
    );
    let failed_parse = output.lines().find(|line| {
        line.contains("[clang-diagnostic-error")
            || line.contains("[clang-diagnostic-fatal")
            || line.contains("Error while processing")
            || line.starts_with("error:")
    });
    anyhow::ensure!(
        failed_parse.is_none(),
        "clang-tidy could not parse {source}: {}",
        failed_parse.unwrap_or_default()
    );
    static LOCATED_DIAGNOSTIC: std::sync::OnceLock<Regex> = std::sync::OnceLock::new();
    let located = LOCATED_DIAGNOSTIC
        .get_or_init(|| Regex::new(r"^.*?:\d+:\d+:\s+(?:warning|error|fatal error):").unwrap());
    let malformed = output
        .lines()
        .find(|line| located.is_match(line) && !diagnostic_regex().is_match(line));
    anyhow::ensure!(
        malformed.is_none(),
        "clang-tidy reported an incomplete diagnostic for {source}: {}",
        malformed.unwrap_or_default()
    );
    anyhow::ensure!(
        code == 0 || output.lines().any(|line| diagnostic_regex().is_match(line)),
        "clang-tidy failed without diagnostic evidence for {source}: {output}"
    );
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn unsupported_or_duplicate_compile_database_fields_are_rejected() {
        for text in [
            r#"[{"directory":"C:/repo","file":"C:/repo/a.cpp","command":"cl a.cpp","output":"a.obj","unexpected":"x"}]"#,
            r#"[{"directory":"C:/repo","file":{},"file":"C:/repo/a.cpp","command":"cl a.cpp","output":"a.obj"}]"#,
        ] {
            assert!(parse_compile_db(text).is_err(), "{text}");
        }
    }

    #[test]
    fn relative_source_and_object_paths_are_resolved_against_each_command_directory() {
        let entries = parse_compile_db(r#"[{"directory":"C:/repo/build/child","file":"../../libs/a.cpp","command":"cl ../../libs/a.cpp","output":"a.obj"}]"#).unwrap();
        assert_eq!(entries[0].file, "C:/repo/libs/a.cpp");
        assert_eq!(
            entries[0].output.as_deref(),
            Some("C:/repo/build/child/a.obj")
        );
    }

    #[test]
    fn malformed_compile_entries_are_rejected_without_a_partial_census() {
        let valid = serde_json::json!({"file":"C:/repo/a.cpp", "directory":"C:/repo/build", "command":"cl.exe a.cpp"});
        for (field, value) in [
            ("arguments", serde_json::json!([null])),
            ("arguments", serde_json::json!("bad")),
            ("file", serde_json::Value::Null),
            ("directory", serde_json::Value::Null),
            ("command", serde_json::Value::Null),
            ("output", serde_json::json!(1)),
        ] {
            let mut malformed = valid.clone();
            malformed[field] = value;
            assert!(
                parse_compile_db(&serde_json::to_string(&vec![valid.clone(), malformed]).unwrap())
                    .is_err(),
                "{field}"
            );
        }
        for entry in [
            serde_json::json!({"file":"a.cpp","command":"cl.exe a.cpp"}),
            serde_json::json!({"file":"a.cpp","directory":"C:/repo","arguments":[]}),
        ] {
            assert!(parse_compile_db(&serde_json::to_string(&vec![entry]).unwrap()).is_err());
        }
    }

    #[test]
    fn extracted_analysis_modules_trigger_the_existing_canary_scope() {
        let module = "tools/exo-dev/src/lint/clang_tidy/input_identity.rs";
        let dir = crate::test_support::fixture_repo_committed(&[(module, "old\n")]);
        crate::test_support::write_files(dir.path(), &[(module, "changed\n")]);
        let root = normalize_root(dir.path());
        let sources: Vec<String> = CANARY_SOURCES
            .iter()
            .map(|source| format!("{root}/{source}"))
            .collect();
        let scope = scope_for_base(
            dir.path(),
            &root,
            &format!("{root}/build"),
            "HEAD",
            &sources,
            &HashMap::new(),
            &FakeNinja {
                whole: None,
                batched: None,
            },
            &mut TidyTiming::default(),
        )
        .unwrap();
        assert!(
            matches!(scope, BaseScope::Scoped { sources: selected, .. } if selected.len() == CANARY_SOURCES.len())
        );
    }

    #[test]
    fn mixed_compile_configuration_and_header_membership_changes_disable_replay() {
        let dir = crate::test_support::fixture_repo_committed(&[("CMakeLists.txt", "old\n")]);
        crate::test_support::write_files(
            dir.path(),
            &[
                ("CMakeLists.txt", "changed\n"),
                ("libs/shadow.h", "int shadow;\n"),
            ],
        );
        assert_eq!(
            crate::git::Git::new(dir.path())
                .run(&["add", "libs/shadow.h"])
                .0,
            0
        );
        let root = normalize_root(dir.path());
        let sources = vec![format!("{root}/libs/a.cpp")];
        let scope = scope_for_base(
            dir.path(),
            &root,
            &format!("{root}/build"),
            "HEAD",
            &sources,
            &HashMap::new(),
            &FakeNinja {
                whole: None,
                batched: None,
            },
            &mut TidyTiming::default(),
        )
        .unwrap();
        assert!(
            matches!(scope, BaseScope::Scoped { dep_graph: Some(graph), .. } if graph.is_empty())
        );
    }

    #[test]
    fn foreign_checkout_dependency_records_cannot_hide_changed_header_consumers() {
        let dir = crate::test_support::fixture_repo_committed(&[("libs/a.h", "old\n")]);
        let foreign = dir.path().join("other");
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
        crate::test_support::write_files(dir.path(), &[("libs/a.h", "changed\n")]);
        let root = normalize_root(dir.path());
        let source = format!("{root}/libs/a.cpp");
        let objects = HashMap::from([("a.obj".into(), source.clone())]);
        let ninja = FakeNinja {
            whole: Some(vec![
                "a.obj: #deps 1, deps mtime 1 (VALID)".into(),
                format!("    {}", foreign.join("libs/a.h").display()),
            ]),
            batched: None,
        };
        let scope = scope_for_base(
            dir.path(),
            &root,
            &format!("{root}/build"),
            "HEAD",
            std::slice::from_ref(&source),
            &objects,
            &ninja,
            &mut TidyTiming::default(),
        )
        .unwrap();
        assert!(
            matches!(scope, BaseScope::Scoped { sources, dep_graph: Some(graph), .. } if sources == vec![source] && graph.is_empty())
        );
    }

    #[test]
    #[cfg(windows)]
    fn a_failing_version_query_is_not_a_cache_identity() {
        let tool = std::env::var_os("COMSPEC").expect("Windows command processor");
        let mut command = crate::process::command(&tool.to_string_lossy());
        command.args(["/d", "/c", "echo version 1.2.3 & exit /b 1"]);
        let (code, stdout) = crate::process::query(command).unwrap();
        assert_ne!(code, 0);
        assert!(version_query_result(Path::new(&tool), code, stdout).is_err());
    }

    #[test]
    fn incomplete_verdict_evidence_cannot_be_cached_or_accepted() {
        let dir = tempfile::tempdir().unwrap();
        let source = normalize_root(&dir.path().join("a.cpp"));
        std::fs::write(&source, "int a();\n").unwrap();
        assert!(
            build_cache_keys(
                std::slice::from_ref(&source),
                &HashMap::from([(source.clone(), vec![])]),
                &HashMap::new(),
                "salt"
            )
            .is_empty()
        );
    }

    #[test]
    fn incomplete_diagnostic_evidence_fails_analysis() {
        for output in [
            "a.cpp:1:1: warning: incomplete diagnostic",
            "a.cpp:1:1: warning: moved [bugprone-use-after-move",
            "a.cpp:1:1: fatal error: incomplete diagnostic",
        ] {
            assert!(require_analysis("a.cpp", output, 0).is_err(), "{output}");
        }
    }

    #[test]
    fn renamed_headers_disable_the_prior_dependency_cache() {
        let dir = crate::test_support::fixture_repo_committed(&[("libs/a.h", "old\n")]);
        assert_eq!(
            crate::git::Git::new(dir.path())
                .run(&["mv", "libs/a.h", "libs/b.h"])
                .0,
            0
        );
        let root = normalize_root(dir.path());
        let sources = vec![format!("{root}/libs/a.cpp")];
        let objects = HashMap::from([("a.obj".into(), sources[0].clone())]);
        let scope = scope_for_base(
            dir.path(),
            &root,
            &format!("{root}/build"),
            "HEAD",
            &sources,
            &objects,
            &FakeNinja {
                whole: Some(vec![
                    "a.obj: #deps 1, deps mtime 1 (VALID)".into(),
                    format!("    {root}/libs/a.h"),
                ]),
                batched: None,
            },
            &mut TidyTiming::default(),
        )
        .unwrap();
        assert!(
            matches!(scope, BaseScope::Scoped { sources: selected, dep_graph: Some(graph), .. } if selected == sources && graph.is_empty())
        );
    }

    #[test]
    fn cache_replay_rejects_unverified_and_truncated_output() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("entry.txt");
        for invalid in [
            "",
            "cached output\n",
            "{\"schema\":",
            "error: invalid argument\n",
        ] {
            std::fs::write(&path, invalid).unwrap();
            assert!(
                read_cache_entry(&path, "abcdef", "source.cpp").is_none(),
                "{invalid:?}"
            );
        }
    }

    #[test]
    fn ninja_dependencies_reject_truncated_and_stale_records() {
        let map = HashMap::from([("a.obj".into(), "C:/repo/a.cpp".into())]);
        for header in [
            "a.obj: #deps 2, deps mtime 1 (VALID)",
            "a.obj: #deps 1, deps mtime 1 (STALE)",
            "a.obj: #deps 0, deps mtime 1 (VALID)",
        ] {
            let lines = vec![header.into(), "    a.h".into()];
            assert!(
                parse_ninja_deps(&lines, &map, "C:/repo").is_empty(),
                "{header}"
            );
        }
    }

    #[test]
    fn ninja_dependencies_require_every_command_variant() {
        let map = HashMap::from([
            ("a.obj".into(), "C:/repo/a.cpp".into()),
            ("other.obj".into(), "C:/repo/a.cpp".into()),
        ]);
        let lines = vec![
            "a.obj: #deps 1, deps mtime 1 (VALID)".into(),
            "    a.h".into(),
        ];
        assert!(parse_ninja_deps(&lines, &map, "C:/repo").is_empty());
    }

    #[test]
    fn compile_arguments_preserve_argument_boundaries() {
        let one = parse_compile_db(
            r#"[{"file":"a.cpp","directory":"C:/repo","arguments":["cl.exe","-DVALUE=a b"]}]"#,
        )
        .unwrap();
        let two = parse_compile_db(
            r#"[{"file":"a.cpp","directory":"C:/repo","arguments":["cl.exe","-DVALUE=a","b"]}]"#,
        )
        .unwrap();
        assert_ne!(one[0].command_line, two[0].command_line);
    }

    #[test]
    fn every_compile_command_contributes_to_the_identity() {
        let mut entries = vec![
            entry("C:/repo/a.cpp", "a.obj", "first"),
            entry("C:/repo/a.cpp", "b.obj", "last"),
        ];
        let before = command_identities(&entries);
        entries[0].command_line = "changed first command".into();
        assert_ne!(before, command_identities(&entries));
        let (_, objects) = object_maps(&entries, "C:/repo/build");
        assert_eq!(objects["C:/repo/a.cpp"].len(), 2);
    }

    #[test]
    fn a_missing_output_in_one_command_leaves_the_source_uncovered() {
        let entries = parse_compile_db(
            r#"[
            {"file":"C:/repo/a.cpp","directory":"C:/repo/build","output":"a.obj","command":"cl.exe a.cpp"},
            {"file":"C:/repo/a.cpp","directory":"C:/repo/build","command":"cl.exe /DOTHER a.cpp"}
        ]"#,
        )
        .unwrap();
        let (objects, sources) = object_maps(&entries, "C:/repo/build");
        assert!(!sources.contains_key("C:/repo/a.cpp"));
        assert!(objects.is_empty());
    }

    #[test]
    fn cache_replay_checks_identity_integrity_and_analysis_status() {
        let dir = tempfile::tempdir().unwrap();
        let key = "abcdef0123456789";
        let source = "source.cpp";
        store_cache_entry(dir.path(), key, source, "", 0);
        let path = cache_entry_path(dir.path(), key);
        assert_eq!(read_cache_entry(&path, key, source), Some(String::new()));
        let original: serde_json::Value =
            serde_json::from_slice(&std::fs::read(&path).unwrap()).unwrap();
        for (field, value) in [
            ("schema", serde_json::json!(CACHE_SCHEMA - 1)),
            ("key", serde_json::json!("other")),
            ("source", serde_json::json!("other.cpp")),
            ("exit_code", serde_json::json!(-1)),
            ("exit_code", serde_json::json!(1)),
            ("output", serde_json::json!("truncated")),
            ("output_sha256", serde_json::json!("wrong")),
        ] {
            let mut damaged = original.clone();
            damaged[field] = value;
            std::fs::write(&path, serde_json::to_vec(&damaged).unwrap()).unwrap();
            assert!(read_cache_entry(&path, key, source).is_none(), "{field}");
        }
        let mut failed_parse = original;
        let output = "source.cpp:1:1: error: cannot parse [clang-diagnostic-error]";
        failed_parse["output"] = serde_json::json!(output);
        failed_parse["output_sha256"] = serde_json::json!(sha256_hex(output.as_bytes()));
        std::fs::write(&path, serde_json::to_vec(&failed_parse).unwrap()).unwrap();
        assert!(read_cache_entry(&path, key, source).is_none());
    }

    #[test]
    fn cached_blocking_findings_preserve_their_verdict() {
        let dir = tempfile::tempdir().unwrap();
        let key = "abcdef0123456789";
        let source = "C:/repo/libs/engine/a.cpp";
        let output =
            format!("{source}:1:1: error: moved [bugprone-use-after-move,-warnings-as-errors]");
        store_cache_entry(dir.path(), key, source, &output, 1);
        let replayed = read_cache_entry(&cache_entry_path(dir.path(), key), key, source).unwrap();
        assert_eq!(
            extract_violations(&[(source.into(), replayed)], "C:/repo", "C:/repo/build").len(),
            1
        );
    }

    #[test]
    fn schema_three_cache_entries_are_rejected_without_input_stability_proof() {
        let dir = tempfile::tempdir().unwrap();
        let key = "abcdef0123456789";
        let source = "source.cpp";
        store_cache_entry(dir.path(), key, source, "", 0);
        let path = cache_entry_path(dir.path(), key);
        let mut entry: serde_json::Value =
            serde_json::from_slice(&std::fs::read(&path).unwrap()).unwrap();
        entry["schema"] = serde_json::json!(3);
        std::fs::write(&path, serde_json::to_vec(&entry).unwrap()).unwrap();
        assert!(read_cache_entry(&path, key, source).is_none());
    }

    #[test]
    fn persistent_input_drift_aborts_before_any_fresh_cache_write() {
        for changed in [
            "unchanged",
            "source",
            "header",
            "database",
            "configuration",
            "membership",
            "toolchain",
            "missing-source",
            "missing-configuration",
        ] {
            let dir = tempfile::tempdir().unwrap();
            let repo = dir.path();
            let build = repo.join("build");
            std::fs::create_dir_all(repo.join("libs")).unwrap();
            std::fs::create_dir(&build).unwrap();
            std::fs::write(repo.join("libs/shared.h"), "int shared;\n").unwrap();
            std::fs::write(repo.join(".clang-tidy"), "Checks: '-*'\n").unwrap();
            std::fs::write(repo.join("compiler-input"), "compiler").unwrap();
            let sources: Vec<String> = ["a.cpp", "b.cpp"]
                .into_iter()
                .map(|file| {
                    let path = repo.join("libs").join(file);
                    std::fs::write(&path, "int value;\n").unwrap();
                    normalize_root(&path)
                })
                .collect();
            let records: Vec<_> = sources.iter().map(|source| serde_json::json!({"file": source, "directory": normalize_root(&build), "command": "cl.exe /c"})).collect();
            let database = serde_json::to_string(&records).unwrap();
            let database_path = build.join("compile_commands.json");
            std::fs::write(&database_path, &database).unwrap();
            let commands = command_identities(&parse_compile_db(&database).unwrap());
            let dependencies: HashMap<_, _> = sources
                .iter()
                .map(|source| {
                    (
                        source.clone(),
                        vec![normalize_root(&repo.join("libs/shared.h"))],
                    )
                })
                .collect();
            let identity = || {
                combine_configuration_identity(
                    input_identity::configuration_hash(repo, &build),
                    hash_file(&normalize_root(&repo.join("compiler-input"))),
                )
            };
            let configuration = identity().unwrap();
            let salt = compute_salt("version", &[], Some(&configuration)).unwrap();
            let keys = build_cache_keys(&sources, &dependencies, &commands, &salt);
            match changed {
                "source" => std::fs::write(&sources[1], "int changed;\n").unwrap(),
                "header" => std::fs::write(repo.join("libs/shared.h"), "int changed;\n").unwrap(),
                "database" => std::fs::write(&database_path, "[]").unwrap(),
                "configuration" => {
                    std::fs::write(repo.join(".clang-tidy"), "Checks: '*'\n").unwrap()
                }
                "membership" => {
                    std::fs::write(repo.join("libs/new.h"), "int new_header;\n").unwrap()
                }
                "toolchain" => {
                    std::fs::write(repo.join("compiler-input"), "changed compiler").unwrap()
                }
                "missing-source" => std::fs::remove_file(&sources[1]).unwrap(),
                "missing-configuration" => std::fs::remove_file(repo.join(".clang-tidy")).unwrap(),
                _ => {}
            }
            let current = identity();
            let checked = revalidate_cache_inputs(
                &database_path,
                &database,
                &configuration,
                current.as_deref(),
                &sources,
                &dependencies,
                &commands,
                &salt,
                &keys,
            );
            let cache = repo.join("cache");
            if checked.is_ok() {
                for source in &sources {
                    store_cache_entry(&cache, &keys[source], source, "", 0);
                }
            }
            if changed == "unchanged" {
                checked.unwrap();
                for source in &sources {
                    assert!(
                        read_cache_entry(
                            &cache_entry_path(&cache, &keys[source]),
                            &keys[source],
                            source
                        )
                        .is_some()
                    );
                }
            } else {
                assert!(checked.is_err(), "{changed}");
                assert!(
                    !cache.exists(),
                    "no earlier fresh result may be stored when another input drifts: {changed}"
                );
            }
        }
    }

    #[cfg(windows)]
    #[test]
    #[ignore = "requires EXO_DEV_UCRT_CLANG_TIDY and a matching MSVC INCLUDE environment"]
    fn ucrt_offsetof_layout_contracts_are_constant_expressions() {
        let tool = PathBuf::from(
            std::env::var_os("EXO_DEV_UCRT_CLANG_TIDY").expect("set EXO_DEV_UCRT_CLANG_TIDY"),
        );
        assert!(std::env::var_os("INCLUDE").is_some(), "set MSVC INCLUDE");
        let dir = tempfile::tempdir().unwrap();
        let source = normalize_root(&dir.path().join("layout.cpp"));
        std::fs::write(
            &source,
            "#include <cstddef>\nstruct Constants { float key[4]; float params[4]; float params2[4]; };\nstatic_assert(sizeof(Constants) == 48);\nstatic_assert(offsetof(Constants, params) == 16);\nstatic_assert(offsetof(Constants, params2) == 32);\n",
        )
        .unwrap();
        let database = serde_json::json!([{
            "directory": normalize_root(dir.path()),
            "file": source,
            "command": format!("clang-cl.exe /TP /std:c++20 /c \"{source}\"")
        }]);
        std::fs::write(
            dir.path().join("compile_commands.json"),
            serde_json::to_vec(&database).unwrap(),
        )
        .unwrap();
        let flags = invocation_flags(&format!("-*,{}", BLOCKING_CHECKS.join(",")), HEADER_FILTER);
        let (output, code, _) = invoke_clang_tidy(&tool, dir.path(), &flags, &source);
        require_analysis(&source, &output, code).expect(&output);
        assert_eq!(code, 0, "{output}");
    }
    #[test]
    #[ignore = "requires EXO_DEV_DRIFT_CLANG_TIDY naming a real clang-tidy executable"]
    fn a_source_changed_after_key_capture_cannot_store_a_clean_verdict_for_the_original() {
        let tool = PathBuf::from(
            std::env::var_os("EXO_DEV_DRIFT_CLANG_TIDY").expect("set EXO_DEV_DRIFT_CLANG_TIDY"),
        );
        let dir = tempfile::tempdir().unwrap();
        let repo = dir.path();
        let build = repo.join("build");
        std::fs::create_dir_all(repo.join("libs")).unwrap();
        std::fs::create_dir(&build).unwrap();
        let source = normalize_root(&repo.join("libs/drift.cpp"));
        let original = include_str!(
            "../../tests/fixtures/lint-canaries/clang-analyzer-core.CallAndMessage.cpp"
        );
        std::fs::write(&source, original).unwrap();
        let database = serde_json::json!([{"directory": normalize_root(&build), "file": source, "command": format!("clang-cl.exe /TP /std:c++20 /c \"{source}\"")}]).to_string();
        std::fs::write(build.join("compile_commands.json"), &database).unwrap();
        let entries = parse_compile_db(&database).unwrap();
        let commands = command_identities(&entries);
        let sources = vec![source.clone()];
        let dependencies = HashMap::from([(source.clone(), vec![source.clone()])]);
        let flags = invocation_flags(&format!("-*,{}", BLOCKING_CHECKS.join(",")), HEADER_FILTER);
        let banner = clang_tidy_version_banner(&tool).unwrap();
        let salt = compute_salt(&banner, &flags, Some("stable-configuration")).unwrap();
        let keys = build_cache_keys(&sources, &dependencies, &commands, &salt);
        let key = &keys[&source];
        let initial = analyze_pending(&tool, &build, &flags, &sources, 1);
        require_analysis(&source, &initial[0].1, initial[0].2).unwrap();
        let findings = extract_violations(
            &[(source.clone(), initial[0].1.clone())],
            &normalize_root(repo),
            &normalize_root(&build),
        );
        assert!(
            findings
                .iter()
                .any(|finding| finding.contains("clang-analyzer-core.CallAndMessage")),
            "original null-pointer call must be blocking: {}",
            initial[0].1
        );
        eprintln!("original source: {} blocking finding(s)", findings.len());

        std::fs::write(&source, "int value() { return 1; }\n").unwrap();
        let fresh = analyze_pending(&tool, &build, &flags, &sources, 1);
        require_analysis(&source, &fresh[0].1, fresh[0].2).unwrap();
        assert!(
            extract_violations(
                &[(source.clone(), fresh[0].1.clone())],
                &normalize_root(repo),
                &normalize_root(&build)
            )
            .is_empty(),
            "changed source must analyze cleanly: {}",
            fresh[0].1
        );
        let cache = repo.join("cache");
        let checked = revalidate_cache_inputs(
            &build.join("compile_commands.json"),
            &database,
            "stable-configuration",
            Some("stable-configuration"),
            &sources,
            &dependencies,
            &commands,
            &salt,
            &keys,
        );
        if checked.is_ok() {
            store_cache_entry(&cache, key, &source, &fresh[0].1, fresh[0].2);
        }
        assert!(
            checked.is_err(),
            "changed source must abort before storing any fresh output"
        );
        std::fs::write(&source, original).unwrap();
        assert_eq!(
            build_cache_keys(&sources, &dependencies, &commands, &salt),
            keys
        );
        let replayed = read_cache_entry(&cache_entry_path(&cache, key), key, &source);
        if let Some(output) = &replayed {
            assert!(
                extract_violations(
                    &[(source.clone(), output.clone())],
                    &normalize_root(repo),
                    &normalize_root(&build)
                )
                .is_empty()
            );
            eprintln!(
                "restored original source incorrectly replays the clean changed-source verdict under key {key}"
            );
        }
        assert!(
            replayed.is_none(),
            "a changed input must prevent storing the fresh verdict under its prior key"
        );
    }

    #[test]
    fn header_appearance_changes_select_all_sources_without_cache_keys() {
        for added in [true, false] {
            let dir = crate::test_support::fixture_repo_committed(&[("libs/a.h", "old\n")]);
            if added {
                crate::test_support::write_files(dir.path(), &[("libs/feature.h", "new\n")]);
                assert_eq!(
                    crate::git::Git::new(dir.path())
                        .run(&["add", "libs/feature.h"])
                        .0,
                    0
                );
            } else {
                std::fs::remove_file(dir.path().join("libs/a.h")).unwrap();
            }
            let root = normalize_root(dir.path());
            let sources = vec![format!("{root}/libs/a.cpp"), format!("{root}/app/b.cpp")];
            let scope = scope_for_base(
                dir.path(),
                &root,
                &format!("{root}/build"),
                "HEAD",
                &sources,
                &HashMap::new(),
                &FakeNinja {
                    whole: None,
                    batched: None,
                },
                &mut TidyTiming::default(),
            )
            .unwrap();
            assert!(
                matches!(scope, BaseScope::Scoped { sources: selected, dep_graph: Some(graph), .. } if selected == sources && graph.is_empty())
            );
        }
    }

    struct BatchNinja {
        current: bool,
    }

    impl NinjaDeps for BatchNinja {
        fn whole_graph(&self) -> Option<Vec<String>> {
            self.for_objects(
                &(0..101)
                    .map(|index| format!("a{index}.obj"))
                    .collect::<Vec<_>>(),
            )
        }
        fn objects_current(&self, _objects: &[String]) -> bool {
            self.current
        }
        fn for_objects(&self, objects: &[String]) -> Option<Vec<String>> {
            Some(
                objects
                    .iter()
                    .flat_map(|object| {
                        [
                            format!("{object}: #deps 1, deps mtime 1 (VALID)"),
                            format!("    {object}.h"),
                        ]
                    })
                    .collect(),
            )
        }
    }

    #[test]
    fn every_object_variant_survives_dependency_batches() {
        let source = "C:/repo/a.cpp".to_string();
        let objects: Vec<String> = (0..101).map(|index| format!("a{index}.obj")).collect();
        let source_to_obj = HashMap::from([(source.clone(), objects.clone())]);
        let obj_to_source = objects
            .into_iter()
            .map(|object| (object, source.clone()))
            .collect();
        let graph = deps_for_sources(
            &BatchNinja { current: true },
            std::slice::from_ref(&source),
            &source_to_obj,
            &obj_to_source,
            "C:/repo/build",
            None,
        )
        .unwrap();
        assert_eq!(graph[&source].len(), 101);
        assert!(
            deps_for_sources(
                &BatchNinja { current: false },
                &[source],
                &source_to_obj,
                &obj_to_source,
                "C:/repo/build",
                None
            )
            .is_none()
        );
    }

    #[test]
    fn large_selections_read_the_whole_graph_once_with_equal_dependencies() {
        struct CountingNinja {
            whole: std::cell::Cell<usize>,
            batches: std::cell::Cell<usize>,
            objects: Vec<String>,
        }
        impl NinjaDeps for CountingNinja {
            fn objects_current(&self, _objects: &[String]) -> bool {
                true
            }
            fn whole_graph(&self) -> Option<Vec<String>> {
                self.whole.set(self.whole.get() + 1);
                BatchNinja { current: true }.for_objects(&self.objects)
            }
            fn for_objects(&self, objects: &[String]) -> Option<Vec<String>> {
                self.batches.set(self.batches.get() + 1);
                BatchNinja { current: true }.for_objects(objects)
            }
        }
        for count in [999, 1000] {
            let source = "C:/repo/a.cpp".to_string();
            let objects: Vec<String> = (0..count).map(|index| format!("a{index}.obj")).collect();
            let source_to_obj = HashMap::from([(source.clone(), objects.clone())]);
            let obj_to_source = objects
                .iter()
                .cloned()
                .map(|object| (object, source.clone()))
                .collect();
            let expected = parse_ninja_deps(
                &BatchNinja { current: true }.for_objects(&objects).unwrap(),
                &obj_to_source,
                "C:/repo/build",
            );
            let ninja = CountingNinja {
                whole: std::cell::Cell::new(0),
                batches: std::cell::Cell::new(0),
                objects,
            };
            let actual = deps_for_sources(
                &ninja,
                &[source],
                &source_to_obj,
                &obj_to_source,
                "C:/repo/build",
                None,
            )
            .unwrap();
            assert_eq!(actual, expected);
            assert_eq!(ninja.whole.get(), usize::from(count >= 1000));
            assert_eq!(ninja.batches.get(), if count < 1000 { 10 } else { 0 });
        }
    }

    #[test]
    fn every_object_variants_headers_contribute_to_the_key() {
        let dir = tempfile::tempdir().unwrap();
        let source_path = dir.path().join("a.cpp");
        let first_header = dir.path().join("first.h");
        let other_header = dir.path().join("other.h");
        for path in [&source_path, &first_header, &other_header] {
            std::fs::write(path, "old\n").unwrap();
        }
        let source = normalize_root(&source_path);
        let map = HashMap::from([
            ("first.obj".into(), source.clone()),
            ("other.obj".into(), source.clone()),
        ]);
        let graph = parse_ninja_deps(
            &[
                "first.obj: #deps 1, deps mtime 1 (VALID)".into(),
                format!("    {}", first_header.display()),
                "other.obj: #deps 1, deps mtime 1 (VALID)".into(),
                format!("    {}", other_header.display()),
            ],
            &map,
            &normalize_root(dir.path()),
        );
        let commands = HashMap::from([(source.clone(), "commands".into())]);
        let before = build_cache_keys(std::slice::from_ref(&source), &graph, &commands, "salt");
        std::fs::write(first_header, "changed\n").unwrap();
        assert_ne!(
            before,
            build_cache_keys(&[source], &graph, &commands, "salt")
        );
    }

    #[test]
    fn a_compile_configuration_change_analyzes_the_full_tree() {
        for config in [
            "CMakeLists.txt",
            "libs/foo/CMakeLists.txt",
            "cmake/toolchain.cmake",
            "CMakePresets.json",
        ] {
            let dir = crate::test_support::fixture_repo_committed(&[(config, "old\n")]);
            crate::test_support::write_files(dir.path(), &[(config, "changed\n")]);
            let root = normalize_root(dir.path());
            let sources = vec![format!("{root}/libs/a.cpp"), format!("{root}/app/b.cpp")];
            let scope = scope_for_base(
                dir.path(),
                &root,
                &format!("{root}/build"),
                "HEAD",
                &sources,
                &HashMap::new(),
                &FakeNinja {
                    whole: None,
                    batched: None,
                },
                &mut TidyTiming::default(),
            )
            .unwrap();
            assert!(
                matches!(scope, BaseScope::Scoped { sources: selected, .. } if selected == sources),
                "{config}"
            );
        }
    }

    #[test]
    fn a_partial_dependency_graph_cannot_skip_header_consumers() {
        let dir = crate::test_support::fixture_repo_committed(&[("libs/a.h", "old\n")]);
        crate::test_support::write_files(dir.path(), &[("libs/a.h", "changed\n")]);
        let root = normalize_root(dir.path());
        let sources = vec![format!("{root}/libs/a.cpp"), format!("{root}/libs/b.cpp")];
        let map = HashMap::from([
            ("a.obj".into(), sources[0].clone()),
            ("b.obj".into(), sources[1].clone()),
        ]);
        let ninja = FakeNinja {
            whole: Some(vec!["a.obj: #deps 0, deps mtime 1 (VALID)".into()]),
            batched: None,
        };
        let scope = scope_for_base(
            dir.path(),
            &root,
            &format!("{root}/build"),
            "HEAD",
            &sources,
            &map,
            &ninja,
            &mut TidyTiming::default(),
        )
        .unwrap();
        assert!(
            matches!(scope, BaseScope::Scoped { sources: selected, .. } if selected == vec![sources[1].clone()])
        );
    }

    #[test]
    fn scoped_findings_survive_windows_path_case_changes() {
        for file in [
            "c:/repo/libs/engine/file.cpp",
            "C:/repo/LIBS/ENGINE/file.cpp",
        ] {
            let output = format!("{file}:1:1: warning: missing provider [misc-include-cleaner]");
            assert_eq!(
                extract_violations(&[("file.cpp".into(), output)], "C:/repo", "C:/repo/build")
                    .len(),
                1,
                "{file}"
            );
        }
    }

    #[test]
    fn blocking_analysis_rejects_crashes_and_unparsed_translation_units() {
        for (output, code) in [
            ("", -1),
            ("", 2),
            ("error: invalid argument", 1),
            ("file.cpp:1:1: error: syntax [clang-diagnostic-error]", 1),
            (
                "file.cpp:1:1: fatal error: missing header [clang-diagnostic-error]",
                0,
            ),
        ] {
            assert!(
                require_analysis("file.cpp", output, code).is_err(),
                "{output}, {code}"
            );
        }
        assert!(require_analysis("file.cpp", "", 0).is_ok());
        assert!(
            require_analysis(
                "file.cpp",
                "file.cpp:1:1: error: violation [bugprone-use-after-move,-warnings-as-errors]",
                1
            )
            .is_ok()
        );
    }

    #[test]
    fn include_cleaner_blocks_core_libraries_but_remains_advisory_in_app() {
        let output = ["libs/engine", "libs/capability", "libs/update", "app"]
            .map(|directory| format!("C:/repo/{directory}/file.cpp:1:1: warning: missing provider [misc-include-cleaner]"))
            .join("\n");
        let findings =
            extract_violations(&[("file.cpp".into(), output)], "C:/repo", "C:/repo/build");
        assert_eq!(findings.len(), 3);
        assert!(findings.iter().all(|finding| finding.starts_with("libs/")));
    }

    #[test]
    fn the_compiler_installation_is_read_from_a_quoted_compiler_path() {
        let line = r#"C:/repo/build|"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64\cl.exe"  /nologo /TP"#;
        assert_eq!(
            compiler_installation(line),
            Some(PathBuf::from(
                r"C:\Program Files\Microsoft Visual Studio\18\Community"
            ))
        );
    }

    #[test]
    fn the_compiler_installation_is_read_from_an_unquoted_forward_slash_path() {
        let line =
            "C:/repo/build|C:/BuildTools/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe /nologo";
        assert_eq!(
            compiler_installation(line),
            Some(PathBuf::from(r"C:\BuildTools"))
        );
    }

    #[test]
    fn a_compiler_outside_a_visual_studio_layout_names_no_installation() {
        assert_eq!(
            compiler_installation(r"C:/repo/build|C:\LLVM\bin\clang-cl.exe /nologo"),
            None
        );
        assert_eq!(compiler_installation("C:/repo/build|"), None);
    }

    fn entry(file: &str, output: &str, command: &str) -> CompileEntry {
        CompileEntry {
            file: file.to_string(),
            output: Some(output.to_string()),
            command_line: command.to_string(),
        }
    }

    #[test]
    fn project_sources_excludes_build_tree_and_third_party() {
        let entries = vec![
            entry("C:/repo/libs/engine/a.cpp", "libs/engine/a.obj", "cmd"),
            entry(
                "C:/repo/build/windows-x64-ninja-debug/generated/moc.cpp",
                "generated/moc.obj",
                "cmd",
            ),
            entry(
                "C:/repo/third_party/vendor/x.cpp",
                "third_party/vendor/x.obj",
                "cmd",
            ),
            entry("C:/repo/libs/engine/a.h", "n/a", "cmd"),
        ];
        let sources = project_sources(&entries, "C:/repo", "C:/repo/build/windows-x64-ninja-debug");
        assert_eq!(sources, vec!["C:/repo/libs/engine/a.cpp".to_string()]);
    }

    #[test]
    fn resolve_against_build_dir_collapses_parent_segments() {
        let resolved = resolve_against_build_dir(
            "../../libs/engine/a.h",
            "C:/repo/build/windows-x64-ninja-debug",
        );
        assert_eq!(resolved, "C:/repo/libs/engine/a.h");
    }

    #[test]
    fn resolve_against_build_dir_leaves_an_already_rooted_path_alone() {
        let resolved = resolve_against_build_dir(
            "C:/repo/libs/engine/a.h",
            "C:/repo/build/windows-x64-ninja-debug",
        );
        assert_eq!(resolved, "C:/repo/libs/engine/a.h");
    }

    #[test]
    fn parse_ninja_deps_reads_a_header_consumer_relationship() {
        let mut obj_to_source = HashMap::new();
        obj_to_source.insert(
            "libs/engine/a.obj".to_string(),
            "C:/repo/libs/engine/a.cpp".to_string(),
        );
        let lines: Vec<String> = [
            "libs/engine/a.obj: #deps 1, deps mtime 1 (VALID)",
            "    ../../libs/engine/a.h",
            "",
        ]
        .into_iter()
        .map(str::to_string)
        .collect();
        let graph = parse_ninja_deps(
            &lines,
            &obj_to_source,
            "C:/repo/build/windows-x64-ninja-debug",
        );
        assert_eq!(
            graph.get("C:/repo/libs/engine/a.cpp").unwrap(),
            &vec!["C:/repo/libs/engine/a.h".to_string()]
        );
    }

    #[test]
    fn parse_ninja_deps_skips_objects_outside_the_map() {
        let obj_to_source = HashMap::new();
        let lines: Vec<String> = [
            "unmapped.obj: #deps 1, deps mtime 1 (VALID)",
            "    some/header.h",
        ]
        .into_iter()
        .map(str::to_string)
        .collect();
        let graph = parse_ninja_deps(&lines, &obj_to_source, "C:/repo/build");
        assert!(graph.is_empty());
    }

    #[test]
    fn check_matches_supports_a_trailing_wildcard() {
        assert!(check_matches(
            "clang-analyzer-cplusplus.NewDeleteLeaks",
            "clang-analyzer-cplusplus.NewDelete*"
        ));
        assert!(!check_matches(
            "clang-analyzer-cplusplus.Other",
            "clang-analyzer-cplusplus.NewDelete*"
        ));
        assert!(check_matches(
            "bugprone-use-after-move",
            "bugprone-use-after-move"
        ));
        assert!(!check_matches(
            "bugprone-use-after-move-x",
            "bugprone-use-after-move"
        ));
    }

    #[test]
    fn extract_violations_keeps_only_blocking_checks_in_repo_files() {
        let results = vec![(
            "src".to_string(),
            "C:/repo/libs/engine/a.cpp:10:5: warning: use after move [bugprone-use-after-move]\n\
                 C:/repo/libs/engine/a.cpp:11:5: warning: something else [readability-other]\n\
                 C:/Qt/include/qobject.h:1:1: warning: irrelevant [bugprone-use-after-move]\n"
                .to_string(),
        )];
        let violations =
            extract_violations(&results, "C:/repo", "C:/repo/build/windows-x64-ninja-debug");
        assert_eq!(violations.len(), 1);
        assert!(
            violations
                .iter()
                .next()
                .unwrap()
                .starts_with("libs/engine/a.cpp:10:5")
        );
    }

    #[test]
    fn extract_violations_strips_the_warnings_as_errors_suffix() {
        let results = vec![(
            "src".to_string(),
            "C:/repo/libs/engine/a.cpp:1:1: error: boom [bugprone-use-after-move,-warnings-as-errors]\n"
                .to_string(),
        )];
        let violations =
            extract_violations(&results, "C:/repo", "C:/repo/build/windows-x64-ninja-debug");
        assert_eq!(violations.len(), 1);
        assert!(
            violations
                .iter()
                .next()
                .unwrap()
                .contains("[bugprone-use-after-move]")
        );
    }

    struct FakeNinja {
        whole: Option<Vec<String>>,
        batched: Option<Vec<String>>,
    }

    impl NinjaDeps for FakeNinja {
        fn whole_graph(&self) -> Option<Vec<String>> {
            self.whole.clone()
        }
        fn for_objects(&self, _objects: &[String]) -> Option<Vec<String>> {
            self.batched.clone()
        }
        fn objects_current(&self, _objects: &[String]) -> bool {
            true
        }
    }

    #[test]
    fn deps_for_sources_reuses_an_already_read_whole_graph() {
        let mut already = HashMap::new();
        already.insert("src.cpp".to_string(), vec!["hdr.h".to_string()]);
        let ninja = FakeNinja {
            whole: None,
            batched: None,
        };
        let source_to_obj = HashMap::new();
        let obj_to_source = HashMap::new();
        let graph = deps_for_sources(
            &ninja,
            &["src.cpp".to_string()],
            &source_to_obj,
            &obj_to_source,
            "C:/repo/build",
            Some(&already),
        )
        .unwrap();
        assert_eq!(graph.get("src.cpp").unwrap(), &vec!["hdr.h".to_string()]);
    }

    #[test]
    fn deps_for_sources_is_none_when_ninja_cannot_answer() {
        let ninja = FakeNinja {
            whole: None,
            batched: None,
        };
        let mut source_to_obj = HashMap::new();
        source_to_obj.insert("src.cpp".to_string(), vec!["src.obj".to_string()]);
        let obj_to_source = HashMap::new();
        let graph = deps_for_sources(
            &ninja,
            &["src.cpp".to_string()],
            &source_to_obj,
            &obj_to_source,
            "C:/repo/build",
            None,
        );
        assert!(graph.is_none());
    }

    /// `-Base` scoping's core promise: a changed header must reach the real
    /// translation unit that includes it, resolved from a (here, fake)
    /// Ninja dependency graph, and a non-project source that ALSO happens to
    /// depend on the same header must not be pulled in by the
    /// `is_project_source` filter in that same loop.
    #[test]
    fn scope_for_base_expands_a_changed_header_to_its_project_source_consumer() {
        let dir = crate::test_support::fixture_repo_committed(&[
            ("libs/foo/src/foo.h", "int foo_value();\n"),
            (
                "libs/foo/src/foo.cpp",
                "#include \"foo.h\"\nint foo_value() { return 1; }\n",
            ),
            ("libs/foo/src/bar.cpp", "int bar() { return 2; }\n"),
        ]);
        let repo_root = dir.path();
        // An uncommitted edit: `git diff HEAD` (no --cached) sees it against
        // the working tree, exactly like the committed-vs-working-tree scope
        // the real caller runs against.
        crate::test_support::write_files(
            repo_root,
            &[("libs/foo/src/foo.h", "int foo_value(); // changed\n")],
        );

        let repo_root_norm = normalize_root(repo_root);
        let build_dir_norm = format!("{repo_root_norm}/build");

        let foo_cpp = format!("{repo_root_norm}/libs/foo/src/foo.cpp");
        let bar_cpp = format!("{repo_root_norm}/libs/foo/src/bar.cpp");
        let foo_h = format!("{repo_root_norm}/libs/foo/src/foo.h");
        // Not a project source (third_party/): must be excluded even though
        // its fake recorded dependency also names the changed header.
        let third_party_cpp = format!("{repo_root_norm}/third_party/vendor/x.cpp");

        let all_sources = vec![foo_cpp.clone(), bar_cpp.clone()];

        let mut obj_to_source = HashMap::new();
        obj_to_source.insert("foo.obj".to_string(), foo_cpp.clone());
        obj_to_source.insert("bar.obj".to_string(), bar_cpp.clone());
        obj_to_source.insert("thirdparty.obj".to_string(), third_party_cpp.clone());

        let lines: Vec<String> = [
            "foo.obj: #deps 1, deps mtime 1 (VALID)".to_string(),
            format!("    {foo_h}"),
            String::new(),
            "bar.obj: #deps 1, deps mtime 1 (VALID)".to_string(),
            format!("    {bar_cpp}"),
            String::new(),
            "thirdparty.obj: #deps 1, deps mtime 1 (VALID)".to_string(),
            format!("    {foo_h}"),
        ]
        .into_iter()
        .collect();
        let ninja = FakeNinja {
            whole: Some(lines),
            batched: None,
        };

        let scope = scope_for_base(
            repo_root,
            &repo_root_norm,
            &build_dir_norm,
            "HEAD",
            &all_sources,
            &obj_to_source,
            &ninja,
            &mut TidyTiming::default(),
        )
        .unwrap();

        let BaseScope::Scoped { sources, .. } = scope else {
            panic!("expected a scoped result, the header change is analysable");
        };
        assert_eq!(sources, vec![foo_cpp]);
    }

    #[cfg(windows)]
    #[test]
    fn current_header_consumers_survive_a_different_checkout_root_alias() {
        let dir = crate::test_support::fixture_repo_committed(&[
            ("libs/a.h", "int a;\n"),
            ("libs/a.cpp", "int a;\n"),
            ("libs/b.cpp", "int b;\n"),
        ]);
        crate::test_support::write_files(dir.path(), &[("libs/a.h", "int changed;\n")]);
        let alias = std::fs::canonicalize(dir.path()).unwrap();
        let root = normalize_root(&alias);
        let a = format!("{root}/libs/a.cpp");
        let b = format!("{root}/libs/b.cpp");
        let sources = vec![a.clone(), b.clone()];
        let objects = HashMap::from([("a.obj".into(), a.clone()), ("b.obj".into(), b)]);
        let ninja = FakeNinja {
            whole: Some(vec![
                "a.obj: #deps 1, deps mtime 1 (VALID)".into(),
                format!("    {}", dir.path().join("libs/a.h").display()),
                String::new(),
                "b.obj: #deps 1, deps mtime 1 (VALID)".into(),
                format!("    {}", dir.path().join("libs/b.cpp").display()),
            ]),
            batched: None,
        };
        let scope = scope_for_base(
            &alias,
            &root,
            &format!("{root}/build"),
            "HEAD",
            &sources,
            &objects,
            &ninja,
            &mut TidyTiming::default(),
        )
        .unwrap();
        assert!(
            matches!(scope, BaseScope::Scoped { sources, .. } if sources == vec![a]),
            "a current object using another root alias must retain only the changed header's consumer"
        );
    }

    /// The canary branch does not need a Ninja graph at all: a change to an
    /// analysis-configuration trigger (here `.clang-tidy`) pulls in the fixed
    /// canary translation units directly, by path, and a canary missing from
    /// `all_sources` is a hard error rather than a quietly smaller scope.
    #[test]
    fn scope_for_base_pulls_in_canary_sources_when_the_analysis_config_changed() {
        let dir = crate::test_support::fixture_repo_committed(&[
            (".clang-tidy", "Checks: >\n  old-check\n"),
            ("libs/engine/src/audio_thread.cpp", "// canary a\n"),
            (
                "app/quick/ExoSnap/Quick/QuickApplication.cpp",
                "// canary b\n",
            ),
            (
                "libs/engine/tests/test_split_sentinel_policy.cpp",
                "// canary c\n",
            ),
        ]);
        let repo_root = dir.path();
        crate::test_support::write_files(
            repo_root,
            &[(".clang-tidy", "Checks: >\n  old-check,\n  new-check\n")],
        );

        let repo_root_norm = normalize_root(repo_root);
        let build_dir_norm = format!("{repo_root_norm}/build");
        let all_sources: Vec<String> = CANARY_SOURCES
            .iter()
            .map(|c| format!("{repo_root_norm}/{c}"))
            .collect();
        let obj_to_source = HashMap::new();
        let ninja = FakeNinja {
            whole: None,
            batched: None,
        };

        let scope = scope_for_base(
            repo_root,
            &repo_root_norm,
            &build_dir_norm,
            "HEAD",
            &all_sources,
            &obj_to_source,
            &ninja,
            &mut TidyTiming::default(),
        )
        .unwrap();

        let BaseScope::Scoped { sources, .. } = scope else {
            panic!("expected a scoped result, the canary trigger changed");
        };
        let mut sorted = sources;
        sorted.sort();
        let mut expected = all_sources;
        expected.sort();
        assert_eq!(sorted, expected);
    }

    #[test]
    fn scope_for_base_refuses_a_canary_source_missing_from_the_compile_database() {
        let dir = crate::test_support::fixture_repo_committed(&[(
            ".clang-tidy",
            "Checks: >\n  old-check\n",
        )]);
        let repo_root = dir.path();
        crate::test_support::write_files(
            repo_root,
            &[(".clang-tidy", "Checks: >\n  old-check,\n  new-check\n")],
        );

        let repo_root_norm = normalize_root(repo_root);
        let build_dir_norm = format!("{repo_root_norm}/build");
        // None of the canary sources are in the (empty) compile database.
        let all_sources: Vec<String> = Vec::new();
        let obj_to_source = HashMap::new();
        let ninja = FakeNinja {
            whole: None,
            batched: None,
        };

        let error = scope_for_base(
            repo_root,
            &repo_root_norm,
            &build_dir_norm,
            "HEAD",
            &all_sources,
            &obj_to_source,
            &ninja,
            &mut TidyTiming::default(),
        )
        .unwrap_err();
        assert!(error.to_string().contains("is not in the compile database"));
    }

    #[test]
    fn build_cache_keys_skips_a_source_without_a_recorded_dependency_list() {
        let dep_graph = HashMap::new();
        let commands = HashMap::new();
        let keys = build_cache_keys(&["src.cpp".to_string()], &dep_graph, &commands, "salt");
        assert!(keys.is_empty());
    }

    #[test]
    fn build_cache_keys_produces_a_hit_and_miss_on_content_change() {
        let dir = tempfile::tempdir().unwrap();
        let src = dir.path().join("a.cpp");
        std::fs::write(&src, "int a();\n").unwrap();
        let src_str = src.to_string_lossy().replace('\\', "/");

        let mut dep_graph = HashMap::new();
        dep_graph.insert(src_str.clone(), vec![src_str.clone()]);
        let mut commands = HashMap::new();
        commands.insert(src_str.clone(), "cmd".to_string());

        let keys_before = build_cache_keys(
            std::slice::from_ref(&src_str),
            &dep_graph,
            &commands,
            "salt",
        );
        let key_before = keys_before.get(&src_str).unwrap().clone();

        // Unchanged content, unchanged salt: the same key.
        let keys_again = build_cache_keys(
            std::slice::from_ref(&src_str),
            &dep_graph,
            &commands,
            "salt",
        );
        assert_eq!(key_before, *keys_again.get(&src_str).unwrap());

        // A content change moves the key.
        std::fs::write(&src, "int a(); // changed\n").unwrap();
        let keys_after = build_cache_keys(
            std::slice::from_ref(&src_str),
            &dep_graph,
            &commands,
            "salt",
        );
        assert_ne!(key_before, *keys_after.get(&src_str).unwrap());

        // A salt change (simulating a BLOCKING_CHECKS or CACHE_SCHEMA change)
        // also moves the key even though the file did not change again.
        let keys_other_salt = build_cache_keys(
            std::slice::from_ref(&src_str),
            &dep_graph,
            &commands,
            "other-salt",
        );
        assert_ne!(
            *keys_after.get(&src_str).unwrap(),
            *keys_other_salt.get(&src_str).unwrap()
        );
    }

    #[test]
    fn cache_entry_round_trips_through_a_hit() {
        let dir = tempfile::tempdir().unwrap();
        store_cache_entry(
            dir.path(),
            "abcdef0123456789",
            "source.cpp",
            "cached output\n",
            0,
        );
        let path = cache_entry_path(dir.path(), "abcdef0123456789");
        assert!(path.is_file());
        assert_eq!(
            read_cache_entry(&path, "abcdef0123456789", "source.cpp").unwrap(),
            "cached output\n"
        );
    }

    #[test]
    fn run_blocking_reports_a_missing_compile_database() {
        let dir = tempfile::tempdir().unwrap();
        let repo_root = dir.path();
        let build_dir = dir.path().join("build/windows-x64-ninja-debug");
        let cache_dir = dir.path().join("cache");
        let error = run_blocking(
            repo_root, &build_dir, None, &cache_dir, 1, None, false, None,
        )
        .unwrap_err();
        assert!(error.to_string().contains("compile_commands.json"));
    }

    #[test]
    fn run_blocking_list_checks_does_not_touch_the_build_tree() {
        let dir = tempfile::tempdir().unwrap();
        let repo_root = dir.path();
        let build_dir = dir.path().join("build/does-not-exist");
        let cache_dir = dir.path().join("cache");
        let report =
            run_blocking(repo_root, &build_dir, None, &cache_dir, 1, None, true, None).unwrap();
        assert_eq!(report.scope, "list-checks");
        assert!(report.ok());
    }

    /// Integration coverage for the Ninja dependency parsing against a real
    /// `ninja -t deps` invocation: the logic above is unit-tested against
    /// canned graphs (`scope_for_base_expands_a_changed_header_to_its_project_source_consumer`
    /// and friends), this proves the parser agrees with the real tool's
    /// output format on a tiny, real CMake + Ninja project.
    ///
    /// `#[ignore]`, matching `canaries::tests::every_blocking_check_fires_on_its_own_real_canary`:
    /// a plain `cargo test` reporting "ok" regardless of whether cmake/ninja/a
    /// working C++ toolchain were actually available would make a silent skip
    /// indistinguishable from a real pass in the one place that distinction is
    /// visible without extra flags, the `passed`/`ignored` counts in the test
    /// summary. Run with `cargo test -- --ignored` on a machine that has them
    /// (this one does); `EXO_DEV_REQUIRE_NINJA` turns a missing tool into a
    /// panic instead of a graceful skip for that explicit run. No CI job
    /// currently sets `EXO_DEV_REQUIRE_NINJA` or passes `--ignored`.
    #[test]
    #[ignore = "requires cmake, ninja and a working C++ toolchain"]
    fn a_changed_header_reaches_its_real_ninja_recorded_consumer() {
        let Some(fixture) = build_ninja_fixture() else {
            skip("a_changed_header_reaches_its_real_ninja_recorded_consumer");
            return;
        };

        let obj_to_source = fixture.obj_to_source.clone();
        let ninja = RealNinja {
            build_dir: fixture.build_dir.clone(),
            extraction_seconds: std::cell::Cell::new(0.0),
            parse_metrics: std::cell::Cell::new((0.0, 0, 0, 0)),
            freshness_seconds: std::cell::Cell::new(0.0),
        };
        let lines = ninja
            .whole_graph()
            .expect("ninja -t deps produced no output");
        let graph = parse_ninja_deps(&lines, &obj_to_source, &normalize_root(&fixture.build_dir));

        let consumer_deps = graph.get(&fixture.consumer_source).unwrap_or_else(|| {
            panic!(
                "{} missing from the dependency graph",
                fixture.consumer_source
            )
        });
        assert!(
            consumer_deps
                .iter()
                .any(|dep| dep.eq_ignore_ascii_case(&fixture.shared_header)),
            "consumer.cpp's real recorded deps {consumer_deps:?} do not include the header it includes"
        );
    }

    #[test]
    #[ignore = "requires cmake, ninja and a working C++ toolchain"]
    fn configure_depends_freshness_checks_real_objects_without_build_mutations() {
        let fixture = build_ninja_fixture().expect("working Ninja fixture");
        let ninja = RealNinja {
            build_dir: fixture.build_dir.clone(),
            extraction_seconds: std::cell::Cell::new(0.0),
            parse_metrics: std::cell::Cell::new((0.0, 0, 0, 0)),
            freshness_seconds: std::cell::Cell::new(0.0),
        };
        let objects: Vec<String> = fixture.obj_to_source.keys().cloned().collect();
        let preserved: Vec<_> = [
            "build.ninja",
            ".ninja_log",
            ".ninja_deps",
            "CMakeFiles/cmake.verify_globs",
        ]
        .into_iter()
        .map(|name| {
            let path = fixture.build_dir.join(name);
            let bytes = std::fs::read(&path).unwrap();
            let modified = std::fs::metadata(&path).unwrap().modified().unwrap();
            (path, bytes, modified)
        })
        .collect();
        assert!(
            ninja.objects_current(&objects),
            "unchanged complete CONFIGURE_DEPENDS tree must be reusable"
        );
        let root = fixture.build_dir.parent().unwrap();
        for name in ["consumer.cpp", "shared.h", "leaf.h", "CMakeLists.txt"] {
            let path = root.join(name);
            let bytes = std::fs::read(&path).unwrap();
            let modified = std::fs::metadata(&path).unwrap().modified().unwrap();
            let mut changed = bytes.clone();
            changed.extend_from_slice(b"\n");
            std::fs::write(&path, changed).unwrap();
            assert!(!ninja.objects_current(&objects), "{name}");
            std::fs::write(&path, bytes).unwrap();
            std::fs::File::options()
                .write(true)
                .open(path)
                .unwrap()
                .set_times(std::fs::FileTimes::new().set_modified(modified))
                .unwrap();
            assert!(ninja.objects_current(&objects), "restored {name}");
        }
        let manifest = fixture.build_dir.join("build.ninja");
        let bytes = std::fs::read(&manifest).unwrap();
        let modified = std::fs::metadata(&manifest).unwrap().modified().unwrap();
        let changed = String::from_utf8(bytes.clone()).unwrap().replacen(
            "FLAGS =",
            "FLAGS = -DFRESHNESS_COMMAND=1 ",
            1,
        );
        assert_ne!(changed.as_bytes(), bytes);
        std::fs::write(&manifest, changed).unwrap();
        assert!(!ninja.objects_current(&objects), "compiler command changed");
        std::fs::write(&manifest, bytes).unwrap();
        std::fs::File::options()
            .write(true)
            .open(manifest)
            .unwrap()
            .set_times(std::fs::FileTimes::new().set_modified(modified))
            .unwrap();
        let added = root.join("glob_added.cpp");
        std::fs::write(&added, "int added() { return 0; }\n").unwrap();
        assert!(!ninja.objects_current(&objects), "glob membership changed");
        std::fs::remove_file(added).unwrap();
        assert!(ninja.objects_current(&objects));
        for (path, bytes, modified) in preserved {
            assert_eq!(std::fs::read(&path).unwrap(), bytes, "{}", path.display());
            assert_eq!(
                std::fs::metadata(&path).unwrap().modified().unwrap(),
                modified,
                "{}",
                path.display()
            );
        }
    }

    #[test]
    #[ignore = "requires cmake, ninja and a working C++ toolchain"]
    fn an_always_generated_header_only_disables_its_own_consumers() {
        let fixture = build_ninja_fixture_with_metadata(true).expect("working Ninja fixture");
        let ninja = RealNinja {
            build_dir: fixture.build_dir.clone(),
            extraction_seconds: std::cell::Cell::new(0.0),
            parse_metrics: std::cell::Cell::new((0.0, 0, 0, 0)),
            freshness_seconds: std::cell::Cell::new(0.0),
        };
        let entries = parse_compile_db(
            &std::fs::read_to_string(fixture.build_dir.join("compile_commands.json")).unwrap(),
        )
        .unwrap();
        let build = normalize_root(&fixture.build_dir);
        let (objects, sources) = object_maps(&entries, &build);
        let selected: Vec<String> = sources.keys().cloned().collect();
        let header = fixture.build_dir.parent().unwrap().join("metadata.h");
        let modified = std::fs::metadata(&header).unwrap().modified().unwrap();
        let recorded = parse_ninja_deps(
            &ninja
                .for_objects(&objects.keys().cloned().collect::<Vec<_>>())
                .unwrap(),
            &objects,
            &build,
        );
        assert!(
            recorded[&fixture.consumer_source]
                .iter()
                .any(|dependency| dependency.eq_ignore_ascii_case(&normalize_root(&header))),
            "generated header must be an actual recorded include"
        );
        let graph = deps_for_sources(&ninja, &selected, &sources, &objects, &build, None)
            .expect("unaffected objects remain reusable");
        assert!(!graph.contains_key(&fixture.consumer_source));
        let other = normalize_root(&fixture.build_dir.parent().unwrap().join("other.cpp"));
        assert!(graph.contains_key(&other));
        let keys = build_cache_keys(&selected, &graph, &command_identities(&entries), "salt");
        assert_eq!(keys.len(), 1);
        assert!(keys.contains_key(&other));
        assert_eq!(
            std::fs::metadata(header).unwrap().modified().unwrap(),
            modified
        );
    }

    struct NinjaFixture {
        /// Kept alive for the fixture's lifetime so the directory is removed
        /// (RAII) when the test is done with it, rather than leaked into
        /// `%TEMP%` on every run.
        _dir: tempfile::TempDir,
        build_dir: PathBuf,
        obj_to_source: HashMap<String, String>,
        consumer_source: String,
        shared_header: String,
    }

    /// An unmissable marker that a test's assertions did not run: printed
    /// through the child-inherited stdio path (see `build_ninja_fixture`),
    /// which cargo test does not buffer, so it is visible next to whatever
    /// diagnostic the missing/failing tool itself printed, with or without
    /// `--nocapture`. `cargo test`'s own summary line still reports the test
    /// as passed (no assertion ran to fail); this is the human-visible
    /// counter-signal that a plain "ok" does not carry on its own.
    fn skip(test_name: &str) {
        eprintln!(
            "SKIP {test_name}: cmake/ninja/a C++ toolchain are not usable here; \
             this run asserts nothing about real Ninja dependency parsing. \
             Set EXO_DEV_REQUIRE_NINJA=1 on a machine that has them to turn this into a failure."
        );
    }

    /// Configures and builds a two-translation-unit CMake project with Ninja,
    /// one of which includes a shared header. Returns `None` (skip) when
    /// `cmake`, `ninja` or a C++ compiler are not usable here, unless
    /// `EXO_DEV_REQUIRE_NINJA` says that is not an acceptable outcome.
    fn build_ninja_fixture() -> Option<NinjaFixture> {
        build_ninja_fixture_with_metadata(false)
    }

    fn build_ninja_fixture_with_metadata(always_metadata: bool) -> Option<NinjaFixture> {
        if !tool_on_path("cmake") || !tool_on_path("ninja") {
            if std::env::var_os("EXO_DEV_REQUIRE_NINJA").is_some() {
                panic!("EXO_DEV_REQUIRE_NINJA is set but cmake and/or ninja are not on PATH");
            }
            return None;
        }

        let dir = match tempfile::tempdir() {
            Ok(dir) => dir,
            Err(_) => return None,
        };
        let root = dir.path().to_path_buf();
        let header = root.join("shared.h");
        let consumer = root.join("consumer.cpp");
        let other = root.join("other.cpp");
        let cmake_lists = root.join("CMakeLists.txt");
        std::fs::write(
            root.join("leaf.h"),
            "inline int leaf_value() { return 1; }\n",
        )
        .unwrap();
        std::fs::write(
            &header,
            "#include \"leaf.h\"\ninline int shared_value() { return leaf_value(); }\n",
        )
        .unwrap();
        std::fs::write(
            &consumer,
            "#include \"shared.h\"\nint consumer() { return shared_value(); }\n",
        )
        .unwrap();
        std::fs::write(&other, "int other() { return 2; }\n").unwrap();
        std::fs::write(
            &cmake_lists,
            "cmake_minimum_required(VERSION 3.20)\n\
             project(exo_dev_ninja_fixture CXX)\n\
             set(CMAKE_EXPORT_COMPILE_COMMANDS ON)\n\
             file(GLOB fixture_sources CONFIGURE_DEPENDS \"${CMAKE_CURRENT_SOURCE_DIR}/*.cpp\")\n\
             add_library(fixture OBJECT ${fixture_sources})\n",
        )
        .unwrap();
        if always_metadata {
            std::fs::write(
                root.join("metadata.h"),
                "inline int metadata_value() { return 0; }\n",
            )
            .unwrap();
            std::fs::write(&consumer, "#include \"shared.h\"\n#include \"metadata.h\"\nint consumer() { return shared_value() + metadata_value(); }\n").unwrap();
            let mut contents = std::fs::read_to_string(&cmake_lists).unwrap();
            contents.push_str("add_custom_target(always_metadata\n COMMAND \"${CMAKE_COMMAND}\" -E touch \"${CMAKE_CURRENT_SOURCE_DIR}/metadata.h\"\n BYPRODUCTS \"${CMAKE_CURRENT_SOURCE_DIR}/metadata.h\"\n VERBATIM)\nadd_dependencies(fixture always_metadata)\nset_source_files_properties(consumer.cpp PROPERTIES OBJECT_DEPENDS \"${CMAKE_CURRENT_SOURCE_DIR}/metadata.h\")\n");
            std::fs::write(&cmake_lists, contents).unwrap();
        }

        let build_dir = root.join("build");
        std::fs::create_dir_all(&build_dir).ok()?;

        let mut configure = std::process::Command::new("cmake");
        configure
            .args(["-G", "Ninja", "-S"])
            .arg(&root)
            .arg("-B")
            .arg(&build_dir);
        let compiler_environment = crate::msvc::environment()
            .ok()
            .flatten()
            .map(|(_, values)| values)
            .unwrap_or_default();
        configure.envs(
            compiler_environment
                .iter()
                .map(|(name, value)| (name, value)),
        );
        let configured = configure.status().ok()?;
        require_or_none(configured.success(), "cmake configure failed")?;

        let mut build = std::process::Command::new("cmake");
        build.args(["--build"]).arg(&build_dir);
        build.envs(
            compiler_environment
                .iter()
                .map(|(name, value)| (name, value)),
        );
        let built = build.status().ok()?;
        require_or_none(built.success(), "ninja build failed")?;

        let compile_db = std::fs::read_to_string(build_dir.join("compile_commands.json")).ok()?;
        let entries = parse_compile_db(&compile_db).ok()?;
        let build_dir_norm = normalize_root(&build_dir);
        let (obj_to_source, _) = object_maps(&entries, &build_dir_norm);

        let consumer_source = normalize_root(&consumer);
        let shared_header = normalize_root(&header);
        Some(NinjaFixture {
            _dir: dir,
            build_dir,
            obj_to_source,
            consumer_source,
            shared_header,
        })
    }

    fn require_or_none(ok: bool, reason: &str) -> Option<()> {
        if ok {
            return Some(());
        }
        if std::env::var_os("EXO_DEV_REQUIRE_NINJA").is_some() {
            panic!("EXO_DEV_REQUIRE_NINJA is set but {reason}");
        }
        None
    }

    fn tool_on_path(name: &str) -> bool {
        let mut command = crate::process::command(name);
        command.arg("--version");
        matches!(crate::process::query(command), Ok((code, _)) if code == 0)
    }
}
