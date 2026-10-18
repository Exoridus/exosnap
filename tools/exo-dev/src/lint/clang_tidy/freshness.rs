//! Read-only freshness checks for CMake's configured Ninja graph.

use std::collections::{HashMap, HashSet};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

#[cfg(test)]
#[path = "freshness_measurement.rs"]
mod measurement;

pub(super) fn objects_current(build_dir: &Path, objects: &[String]) -> bool {
    let current = current_objects(build_dir, objects);
    objects.iter().all(|object| current.contains(object))
}

pub(super) fn current_objects(build_dir: &Path, objects: &[String]) -> HashSet<String> {
    prove_current(build_dir, objects, objects.len() >= 1000).unwrap_or_default()
}

#[cfg(test)]
pub(super) fn current_objects_batched(build_dir: &Path, objects: &[String]) -> HashSet<String> {
    prove_current(build_dir, objects, false).unwrap_or_default()
}

#[cfg(test)]
pub(super) fn current_objects_with_default_plan(
    build_dir: &Path,
    objects: &[String],
) -> HashSet<String> {
    prove_current(build_dir, objects, true).unwrap_or_default()
}

fn prove_current(
    build_dir: &Path,
    objects: &[String],
    use_default_plan: bool,
) -> Option<HashSet<String>> {
    let manifest_path = build_dir.join("build.ninja");
    let manifest = std::fs::read(&manifest_path).ok()?;
    let modified = std::fs::metadata(&manifest_path).ok()?.modified().ok()?;
    let mut query = crate::process::command("ninja");
    query
        .current_dir(build_dir)
        .args(["-t", "query", "build.ninja"]);
    let (code, output) = crate::process::query(query).ok()?;
    if code != 0 {
        return None;
    }
    let inputs = manifest_inputs(&output)?;
    for input in &inputs {
        let path = resolve(build_dir, input);
        let metadata = std::fs::metadata(path).ok()?;
        if !metadata.is_file() || metadata.modified().ok()? > modified {
            return None;
        }
    }
    for input in inputs.iter().filter(|input| {
        Path::new(input)
            .file_name()
            .is_some_and(|name| name == "VerifyGlobs.cmake")
    }) {
        let script = std::fs::read_to_string(resolve(build_dir, input)).ok()?;
        let read_only = read_only_glob_script(&script)?;
        let scratch = TemporaryFile::new(read_only.as_bytes(), "cmake").ok()?;
        let cache = std::fs::read_to_string(build_dir.join("CMakeCache.txt")).ok()?;
        let cmake = cache
            .lines()
            .find_map(|line| line.strip_prefix("CMAKE_COMMAND:INTERNAL="))?;
        let mut verify = crate::process::command(cmake);
        verify.current_dir(build_dir).arg("-P").arg(&scratch.path);
        if crate::process::query(verify).ok()?.0 != 0 {
            return None;
        }
    }
    // CMake's unconditional glob preflight makes a normal dry run assume
    // regeneration, even after a complete build. A byte-identical manifest
    // under another name omits only that automatic self-regeneration step.
    // Its configuration inputs and glob memberships were checked above.
    let scratch = TemporaryFile::new(&manifest, "ninja").ok()?;
    // Expanding response files would change the strings printed by -v.
    // Ninja's dirty scan still hashes their configured contents internally.
    let mut query = crate::process::command("ninja");
    query
        .current_dir(build_dir)
        .arg("-f")
        .arg(&scratch.path)
        .args(["-t", "compdb"]);
    let (code, output) = crate::process::query(query).ok()?;
    if code != 0 {
        return None;
    }
    let commands = object_commands(build_dir, &output)?;
    let mut current = HashSet::new();
    let mut remaining = objects.to_vec();
    if use_default_plan {
        if let Some((proven, covered)) =
            current_from_plan(build_dir, &scratch.path, &[], objects, &commands)
        {
            current.extend(proven);
            // EXCLUDE_FROM_ALL commands are absent from the default plan.
            // They still need the same targeted dirty scan as smaller scopes.
            remaining.retain(|object| !covered.contains(object));
        }
    }
    for chunk in remaining.chunks(100) {
        if let Some((proven, _)) =
            current_from_plan(build_dir, &scratch.path, chunk, chunk, &commands)
        {
            current.extend(proven);
        }
    }
    if std::fs::read(&manifest_path).ok()? != manifest {
        return None;
    }
    Some(current)
}

fn current_from_plan(
    build_dir: &Path,
    manifest: &Path,
    targets: &[String],
    objects: &[String],
    commands: &HashMap<String, Option<String>>,
) -> Option<(HashSet<String>, HashSet<String>)> {
    // compdb omits command edges without inputs. The command chain also
    // identifies always-run producers, so unknown output fails closed.
    let mut query = crate::process::command("ninja");
    query
        .current_dir(build_dir)
        .arg("-f")
        .arg(manifest)
        .args(["-t", "commands"])
        .args(targets);
    let (code, output) = crate::process::query(query).ok()?;
    if code != 0 {
        return None;
    }
    let known: HashSet<&str> = output.lines().collect();
    let mut query = crate::process::command("ninja");
    query
        .current_dir(build_dir)
        .arg("-f")
        .arg(manifest)
        .args(["-n", "-v"])
        .env("NINJA_STATUS", "EXO_TIDY_COMMAND:%f/%t: ")
        .args(targets);
    let (code, output) = crate::process::query(query).ok()?;
    if code != 0 {
        return None;
    }
    let scheduled = scheduled_commands(&output, &known)?;
    let mut current = HashSet::new();
    let mut covered = HashSet::new();
    for object in objects {
        let Some(Some(command)) = commands.get(&object_identity(build_dir, object)) else {
            continue;
        };
        if known.contains(command.as_str()) {
            covered.insert(object.clone());
            if !scheduled.contains(command.as_str()) {
                current.insert(object.clone());
            }
        }
    }
    Some((current, covered))
}

#[derive(serde::Deserialize)]
struct EvaluatedCommand {
    output: String,
    command: String,
}

fn object_identity(build_dir: &Path, output: &str) -> String {
    super::resolve_against_build_dir(output, &super::normalize_root(build_dir))
}

fn object_commands(build_dir: &Path, output: &str) -> Option<HashMap<String, Option<String>>> {
    let entries: Vec<EvaluatedCommand> = serde_json::from_str(output).ok()?;
    let mut commands = HashMap::new();
    for entry in entries {
        if entry.output.is_empty() {
            return None;
        }
        let object = object_identity(build_dir, &entry.output);
        let valid = !entry.command.is_empty() && !entry.command.contains(['\r', '\n']);
        commands
            .entry(object)
            .and_modify(|value| *value = None)
            .or_insert(valid.then_some(entry.command));
    }
    Some(commands)
}

fn scheduled_commands<'a>(output: &'a str, known: &HashSet<&str>) -> Option<HashSet<&'a str>> {
    if output.trim() == "ninja: no work to do." {
        return Some(HashSet::new());
    }
    let mut scheduled = HashSet::new();
    let mut expected_total = None;
    let mut completed = 0;
    for (index, line) in output.lines().enumerate() {
        let (status, command) = line.strip_prefix("EXO_TIDY_COMMAND:")?.split_once(": ")?;
        let (finished, total) = status.split_once('/')?;
        let finished: usize = finished.parse().ok()?;
        let total: usize = total.parse().ok()?;
        if total == 0
            || finished != index + 1
            || expected_total.is_some_and(|expected| expected != total)
        {
            return None;
        }
        expected_total = Some(total);
        completed = finished;
        if command.is_empty() || !known.contains(command) {
            return None;
        }
        scheduled.insert(command);
    }
    (expected_total == Some(completed) && completed > 0).then_some(scheduled)
}

fn resolve(build_dir: &Path, input: &str) -> PathBuf {
    let path = Path::new(input);
    if path.is_absolute() {
        path.to_path_buf()
    } else {
        build_dir.join(path)
    }
}

fn manifest_inputs(output: &str) -> Option<Vec<String>> {
    let mut lines = output.lines();
    if lines.next()? != "build.ninja:" || lines.next()?.trim() != "input: RERUN_CMAKE" {
        return None;
    }
    let mut inputs = Vec::new();
    for line in lines {
        if line.trim() == "outputs:" {
            return (!inputs.is_empty()).then_some(inputs);
        }
        if !line.starts_with("    ") {
            return None;
        }
        let input = line.trim().trim_start_matches('|').trim();
        if input.is_empty() {
            return None;
        }
        inputs.push(input.to_string());
    }
    None
}

fn read_only_glob_script(script: &str) -> Option<String> {
    if !script.starts_with("# CMAKE generated file: DO NOT EDIT!") {
        return None;
    }
    let quoted = r#""(?:[^"\\]|\\.)*""#;
    let glob = regex::Regex::new(&format!(r"^file\(GLOB(?:_RECURSE)? NEW_GLOB(?: FOLLOW_SYMLINKS)?(?: LIST_DIRECTORIES (?:true|false))? {quoted}(?: {quoted})*\)$")).ok()?;
    let touch = regex::Regex::new(&format!(r"^file\(TOUCH_NOCREATE {quoted}\)$")).ok()?;
    let quoted = regex::Regex::new(&format!("^{quoted}$")).ok()?;
    let mut globs = 0;
    let mut touches = 0;
    for line in script.lines().map(str::trim) {
        if line.is_empty() || line.starts_with('#') || quoted.is_match(line) || line == ")" {
            continue;
        }
        if glob.is_match(line) {
            globs += 1;
            continue;
        }
        if touch.is_match(line) {
            touches += 1;
            continue;
        }
        if !matches!(
            line,
            "set(OLD_GLOB"
                | "if(NOT \"${NEW_GLOB}\" STREQUAL \"${OLD_GLOB}\")"
                | "message(\"-- GLOB mismatch!\")"
                | "set(NEW_ONLY ${NEW_GLOB})"
                | "set(OLD_ONLY ${OLD_GLOB})"
                | "list(REMOVE_ITEM NEW_ONLY ${OLD_GLOB})"
                | "list(REMOVE_ITEM OLD_ONLY ${NEW_GLOB})"
                | "if(NEW_ONLY)"
                | "if(OLD_ONLY)"
                | "message(\"The following files were added:\")"
                | "message(\"The following files were removed:\")"
                | "foreach(VAR_FILE IN LISTS NEW_ONLY)"
                | "foreach(VAR_FILE IN LISTS OLD_ONLY)"
                | "message(\"  +${VAR_FILE}\")"
                | "message(\"  -${VAR_FILE}\")"
                | "endforeach()"
                | "endif()"
                | "cmake_policy(SET CMP0009 NEW)"
        ) {
            return None;
        }
    }
    if globs == 0 || touches != globs {
        return None;
    }
    Some(script.replace("file(TOUCH_NOCREATE", "message(FATAL_ERROR"))
}

struct TemporaryFile {
    path: PathBuf,
}

impl TemporaryFile {
    fn new(bytes: &[u8], extension: &str) -> std::io::Result<Self> {
        static NEXT: AtomicU64 = AtomicU64::new(0);
        for _ in 0..16 {
            let path = std::env::temp_dir().join(format!(
                "exo-dev-tidy-{}-{}.{}",
                std::process::id(),
                NEXT.fetch_add(1, Ordering::Relaxed),
                extension
            ));
            match std::fs::OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(&path)
            {
                Ok(mut file) => {
                    let scratch = Self { path };
                    let written = file.write_all(bytes);
                    drop(file);
                    written?;
                    return Ok(scratch);
                }
                Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(error) => return Err(error),
            }
        }
        Err(std::io::Error::new(
            std::io::ErrorKind::AlreadyExists,
            "could not reserve a freshness snapshot",
        ))
    }
}

impl Drop for TemporaryFile {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.path);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn unsupported_glob_operations_are_never_executed() {
        for script in [
            "# CMAKE generated file: DO NOT EDIT!\nfile(WRITE \"output\" \"content\")\n",
            "# CMAKE generated file: DO NOT EDIT!\nexecute_process(COMMAND arbitrary)\n",
            "# CMAKE generated file: DO NOT EDIT!\ninclude(\"other.cmake\")\n",
        ] {
            assert!(read_only_glob_script(script).is_none());
        }
    }

    #[test]
    fn incomplete_regeneration_inputs_do_not_prove_freshness() {
        for output in [
            "",
            "build.ninja:\n  input: RERUN_CMAKE\n",
            "build.ninja:\n  input: UNKNOWN\n  outputs:\n",
        ] {
            assert!(manifest_inputs(output).is_none());
        }
    }

    #[test]
    fn scheduled_commands_require_complete_known_verbose_frames() {
        let known = HashSet::from(["cl.exe @a.rsp", "cmake.exe -E touch metadata.h"]);
        let output = "EXO_TIDY_COMMAND:1/2: cmake.exe -E touch metadata.h\nEXO_TIDY_COMMAND:2/2: cl.exe @a.rsp\n";
        assert_eq!(scheduled_commands(output, &known).unwrap().len(), 2);
        for output in [
            "",
            "EXO_TIDY_COMMAND:1/1: cl.exe @",
            "[1/1] cl.exe @a.rsp",
            "EXO_TIDY_COMMAND:1/1: ",
        ] {
            assert!(scheduled_commands(output, &known).is_none());
        }
        assert!(
            scheduled_commands("ninja: no work to do.\n", &known)
                .unwrap()
                .is_empty()
        );
    }

    #[test]
    fn scheduled_command_proof_rejects_a_valid_prefix_without_a_complete_plan() {
        let known = HashSet::from(["cl.exe @a.rsp", "cl.exe @b.rsp"]);
        for output in [
            "EXO_TIDY_COMMAND: cl.exe @a.rsp\n",
            "EXO_TIDY_COMMAND:1/2: cl.exe @a.rsp\n",
            "EXO_TIDY_COMMAND:1/2: cl.exe @a.rsp\nEXO_TIDY_COMMAND:2/3: cl.exe @b.rsp\n",
        ] {
            assert!(scheduled_commands(output, &known).is_none(), "{output}");
        }
    }

    #[test]
    fn ambiguous_object_commands_remain_uncovered_and_rsp_tokens_are_preserved() {
        let commands = object_commands(
            Path::new("C:/repo/build"),
            r#"[
            {"output":"a.obj","command":"cl.exe @a.rsp"},
            {"output":"b.obj","command":"cl.exe @b.rsp"},
            {"output":"b.obj","command":"cl.exe @other.rsp"}
        ]"#,
        )
        .unwrap();
        assert_eq!(
            commands["C:/repo/build/a.obj"].as_deref(),
            Some("cl.exe @a.rsp")
        );
        assert!(commands["C:/repo/build/b.obj"].is_none());
        assert!(object_commands(Path::new("C:/repo/build"), "[{\"output\":\"a.obj\"}]").is_none());
    }
}
