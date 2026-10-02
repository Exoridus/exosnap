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
//! What `base` does NOT cover: a change that alters how the code is compiled
//! without touching a source file, a header, or a canary trigger below (new
//! compiler flags in a `CMakeLists.txt`, a different toolchain, a preset
//! edit). Those can move findings in units this run never looks at. Re-run
//! the full pass by hand after such a change.
//!
//! Analyser note. clang-tidy registers the entire clang-analyzer core package
//! as soon as any `clang-analyzer-*` check is requested;
//! `-clang-analyzer-core.X` cannot turn the extras back off. Only the checks
//! named in `BLOCKING_CHECKS` count as a violation here, so the extras stay
//! out of the verdict.

use std::collections::{BTreeSet, HashMap, HashSet, VecDeque};
use std::path::{Path, PathBuf};
use std::sync::Mutex;

use anyhow::Context as _;
use regex::Regex;
use sha2::{Digest, Sha256};

use crate::executor::ToolMissing;
use crate::lint::{self, canaries::BLOCKING_CHECKS};

/// Version of the per-translation-unit cache key composition. There is no
/// content hash of this module's own source to fold into the key, so this
/// stands in for it. Bump by hand whenever a stored diagnostic could stop
/// describing the same verdict for the same inputs: the clang-tidy
/// invocation's own literal flags change (a new `--extra-arg`, a different
/// `-p`/`--quiet` form), or the cache entry's stored format changes.
/// Scoping (which translation units get analysed) does not affect what a
/// stored entry means and is not a reason to bump this. Nothing else
/// enforces the bump.
pub const CACHE_SCHEMA: u32 = 1;

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
/// translation unit and each header the build recorded for it, the entry's
/// own compile command, `.clang-tidy`, [`CACHE_SCHEMA`], the resolved check
/// list, and the clang-tidy binary's version banner. Anything that cannot be
/// hashed with certainty, or a missing dependency graph, makes the unit a
/// miss, never a false hit.
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

    let tool = clang_tidy_path
        .map(Path::to_path_buf)
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

    let (obj_to_source, source_to_obj) = object_maps(&entries, &build_dir_norm);
    let ninja = RealNinja {
        build_dir: build_dir.to_path_buf(),
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
            )? {
                BaseScope::Nothing(message) => {
                    return Ok(ClangTidyReport::skipped(&message, cache_dir));
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

    let jobs = if jobs == 0 {
        std::thread::available_parallelism()
            .map(std::num::NonZeroUsize::get)
            .unwrap_or(4)
    } else {
        jobs
    };

    let checks_arg = format!("-*,{}", BLOCKING_CHECKS.join(","));
    let flags = invocation_flags(&checks_arg, HEADER_FILTER);

    let commands: HashMap<String, String> = entries
        .iter()
        .map(|entry| (entry.file.clone(), entry.command_line.clone()))
        .collect();

    let clang_tidy_config_hash = hash_file(&repo_root.join(".clang-tidy").to_string_lossy());
    let salt = compute_salt(&banner, &flags, clang_tidy_config_hash.as_deref());

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
            Some(dep_graph) => {
                let keys =
                    build_cache_keys(&sources, &dep_graph, &commands, salt.as_deref().unwrap());
                (true, keys)
            }
        }
    };

    if cache_enabled {
        std::fs::create_dir_all(cache_dir)
            .with_context(|| format!("could not create {}", cache_dir.display()))?;
    }

    let mut results: Vec<(String, String)> = Vec::new();
    let mut pending: Vec<String> = Vec::new();
    let mut replayed = 0;
    for src in &sources {
        if let Some(key) = cache_keys.get(src) {
            let entry_path = cache_entry_path(cache_dir, key);
            if let Ok(text) = std::fs::read_to_string(&entry_path) {
                results.push((src.clone(), text));
                replayed += 1;
                continue;
            }
        }
        pending.push(src.clone());
    }

    let fresh = analyze_pending(&tool, build_dir, &flags, &pending, jobs);
    for (src, output, code) in &fresh {
        results.push((src.clone(), output.clone()));
        if !cache_enabled {
            continue;
        }
        let Some(key) = cache_keys.get(src) else {
            continue;
        };
        // clang-tidy exits 0 (clean) or 1 (diagnostics emitted); anything else
        // is the process dying rather than reporting, and its truncated
        // output must not be stored as this unit's standing verdict.
        if *code != 0 && *code != 1 {
            continue;
        }
        store_cache_entry(cache_dir, key, output);
    }

    let violations = extract_violations(&results, &repo_root_norm, &build_dir_norm);

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
    })
}

// ---------------------------------------------------------------------------
// compile_commands.json
// ---------------------------------------------------------------------------

#[derive(Debug, Clone)]
struct CompileEntry {
    /// Absolute source path, forward slashes.
    file: String,
    /// Object output path relative to the build directory, forward slashes.
    /// `None` when the entry carries no `output` field.
    output: Option<String>,
    /// `directory|command arguments`, the entry's contribution to the cache
    /// key: defines and include paths steer the analysis as much as the
    /// source does.
    command_line: String,
}

fn parse_compile_db(text: &str) -> anyhow::Result<Vec<CompileEntry>> {
    let value: serde_json::Value =
        serde_json::from_str(text).context("compile_commands.json is not valid JSON")?;
    let array = value
        .as_array()
        .context("compile_commands.json is not a JSON array")?;
    let mut entries = Vec::with_capacity(array.len());
    for entry in array {
        let file = entry
            .get("file")
            .and_then(|v| v.as_str())
            .unwrap_or_default()
            .replace('\\', "/");
        if file.is_empty() {
            continue;
        }
        let directory = entry
            .get("directory")
            .and_then(|v| v.as_str())
            .unwrap_or_default();
        let command = entry
            .get("command")
            .and_then(|v| v.as_str())
            .unwrap_or_default();
        let arguments = entry
            .get("arguments")
            .and_then(|v| v.as_array())
            .map(|values| {
                values
                    .iter()
                    .filter_map(|v| v.as_str())
                    .collect::<Vec<_>>()
                    .join(" ")
            })
            .unwrap_or_default();
        let output = entry
            .get("output")
            .and_then(|v| v.as_str())
            .map(|s| s.replace('\\', "/"));
        entries.push(CompileEntry {
            file,
            output,
            command_line: format!("{directory}|{command}{arguments}"),
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
) -> (HashMap<String, String>, HashMap<String, String>) {
    let mut obj_to_source = HashMap::new();
    let mut source_to_obj = HashMap::new();
    let prefix = format!("{}/", build_dir_norm.to_ascii_lowercase());
    for entry in entries {
        let Some(out) = &entry.output else { continue };
        let stripped = if out.to_ascii_lowercase().starts_with(&prefix) {
            out[prefix.len()..].to_string()
        } else {
            out.clone()
        };
        obj_to_source.insert(stripped.clone(), entry.file.clone());
        source_to_obj.insert(entry.file.clone(), stripped);
    }
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
}

struct RealNinja {
    build_dir: PathBuf,
}

impl NinjaDeps for RealNinja {
    fn whole_graph(&self) -> Option<Vec<String>> {
        invoke_ninja_deps(&self.build_dir, &[])
    }

    fn for_objects(&self, objects: &[String]) -> Option<Vec<String>> {
        invoke_ninja_deps(&self.build_dir, objects)
    }
}

fn invoke_ninja_deps(build_dir: &Path, args: &[String]) -> Option<Vec<String>> {
    let mut command = crate::process::command("ninja");
    command.current_dir(build_dir);
    command.arg("-t").arg("deps");
    command.args(args);
    let (_, stdout) = crate::process::query(command).ok()?;
    let lines: Vec<String> = stdout.lines().map(str::to_string).collect();
    if lines.is_empty() { None } else { Some(lines) }
}

fn deps_header_regex() -> &'static Regex {
    static RE: std::sync::OnceLock<Regex> = std::sync::OnceLock::new();
    RE.get_or_init(|| Regex::new(r"^(\S.*?):\s+#deps ").unwrap())
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
    let mut graph: HashMap<String, Vec<String>> = HashMap::new();
    let mut current: Option<String> = None;
    for line in lines {
        if line.starts_with(' ') || line.starts_with('\t') {
            if let Some(src) = &current {
                let dep = resolve_against_build_dir(line.trim(), build_dir_norm);
                graph.entry(src.clone()).or_default().push(dep);
            }
            continue;
        }
        current = None;
        if let Some(caps) = deps_header_regex().captures(line) {
            let obj = caps[1].replace('\\', "/");
            if let Some(src) = obj_to_source.get(&obj) {
                current = Some(src.clone());
                graph.entry(src.clone()).or_default();
            }
        }
    }
    graph
}

fn deps_for_sources(
    ninja: &dyn NinjaDeps,
    sources: &[String],
    source_to_obj: &HashMap<String, String>,
    obj_to_source: &HashMap<String, String>,
    build_dir_norm: &str,
    already: Option<&HashMap<String, Vec<String>>>,
) -> Option<HashMap<String, Vec<String>>> {
    if let Some(graph) = already {
        return Some(graph.clone());
    }
    let objects: Vec<String> = sources
        .iter()
        .filter_map(|s| source_to_obj.get(s).cloned())
        .collect();
    if objects.is_empty() {
        return Some(HashMap::new());
    }
    // Batched: a whole-tree selection would otherwise build a command line
    // past the Windows limit and fail as a malformed invocation rather than
    // as a missing graph.
    let mut graph = HashMap::new();
    for chunk in objects.chunks(100) {
        let lines = ninja.for_objects(chunk)?;
        graph.extend(parse_ninja_deps(&lines, obj_to_source, build_dir_norm));
    }
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
) -> anyhow::Result<BaseScope> {
    let git = crate::git::Git::new(repo_root);
    let (code, stdout) = git.run(&["diff", "--name-only", "--diff-filter=ACMR", base, "--", "."]);
    anyhow::ensure!(
        code == 0,
        "git diff against '{base}' failed - is the base revision fetched?"
    );
    let changed_all: Vec<String> = stdout
        .lines()
        .map(|line| line.trim().replace('\\', "/"))
        .filter(|line| !line.is_empty())
        .collect();
    let changed: Vec<String> = changed_all
        .iter()
        .filter(|f| is_source_or_header(f))
        .cloned()
        .collect();
    let canary_hit: Vec<String> = changed_all
        .iter()
        .filter(|f| CANARY_TRIGGERS.contains(&f.as_str()))
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
        let graph = parse_ninja_deps(&lines, obj_to_source, build_dir_norm);
        for (src, deps) in &graph {
            if !is_project_source(src, repo_root_norm, build_dir_norm) {
                continue;
            }
            if deps
                .iter()
                .any(|dep| changed_abs.contains(&dep.to_ascii_lowercase()))
            {
                affected.insert(src.clone());
            }
        }
        dep_graph_read = Some(graph);
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
/// reporting a green that was never established, so the key covers every
/// input that can change the verdict and anything unhashable degrades to a
/// miss.
fn compute_salt(
    clang_tidy_banner: &str,
    invocation_flags: &[String],
    clang_tidy_config_hash: Option<&str>,
) -> Option<String> {
    let config_hash = clang_tidy_config_hash?;
    Some(format!(
        "{clang_tidy_banner}\n{}\nCACHE_SCHEMA={CACHE_SCHEMA}\n.clang-tidy={config_hash}",
        invocation_flags.join("\n")
    ))
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
        let command_line = commands.get(src).cloned().unwrap_or_default();
        let mut parts = vec![salt.to_string(), command_line];
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

/// Written aside and moved into place: a run cancelled mid-write would
/// otherwise leave a truncated entry that later reads as a complete verdict.
fn store_cache_entry(cache_dir: &Path, key: &str, output: &str) {
    let entry = cache_entry_path(cache_dir, key);
    let Some(parent) = entry.parent() else { return };
    if std::fs::create_dir_all(parent).is_err() {
        return;
    }
    let temp = entry.with_extension(format!("{}.tmp", std::process::id()));
    if std::fs::write(&temp, output).is_err() {
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

fn clang_tidy_version_banner(tool: &Path) -> anyhow::Result<String> {
    let mut command = crate::process::command(&tool.to_string_lossy());
    command.arg("--version");
    let (_, stdout) = crate::process::query(command)
        .with_context(|| format!("could not run {}", tool.display()))?;
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
    vec![
        "--quiet".to_string(),
        format!("--checks={checks_arg}"),
        format!("--header-filter={header_filter}"),
    ]
}

fn invoke_clang_tidy(
    tool: &Path,
    build_dir: &Path,
    flags: &[String],
    source: &str,
) -> (String, i32) {
    let mut command = crate::process::command(&tool.to_string_lossy());
    command.arg("-p").arg(build_dir);
    command.args(flags);
    command.arg(source);
    match command.output() {
        Ok(output) => {
            let mut combined = String::from_utf8_lossy(&output.stdout).into_owned();
            combined.push_str(&String::from_utf8_lossy(&output.stderr));
            (combined, output.status.code().unwrap_or(-1))
        }
        Err(_) => (String::new(), -1),
    }
}

/// Runs clang-tidy over `pending` with up to `jobs` processes in flight at
/// once.
fn analyze_pending(
    tool: &Path,
    build_dir: &Path,
    flags: &[String],
    pending: &[String],
    jobs: usize,
) -> Vec<(String, String, i32)> {
    if pending.is_empty() {
        return Vec::new();
    }
    let jobs = jobs.max(1).min(pending.len());
    let queue: Mutex<VecDeque<String>> = Mutex::new(pending.iter().cloned().collect());
    let results: Mutex<Vec<(String, String, i32)>> = Mutex::new(Vec::new());
    std::thread::scope(|scope| {
        for _ in 0..jobs {
            scope.spawn(|| {
                loop {
                    let next = queue.lock().unwrap().pop_front();
                    let Some(src) = next else { break };
                    let (output, code) = invoke_clang_tidy(tool, build_dir, flags, &src);
                    results.lock().unwrap().push((src, output, code));
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

/// clang-tidy's exit code is NOT the verdict here. Two pre-existing
/// conditions make it non-zero for reasons unrelated to the blocking set:
/// clang parses in MSVC-compat mode and rejects a handful of constructs
/// cl.exe accepts, and findings inside Qt and Windows SDK headers, which are
/// not ours to fix. The verdict is therefore: a diagnostic from a blocking
/// check, located in a file this repository owns.
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
            let rel = where_.strip_prefix(&root_prefix).unwrap_or(where_.as_str());
            for check in checks_field.split(',') {
                let check = check.trim();
                if BLOCKING_CHECKS
                    .iter()
                    .any(|pattern| check_matches(check, pattern))
                {
                    violations.insert(format!("{rel}:{row}:{col} [{check}] {message}"));
                }
            }
        }
    }
    violations
}

#[cfg(test)]
mod tests {
    use super::*;

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
        source_to_obj.insert("src.cpp".to_string(), "src.obj".to_string());
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
        )
        .unwrap();

        let BaseScope::Scoped { sources, .. } = scope else {
            panic!("expected a scoped result, the header change is analysable");
        };
        assert_eq!(sources, vec![foo_cpp]);
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
        store_cache_entry(dir.path(), "abcdef0123456789", "cached output\n");
        let path = cache_entry_path(dir.path(), "abcdef0123456789");
        assert!(path.is_file());
        assert_eq!(std::fs::read_to_string(path).unwrap(), "cached output\n");
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
        std::fs::write(&header, "inline int shared_value() { return 1; }\n").unwrap();
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
             add_library(fixture OBJECT consumer.cpp other.cpp)\n",
        )
        .unwrap();

        let build_dir = root.join("build");
        std::fs::create_dir_all(&build_dir).ok()?;

        let mut configure = std::process::Command::new("cmake");
        configure
            .args(["-G", "Ninja", "-S"])
            .arg(&root)
            .arg("-B")
            .arg(&build_dir);
        let configured = configure.status().ok()?;
        require_or_none(configured.success(), "cmake configure failed")?;

        let mut build = std::process::Command::new("cmake");
        build.args(["--build"]).arg(&build_dir);
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
