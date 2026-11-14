//! Single-translation-unit probes. The compile command comes from a variant's
//! `compile_commands.json`, so the probe sees exactly the flags the build uses.
//! Only the object path is redirected, which leaves the build tree untouched.

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::path::{Path, PathBuf};

use anyhow::{Context, Result, bail};
use clap::{Args, ValueEnum};
use serde::Serialize;

use super::measure::timed;
use super::{CommonArgs, Compiler, Variant, apply_env, out_dir, toolchain_env};
use crate::process;

#[derive(Clone, Copy, ValueEnum, PartialEq, Eq, Debug)]
pub enum Kind {
    /// MSVC `/Bt+` frontend/backend split and `/d1reportTime` include costs.
    MsvcTime,
    /// clang `-ftime-trace` summarized into frontend, backend, includes and templates.
    ClangTrace,
    /// clang optimization remarks of the chosen passes.
    Remarks,
}

#[derive(Args)]
pub struct ProbeArgs {
    #[command(flatten)]
    pub common: CommonArgs,
    #[arg(long, value_enum)]
    pub kind: Kind,
    /// Variant whose build tree supplies compile_commands.json. The tree must
    /// be configured already (run a `run` experiment for it first).
    #[arg(long)]
    pub variant: String,
    /// Source files, matched as a path suffix inside compile_commands.json.
    #[arg(long = "file", required = true)]
    pub files: Vec<String>,
    /// Remark passes for `--kind remarks`.
    #[arg(long = "pass", default_values_t = ["loop-vectorize".to_string()])]
    pub passes: Vec<String>,
    #[arg(long, default_value_t = 12)]
    pub top: usize,
}

/// Splits a Windows command line the way CreateProcess consumers do: double
/// quotes group, and a backslash only escapes a following quote.
pub fn tokenize(line: &str) -> Vec<String> {
    let mut tokens = Vec::new();
    let mut current = String::new();
    let mut in_quotes = false;
    let mut started = false;
    let chars: Vec<char> = line.chars().collect();
    let mut i = 0;
    while i < chars.len() {
        let c = chars[i];
        match c {
            '\\' => {
                let mut slashes = 0;
                while i < chars.len() && chars[i] == '\\' {
                    slashes += 1;
                    i += 1;
                }
                started = true;
                if i < chars.len() && chars[i] == '"' {
                    current.extend(std::iter::repeat_n('\\', slashes / 2));
                    if slashes % 2 == 1 {
                        current.push('"');
                        i += 1;
                    }
                } else {
                    current.extend(std::iter::repeat_n('\\', slashes));
                }
                continue;
            }
            '"' => {
                in_quotes = !in_quotes;
                started = true;
            }
            c if c.is_whitespace() && !in_quotes => {
                if started {
                    tokens.push(std::mem::take(&mut current));
                    started = false;
                }
            }
            c => {
                current.push(c);
                started = true;
            }
        }
        i += 1;
    }
    if started {
        tokens.push(current);
    }
    tokens
}

struct Entry {
    directory: PathBuf,
    arguments: Vec<String>,
}

fn find_entry(build_dir: &Path, suffix: &str) -> Result<Entry> {
    let path = build_dir.join("compile_commands.json");
    let text = std::fs::read_to_string(&path)
        .with_context(|| format!("{} is missing; configure the variant first", path.display()))?;
    let entries: Vec<serde_json::Value> = serde_json::from_str(&text)?;
    let wanted = suffix.replace('\\', "/").to_lowercase();
    for entry in entries {
        let file = entry["file"]
            .as_str()
            .unwrap_or_default()
            .replace('\\', "/")
            .to_lowercase();
        if file.ends_with(&wanted) {
            let mut arguments =
                tokenize(entry["command"].as_str().context("entry has no command")?);
            if arguments
                .first()
                .is_some_and(|a| a.to_lowercase().contains("sccache"))
            {
                arguments.remove(0);
            }
            return Ok(Entry {
                directory: PathBuf::from(entry["directory"].as_str().unwrap_or_default()),
                arguments,
            });
        }
    }
    bail!("no compile command ends with '{suffix}'")
}

#[derive(Serialize, Default, Debug)]
pub struct TraceSummary {
    pub frontend_seconds: f64,
    pub backend_seconds: f64,
    pub source_seconds: f64,
    pub instantiate_class_seconds: f64,
    pub instantiate_function_seconds: f64,
    pub slowest_sources: Vec<(String, f64)>,
    pub slowest_instantiations: Vec<(String, f64)>,
}

/// Summarizes a clang `-ftime-trace` file. Durations are inclusive, so a
/// header's time contains the headers it includes.
pub fn summarize_trace(json: &str, top: usize) -> Result<TraceSummary> {
    let value: serde_json::Value = serde_json::from_str(json)?;
    let events = value["traceEvents"].as_array().context("no traceEvents")?;
    let mut summary = TraceSummary::default();
    let mut sources: BTreeMap<String, f64> = BTreeMap::new();
    let mut templates: BTreeMap<String, f64> = BTreeMap::new();
    // Source inclusions are async begin/end pairs (`ph` b/e) matched by id,
    // not complete events with a `dur`.
    let mut open_sources: BTreeMap<String, (f64, String)> = BTreeMap::new();
    for event in events {
        let name = event["name"].as_str().unwrap_or_default();
        let seconds = event["dur"].as_f64().unwrap_or(0.0) / 1e6;
        let detail = event["args"]["detail"].as_str().unwrap_or_default();
        match (name, event["ph"].as_str().unwrap_or_default()) {
            ("Total Frontend", _) => summary.frontend_seconds = seconds,
            ("Total Backend", _) => summary.backend_seconds = seconds,
            ("Total Source", _) => summary.source_seconds = seconds,
            ("Total InstantiateClass", _) => summary.instantiate_class_seconds = seconds,
            ("Total InstantiateFunction", _) => summary.instantiate_function_seconds = seconds,
            ("Source", "b") => {
                open_sources.insert(
                    event["id"].to_string(),
                    (event["ts"].as_f64().unwrap_or(0.0), detail.to_string()),
                );
            }
            ("Source", "e") => {
                if let Some((start, path)) = open_sources.remove(&event["id"].to_string()) {
                    *sources.entry(path).or_default() +=
                        (event["ts"].as_f64().unwrap_or(start) - start) / 1e6;
                }
            }
            ("InstantiateClass" | "InstantiateFunction", _) => {
                *templates.entry(detail.to_string()).or_default() += seconds;
            }
            _ => {}
        }
    }
    let ranked = |map: BTreeMap<String, f64>| {
        let mut rows: Vec<(String, f64)> = map.into_iter().collect();
        rows.sort_by(|a, b| b.1.total_cmp(&a.1));
        rows.truncate(top);
        rows
    };
    summary.slowest_sources = ranked(sources);
    summary.slowest_instantiations = ranked(templates);
    Ok(summary)
}

#[derive(Serialize, Default, Debug)]
pub struct MsvcTimeSummary {
    pub frontend_seconds: f64,
    pub backend_seconds: f64,
    pub include_total_seconds: f64,
    pub class_total_seconds: f64,
    pub function_total_seconds: f64,
    pub slowest_headers: Vec<(String, f64)>,
    pub slowest_classes: Vec<(String, f64)>,
    pub slowest_functions: Vec<(String, f64)>,
}

/// Summarizes `/Bt+ /d1reportTime` output. `c1xx.dll` is the frontend and `c2.dll`
/// the backend. Header times are inclusive of the headers they include.
pub fn summarize_msvc_time(text: &str, top: usize) -> Result<MsvcTimeSummary> {
    let phase = regex::Regex::new(r"time\(.*?(c1xx|c2)\.dll\)=([\d.]+)s")?;
    let entry = regex::Regex::new(r"^\t{2,}(.+): ([\d.]+)s$")?;
    let total = regex::Regex::new(r"^\tTotal: ([\d.]+)s$")?;
    let mut summary = MsvcTimeSummary::default();
    let mut section = "";
    let mut rows: [Vec<(String, f64)>; 3] = Default::default();
    for line in text.lines() {
        if let Some(caps) = phase.captures(line) {
            let seconds: f64 = caps[2].parse().unwrap_or(0.0);
            if &caps[1] == "c1xx" {
                summary.frontend_seconds = seconds
            } else {
                summary.backend_seconds = seconds
            }
            continue;
        }
        match line {
            "Include Headers:" => section = "h",
            "Class Definitions:" => section = "c",
            "Function Definitions:" => section = "f",
            _ => {}
        }
        if let Some(caps) = total.captures(line) {
            let seconds: f64 = caps[1].parse().unwrap_or(0.0);
            match section {
                "h" => summary.include_total_seconds = seconds,
                "c" => summary.class_total_seconds = seconds,
                "f" => summary.function_total_seconds = seconds,
                _ => {}
            }
        } else if let Some(caps) = entry.captures(line) {
            let slot = match section {
                "h" => 0,
                "c" => 1,
                "f" => 2,
                _ => continue,
            };
            rows[slot].push((caps[1].to_string(), caps[2].parse().unwrap_or(0.0)));
        }
    }
    let ranked = |mut list: Vec<(String, f64)>| {
        list.sort_by(|a, b| b.1.total_cmp(&a.1));
        list.truncate(top);
        list
    };
    let [headers, classes, functions] = rows;
    // A header reached through several include paths appears once per path.
    let mut longest: BTreeMap<String, f64> = BTreeMap::new();
    for (path, seconds) in headers {
        let slot = longest.entry(path).or_default();
        *slot = slot.max(seconds);
    }
    summary.slowest_headers = ranked(longest.into_iter().collect());
    summary.slowest_classes = ranked(classes);
    summary.slowest_functions = ranked(functions);
    Ok(summary)
}

#[derive(Serialize, Default, Debug)]
pub struct RemarkSummary {
    pub applied: usize,
    pub missed: usize,
    pub analysis: usize,
    /// Missed or analysis reasons with numbers collapsed, most frequent first,
    /// each with one example location.
    pub top_reasons: Vec<(String, usize, String)>,
}

/// Counts remarks of the form `file:line:col: remark: text [-Rpass...=pass]`.
pub fn summarize_remarks(text: &str, top: usize) -> RemarkSummary {
    let pattern = regex::Regex::new(
        r"^(.+?(?::\d+:\d+|\(\d+,\d+\))): remark: (.*) \[-Rpass(-missed|-analysis)?=[\w-]+\]$",
    )
    .expect("static pattern");
    let digits = regex::Regex::new(r"\d+").expect("static pattern");
    let mut summary = RemarkSummary::default();
    let mut reasons: BTreeMap<String, (usize, String)> = BTreeMap::new();
    for line in text.lines() {
        let Some(captures) = pattern.captures(line.trim_end()) else {
            continue;
        };
        match captures.get(3).map(|m| m.as_str()) {
            None => summary.applied += 1,
            Some(kind) => {
                if kind == "-missed" {
                    summary.missed += 1;
                } else {
                    summary.analysis += 1;
                }
                let reason = digits.replace_all(&captures[2], "N").into_owned();
                let slot = reasons
                    .entry(reason)
                    .or_insert((0, captures[1].to_string()));
                slot.0 += 1;
            }
        }
    }
    let mut rows: Vec<(String, usize, String)> = reasons
        .into_iter()
        .map(|(reason, (count, at))| (reason, count, at))
        .collect();
    rows.sort_by_key(|row| std::cmp::Reverse(row.1));
    rows.truncate(top);
    summary.top_reasons = rows;
    summary
}

pub fn run(repo: &Path, args: ProbeArgs) -> Result<()> {
    let variant = Variant::parse(&args.variant)?;
    match (args.kind, variant.compiler) {
        (Kind::MsvcTime, Compiler::ClangCl) => bail!("msvc-time needs an msvc variant"),
        (Kind::ClangTrace | Kind::Remarks, Compiler::Msvc) => {
            bail!("{:?} needs a clangcl variant", args.kind)
        }
        _ => {}
    }
    let out = out_dir(repo, &args.common, "probe")?;
    let env = toolchain_env()?;
    let build_dir = variant.build_dir(repo);
    let mut report = String::new();
    let _ = writeln!(report, "# Probe {:?} on {}\n", args.kind, variant.id);
    for file in &args.files {
        let entry = find_entry(&build_dir, file)?;
        let stem = Path::new(file)
            .file_stem()
            .unwrap_or_default()
            .to_string_lossy()
            .into_owned();
        let object = out.join(format!("{stem}.obj"));
        let log = out.join("logs").join(format!("{:?}-{stem}.log", args.kind));
        let mut extra: Vec<String> = vec![format!("/Fo{}", object.display())];
        match args.kind {
            Kind::MsvcTime => extra.extend(["/Bt+".into(), "/d1reportTime".into()]),
            Kind::ClangTrace => extra.push("/clang:-ftime-trace".into()),
            Kind::Remarks => {
                for pass in &args.passes {
                    for flag in ["Rpass", "Rpass-missed", "Rpass-analysis"] {
                        extra.push(format!("/clang:-{flag}={pass}"));
                    }
                }
            }
        }
        // CMake's clang-cl rule ends in `-c -- <source>`, and everything after
        // `--` is an input file, so the extra options go in front of it.
        let mut arguments = entry.arguments.clone();
        let at = arguments
            .iter()
            .position(|a| a == "--")
            .unwrap_or(arguments.len());
        arguments.splice(at..at, extra);
        let mut command = process::command(&arguments[0]);
        command.args(&arguments[1..]).current_dir(&entry.directory);
        apply_env(&mut command, &env);
        // `timed` appends, so a repeated probe would otherwise parse stale output.
        let _ = std::fs::remove_file(&log);
        let timing = timed(command, &log)?;
        let _ = writeln!(
            report,
            "## {file}\n\nWall {:.1} s, exit {}, log `{}`.\n",
            timing.wall_seconds,
            timing.exit_code,
            log.display()
        );
        if timing.exit_code != 0 {
            continue;
        }
        let log_text = std::fs::read_to_string(&log).unwrap_or_default();
        match args.kind {
            Kind::MsvcTime => {
                let summary = summarize_msvc_time(&log_text, args.top)?;
                let _ = writeln!(
                    report,
                    "Frontend (c1xx) {:.2} s, backend (c2) {:.2} s. Include time {:.2} s, class definitions {:.2} s, function definitions {:.2} s.\n\nSlowest headers (inclusive):\n",
                    summary.frontend_seconds,
                    summary.backend_seconds,
                    summary.include_total_seconds,
                    summary.class_total_seconds,
                    summary.function_total_seconds
                );
                for (name, seconds) in &summary.slowest_headers {
                    let _ = writeln!(report, "- {seconds:.2} s {name}");
                }
                let _ = writeln!(report, "\nSlowest class definitions:\n");
                for (name, seconds) in &summary.slowest_classes {
                    let _ = writeln!(report, "- {seconds:.3} s {name}");
                }
                let _ = writeln!(report, "\nSlowest function definitions:\n");
                for (name, seconds) in &summary.slowest_functions {
                    let _ = writeln!(report, "- {seconds:.3} s {name}");
                }
                report.push('\n');
            }
            Kind::ClangTrace => {
                let trace_path =
                    PathBuf::from(format!("{}.json", object.with_extension("").display()));
                let candidates = [trace_path, object.with_extension("json")];
                let Some(path) = candidates.iter().find(|p| p.is_file()) else {
                    let _ = writeln!(
                        report,
                        "No time-trace file was written next to the object.\n"
                    );
                    continue;
                };
                let summary = summarize_trace(&std::fs::read_to_string(path)?, args.top)?;
                let _ = writeln!(
                    report,
                    "Frontend {:.2} s (source inclusion {:.2} s, class instantiation {:.2} s, function instantiation {:.2} s), backend {:.2} s.\n\nSlowest sources (inclusive):\n",
                    summary.frontend_seconds,
                    summary.source_seconds,
                    summary.instantiate_class_seconds,
                    summary.instantiate_function_seconds,
                    summary.backend_seconds
                );
                for (name, seconds) in &summary.slowest_sources {
                    let _ = writeln!(report, "- {seconds:.2} s {name}");
                }
                let _ = writeln!(report, "\nSlowest template instantiations:\n");
                for (name, seconds) in &summary.slowest_instantiations {
                    let _ = writeln!(report, "- {seconds:.2} s {name}");
                }
                report.push('\n');
            }
            Kind::Remarks => {
                let summary = summarize_remarks(&log_text, args.top);
                let _ = writeln!(
                    report,
                    "Applied {}, missed {}, analysis {}.\n",
                    summary.applied, summary.missed, summary.analysis
                );
                for (reason, count, at) in &summary.top_reasons {
                    let _ = writeln!(report, "- {count}x {reason} (e.g. {at})");
                }
                report.push('\n');
            }
        }
    }
    std::fs::write(out.join("probe.md"), &report)?;
    println!("report: {}", out.join("probe.md").display());
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tokenizer_keeps_quoted_paths_and_escaped_quotes() {
        let tokens = tokenize(r#"cl.exe /DX="a b" "C:\Program Files\x.cpp" /DY=\"q\""#);
        assert_eq!(
            tokens,
            vec!["cl.exe", "/DX=a b", r"C:\Program Files\x.cpp", "/DY=\"q\""]
        );
    }

    #[test]
    fn remarks_split_applied_missed_and_collapse_numbers() {
        let text = "\
a.cpp:10:5: remark: vectorized loop (vectorization width: 4, interleaved count: 2) [-Rpass=loop-vectorize]\n\
a.cpp:20:5: remark: loop not vectorized: call instruction cannot be vectorized [-Rpass-analysis=loop-vectorize]\n\
a.cpp:30:5: remark: loop not vectorized [-Rpass-missed=loop-vectorize]\n\
a.cpp:40:5: remark: loop not vectorized: call instruction cannot be vectorized [-Rpass-analysis=loop-vectorize]\n\
C:\\x\\b.cpp(50,3): remark: vectorized loop (vectorization width: 8, interleaved count: 1) [-Rpass=loop-vectorize]\n";
        let summary = summarize_remarks(text, 5);
        assert_eq!(
            (summary.applied, summary.missed, summary.analysis),
            (2, 1, 2)
        );
        assert_eq!(summary.top_reasons[0].1, 2);
    }

    #[test]
    fn msvc_time_reads_phases_through_paths_with_parentheses_and_ranks_sections() {
        let text = "time(C:\\Program Files (x86)\\VS\\bin\\c1xx.dll)=6.5s < 1 - 2 > BB [a.cpp]\n\
time(C:\\Program Files (x86)\\VS\\bin\\c2.dll)=0.5s < 3 - 4 > BB [a.cpp]\n\
a.cpp\n\
Include Headers:\n\
\tCount: 2\n\
\t\tC:\\x\\a.h: 2.0s\n\
\t\t\tC:\\x\\b.h: 1.0s\n\
\tTotal: 3.0s\n\
Class Definitions:\n\
\tCount: 1\n\
\t\tstd::vector<int>: 0.25s\n\
\tTotal: 0.25s\n";
        let summary = summarize_msvc_time(text, 5).unwrap();
        assert_eq!(
            (summary.frontend_seconds, summary.backend_seconds),
            (6.5, 0.5)
        );
        assert_eq!(summary.include_total_seconds, 3.0);
        assert_eq!(summary.slowest_headers[0].0, "C:\\x\\a.h");
        assert_eq!(summary.slowest_classes[0].1, 0.25);
    }

    #[test]
    fn trace_summary_ranks_sources_and_instantiations() {
        let json = r#"{"traceEvents":[
            {"name":"Total Frontend","dur":3000000},
            {"name":"Total Backend","dur":1000000},
            {"name":"Source","ph":"b","id":1,"ts":0,"args":{"detail":"big.h"}},
            {"name":"Source","ph":"e","id":1,"ts":2000000},
            {"name":"Source","ph":"b","id":2,"ts":0,"args":{"detail":"small.h"}},
            {"name":"Source","ph":"e","id":2,"ts":500000},
            {"name":"InstantiateClass","dur":400000,"args":{"detail":"std::vector<int>"}}]}"#;
        let summary = summarize_trace(json, 3).unwrap();
        assert_eq!(summary.frontend_seconds, 3.0);
        assert_eq!(summary.slowest_sources[0].0, "big.h");
        assert_eq!(summary.slowest_instantiations.len(), 1);
    }
}
