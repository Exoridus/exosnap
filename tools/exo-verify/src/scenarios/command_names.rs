//! A regression guard for the control command names scenarios send.
//!
//! There are two control endpoints with overlapping vocabularies: the
//! application answers `update.getState`, the updater answers
//! `updater.getState`. A scenario that sends the application's spelling to
//! the updater is refused with "Unknown command", which then reports as a
//! product failure rather than the scenario mistake it actually is.
//!
//! The command lists are read from the C++ policy tables rather than copied
//! here: a second copy of a vocabulary is a copy that goes stale.

#[cfg(test)]
mod tests {
    use regex::Regex;
    use std::collections::HashSet;
    use std::path::{Path, PathBuf};
    use std::sync::LazyLock;

    fn repo_root() -> PathBuf {
        Path::new(env!("CARGO_MANIFEST_DIR")).join("../..")
    }

    /// The names a policy table declares, as `QStringLiteral("x.y")` entries.
    fn declared_commands(relative_path: &str) -> HashSet<String> {
        static PATTERN: LazyLock<Regex> = LazyLock::new(|| {
            Regex::new(r#"QStringLiteral\("([a-z][a-zA-Z]*\.[a-zA-Z]+)"\)"#).unwrap()
        });
        let path = repo_root().join(relative_path);
        let source = std::fs::read_to_string(&path)
            .unwrap_or_else(|error| panic!("reading {relative_path}: {error}"));
        PATTERN
            .captures_iter(&source)
            .map(|c| c[1].to_string())
            .collect()
    }

    /// Every command literal a `scenarios/*.rs` file passes as the first
    /// argument to a `Client`-sending method (`call`, `request`, `poll`),
    /// with the receiver expression so the caller can tell which endpoint it
    /// went to. Commands passed through a variable rather than a literal are
    /// not found here: this is a text scan, not a type-aware analysis.
    struct Sent {
        file: String,
        line: usize,
        receiver: String,
        command: String,
    }

    fn sent_commands() -> Vec<Sent> {
        static PATTERN: LazyLock<Regex> = LazyLock::new(|| {
            Regex::new(
                r#"([A-Za-z_][A-Za-z0-9_]*)(?:\s*\.\s*([A-Za-z_][A-Za-z0-9_]*))?\s*\.\s*(?:call|request|poll)\s*\(\s*"([a-z][a-zA-Z]*\.[a-zA-Z]+)""#,
            )
            .unwrap()
        });
        let dir = repo_root().join("tools/exo-verify/src/scenarios");
        let mut sent = Vec::new();
        for entry in std::fs::read_dir(&dir).expect("scenarios directory is readable") {
            let entry = entry.expect("scenarios directory entry is readable");
            let path = entry.path();
            if path.extension().and_then(|e| e.to_str()) != Some("rs") {
                continue;
            }
            let name = path
                .file_name()
                .and_then(|n| n.to_str())
                .unwrap_or_default()
                .to_string();
            let source = std::fs::read_to_string(&path)
                .unwrap_or_else(|error| panic!("reading {name}: {error}"));
            for capture in PATTERN.captures_iter(&source) {
                let whole = capture.get(0).unwrap();
                let line = source[..whole.start()].matches('\n').count() + 1;
                let receiver = match capture.get(2) {
                    Some(second) => format!("{}.{}", &capture[1], second.as_str()),
                    None => capture[1].to_string(),
                };
                sent.push(Sent {
                    file: name.clone(),
                    line,
                    receiver,
                    command: capture[3].to_string(),
                });
            }
        }
        sent
    }

    /// Whether a receiver expression (`updater`, `app.client`, `client`, ...)
    /// names the updater endpoint. A member starting with `updater` names
    /// that channel; all other receivers name the application's channel.
    fn is_updater_receiver(receiver: &str) -> bool {
        receiver
            .split('.')
            .any(|part| part.to_ascii_lowercase().starts_with("updater"))
    }

    #[test]
    fn both_policy_tables_were_readable_and_non_trivial() {
        let app_commands = declared_commands("app/live_verify/LiveVerifyCommandPolicy.cpp");
        let updater_commands = declared_commands("apps/updater/UpdaterCommandPolicy.cpp");
        assert!(
            app_commands.len() > 30,
            "the app policy declared only {} commands",
            app_commands.len()
        );
        assert!(
            updater_commands.len() > 5,
            "the updater policy declared only {} commands",
            updater_commands.len()
        );
        assert!(app_commands.contains("record.start"));
        assert!(updater_commands.contains("updater.getState"));
    }

    #[test]
    fn every_command_a_scenario_sends_exists_on_the_endpoint_it_is_sent_to() {
        let app_commands = declared_commands("app/live_verify/LiveVerifyCommandPolicy.cpp");
        let updater_commands = declared_commands("apps/updater/UpdaterCommandPolicy.cpp");
        let sent = sent_commands();
        assert!(
            sent.len() > 10,
            "only {} command sites were found; the scan is probably broken",
            sent.len()
        );

        let mut problems = Vec::new();
        for site in &sent {
            let is_updater = is_updater_receiver(&site.receiver);
            let (known, endpoint) = if is_updater {
                (&updater_commands, "updater")
            } else {
                (&app_commands, "app")
            };
            if !known.contains(&site.command) {
                let other = if is_updater {
                    &app_commands
                } else {
                    &updater_commands
                };
                let hint = if other.contains(&site.command) {
                    format!(
                        " (that is the {}'s spelling)",
                        if is_updater { "app" } else { "updater" }
                    )
                } else {
                    String::new()
                };
                problems.push(format!(
                    "{}:{} sends '{}' to the {endpoint} endpoint{hint}",
                    site.file, site.line, site.command
                ));
            }
        }
        assert!(
            problems.is_empty(),
            "unknown commands:\n{}",
            problems.join("\n")
        );
    }

    #[test]
    fn an_unknown_command_is_detected() {
        let app_commands: HashSet<String> = ["record.start".to_string()].into_iter().collect();
        assert!(!app_commands.contains("record.notARealCommand"));
        let updater_commands: HashSet<String> =
            ["updater.getState".to_string()].into_iter().collect();
        let fixture = [
            Sent {
                file: "fixture.rs".to_string(),
                line: 1,
                receiver: "app".to_string(),
                command: "record.notARealCommand".to_string(),
            },
            Sent {
                file: "fixture.rs".to_string(),
                line: 2,
                receiver: "updater".to_string(),
                command: "update.getState".to_string(),
            },
        ];
        let mut problems = 0;
        for site in &fixture {
            let is_updater = is_updater_receiver(&site.receiver);
            let known = if is_updater {
                &updater_commands
            } else {
                &app_commands
            };
            if !known.contains(&site.command) {
                problems += 1;
            }
        }
        assert_eq!(
            problems, 2,
            "the fixture's two unknown commands were not both caught"
        );
    }
}
