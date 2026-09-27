//! Reading a ctest log: the counts, the failing tests and the failing gtest
//! cases with what each printed.

use std::sync::LazyLock;

use regex::Regex;

/// The counts ctest printed, or the fact that it printed none this run could
/// read. ctest writes two shapes: "N% tests passed, M tests failed out of T"
/// when something failed and "N% tests passed out of T" when nothing did.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Summary {
    pub line: Option<String>,
    pub time_line: Option<String>,
    pub passed: usize,
    pub failed: usize,
    pub parsed: bool,
}

pub fn summary(lines: &[String]) -> Summary {
    static LINE: LazyLock<Regex> =
        LazyLock::new(|| Regex::new(r"(?i)^\s*\d+% tests passed").unwrap());
    static TIME: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"(?i)Total Test time").unwrap());
    static FAILED_SHAPE: LazyLock<Regex> = LazyLock::new(|| {
        Regex::new(r"(?i)(\d+)%\s+tests passed,\s*(\d+)\s+tests failed out of\s*(\d+)").unwrap()
    });
    static PASSED_SHAPE: LazyLock<Regex> =
        LazyLock::new(|| Regex::new(r"(?i)(\d+)%\s+tests passed out of\s*(\d+)").unwrap());

    let line = lines.iter().rev().find(|l| LINE.is_match(l)).cloned();
    let time_line = lines.iter().rev().find(|l| TIME.is_match(l)).cloned();
    let mut result = Summary {
        line: line.clone(),
        time_line,
        ..Summary::default()
    };
    let Some(line) = line else { return result };
    let number = |text: &str| text.parse::<usize>().ok();
    if let Some(captures) = FAILED_SHAPE.captures(&line) {
        if let (Some(failed), Some(total)) = (number(&captures[2]), number(&captures[3])) {
            result.failed = failed;
            result.passed = total.saturating_sub(failed);
            result.parsed = true;
        }
    } else if let Some(captures) = PASSED_SHAPE.captures(&line)
        && let Some(total) = number(&captures[2])
    {
        result.passed = total;
        result.parsed = true;
    }
    result
}

/// The tests ctest lists under "The following tests FAILED:", in log order,
/// each once. The line does not end at the status: ctest appends the test's
/// LABELS after it, and every test here declares a phase label.
pub fn failed_tests(lines: &[String]) -> Vec<String> {
    static FAILED: LazyLock<Regex> = LazyLock::new(|| {
        Regex::new(
            r"(?i)^\s*\d+\s+-\s+(.+?)\s+\((Failed|Timeout|Not Run|Exception[^)]*|Subprocess aborted|Child aborted|SEGFAULT|Illegal|Numerical|Other)\)",
        )
        .unwrap()
    });
    unique(
        lines
            .iter()
            .filter_map(|l| FAILED.captures(l).map(|c| c[1].to_string())),
    )
}

/// The failing gtest cases, as gtest_main prints them: `[  FAILED  ] Suite.Case`.
pub fn failed_cases(lines: &[String]) -> Vec<String> {
    static CASE: LazyLock<Regex> = LazyLock::new(|| {
        Regex::new(r"(?i)\[\s*FAILED\s*\]\s+([A-Za-z0-9_./]+\.[A-Za-z0-9_/]+)").unwrap()
    });
    unique(
        lines
            .iter()
            .filter_map(|l| CASE.captures(l).map(|c| c[1].to_string())),
    )
}

/// What gtest printed between `[ RUN ] case` and `[ FAILED ] case`, with
/// ctest's `<n>: ` line prefix removed so the assertion reads the way gtest
/// wrote it. `None` when the case never started in this log.
pub fn case_block(lines: &[String], case: &str) -> Option<Vec<String>> {
    static PREFIX: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"^\s*\d+:\s?").unwrap());
    let escaped = regex::escape(case);
    let run = Regex::new(&format!(r"(?i)\[\s*RUN\s*\]\s+{escaped}\s*$")).ok()?;
    let failed = Regex::new(&format!(r"(?i)\[\s*FAILED\s*\]\s+{escaped}")).ok()?;
    let start = lines.iter().position(|l| run.is_match(l))?;
    Some(
        lines[start + 1..]
            .iter()
            .take_while(|l| !failed.is_match(l))
            .map(|l| PREFIX.replace(l, "").into_owned())
            .collect(),
    )
}

/// The failing tests and gtest cases, each case with the assertion it
/// printed: on CI the full log is an artifact that has to be downloaded
/// first, so a report that stops at the name sends the reader on a detour.
/// Each block is capped so a case that logs a lot cannot bury the rest.
pub fn failure_report(lines: &[String]) -> Vec<String> {
    const MAX_BLOCK_LINES: usize = 40;
    let mut report = Vec::new();
    let failed = failed_tests(lines);
    if !failed.is_empty() {
        report.push(String::new());
        report.push("Failed test binaries:".to_string());
        report.extend(failed.iter().map(|name| format!("  {name}")));
    }
    let cases = failed_cases(lines);
    if cases.is_empty() {
        return report;
    }
    report.push(String::new());
    report.push("Failing gtest cases:".to_string());
    report.extend(cases.iter().map(|case| format!("  {case}")));
    for case in &cases {
        let Some(block) = case_block(lines, case) else {
            continue;
        };
        report.push(String::new());
        report.push(format!("--- {case}"));
        report.extend(
            block
                .iter()
                .filter(|l| !l.trim().is_empty())
                .take(MAX_BLOCK_LINES)
                .map(|l| format!("  {l}")),
        );
        if block.len() > MAX_BLOCK_LINES {
            report.push(format!(
                "  ... ({} more lines in the log)",
                block.len() - MAX_BLOCK_LINES
            ));
        }
    }
    report
}

fn unique(items: impl Iterator<Item = String>) -> Vec<String> {
    let mut seen = Vec::new();
    for item in items {
        if !seen.contains(&item) {
            seen.push(item);
        }
    }
    seen
}

#[cfg(test)]
mod tests {
    use super::*;

    fn lines(text: &str) -> Vec<String> {
        text.lines().map(str::to_string).collect()
    }

    #[test]
    fn both_summary_shapes_are_read() {
        let failing = summary(&lines(
            "junk\n67% tests passed, 1 tests failed out of 3\n\nTotal Test time (real) =   0.02 sec\n",
        ));
        assert!(failing.parsed);
        assert_eq!((failing.passed, failing.failed), (2, 1));
        assert!(failing.time_line.unwrap().contains("0.02 sec"));

        let green = summary(&lines("100% tests passed out of 4\n"));
        assert!(green.parsed);
        assert_eq!((green.passed, green.failed), (4, 0));

        let none = summary(&lines("nothing here\n"));
        assert!(!none.parsed);
        assert_eq!(none.line, None);
    }

    #[test]
    fn a_failing_test_is_named_although_labels_follow_its_status() {
        let log = lines(
            "The following tests FAILED:\n\
             \t  3 - fixture.labelled_failure (Failed)             phase.hermetic quick\n\
             \t  4 - engine.muxer (Timeout)\n\
             \t  5 - quick.qml.x (Exception: SegFault)  quick\n\
             \t  3 - fixture.labelled_failure (Failed)             phase.hermetic quick\n",
        );
        assert_eq!(
            failed_tests(&log),
            vec!["fixture.labelled_failure", "engine.muxer", "quick.qml.x"]
        );
    }

    #[test]
    fn the_failing_case_block_is_cut_at_its_failure_line() {
        let log = lines(
            "2: [ RUN      ] Suite.Case\n\
             2: value.cpp(12): error: Expected equality\n\
             2:   actual: 1\n\
             2: [  FAILED  ] Suite.Case (0 ms)\n\
             [  FAILED  ] Suite.Case\n\
             [  FAILED  ] Suite.Case\n",
        );
        assert_eq!(failed_cases(&log), vec!["Suite.Case"]);
        assert_eq!(
            case_block(&log, "Suite.Case").unwrap(),
            vec!["value.cpp(12): error: Expected equality", "  actual: 1"]
        );
        assert_eq!(case_block(&log, "Other.Case"), None);
    }

    #[test]
    fn the_failure_report_names_tests_cases_and_assertions() {
        let mut text = String::from(
            "The following tests FAILED:\n\t  2 - engine.value (Failed)  phase.hermetic\n\
             2: [ RUN      ] Suite.Case\n",
        );
        for n in 0..45 {
            text.push_str(&format!("2: line {n}\n"));
        }
        text.push_str("2: [  FAILED  ] Suite.Case (0 ms)\n");
        let report = failure_report(&lines(&text));
        assert!(report.contains(&"Failed test binaries:".to_string()));
        assert!(report.contains(&"  engine.value".to_string()));
        assert!(report.contains(&"--- Suite.Case".to_string()));
        assert!(report.contains(&"  line 39".to_string()));
        assert!(!report.contains(&"  line 40".to_string()));
        assert_eq!(report.last().unwrap(), "  ... (5 more lines in the log)");
    }
}
