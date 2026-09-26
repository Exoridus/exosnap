//! Command-line front end for the product control channel.
//!
//! Machine-readable by default: the JSON result goes to stdout and nothing
//! else does, so a caller can pipe it straight into a JSON parser.
//! Diagnostics go to stderr.
//!
//! Exit codes are distinct on purpose, because "the application refused the
//! command" and "the application never answered" are different acceptance
//! outcomes that a runner must not collapse:
//!
//! - 0 success
//! - 2 usage error
//! - 3 could not connect, handshake refused, protocol mismatch, or the
//!   control channel was lost mid-operation
//! - 4 the command was answered with a refusal
//! - 5 timed out waiting for a response or an event

use serde_json::{Map, Value, json};
use std::process::ExitCode;
use std::time::Duration;

use crate::control::{self, Refusal};

#[derive(clap::ValueEnum, Debug, Clone, Copy, PartialEq, Eq)]
#[value(rename_all = "lowercase")]
pub enum Verb {
    Hello,
    Query,
    Command,
    Wait,
    Capabilities,
    Describe,
    State,
}

#[derive(clap::Args, Debug)]
pub struct ControlArgs {
    /// The operation to perform.
    pub verb: Verb,
    /// The command, event or query domain name. Unused by hello, capabilities,
    /// describe and state.
    pub name: Option<String>,
    /// The run id of the armed process to connect to.
    #[arg(long)]
    pub run_id: String,
    /// JSON object for `command` parameters, e.g. '{"screen":"\\.\\DISPLAY2"}'.
    #[arg(long)]
    pub params: Option<String>,
    /// key=value pairs an awaited event's data must match.
    #[arg(long = "where")]
    pub filters: Vec<String>,
    #[arg(long, default_value_t = 30)]
    pub timeout_seconds: u64,
    /// The envelope version to speak. 2 is the default; 1 exercises the
    /// backward-compatible surface.
    #[arg(long, default_value_t = control::PROTOCOL, value_parser = clap::value_parser!(u64).range(1..=2))]
    pub protocol: u64,
}

/// `query <domain>` is sugar for the domain's snapshot command; anything else
/// is passed through verbatim so the CLI can never drift from the server
/// allowlist.
const QUERY_DOMAINS: &[(&str, &str)] = &[
    ("system", "system.snapshot"),
    ("app", "app.snapshot"),
    ("window", "window.snapshot"),
    ("preview", "preview.snapshot"),
    ("record", "record.snapshot"),
    ("result", "record.result"),
    ("ui", "ui.getState"),
    ("overlay", "overlay.snapshot"),
    ("editor", "editor.snapshot"),
    ("diagnostics", "diagnostics.snapshot"),
];

fn resolve_query_command(name: &str) -> String {
    QUERY_DOMAINS
        .iter()
        .find(|(domain, _)| *domain == name)
        .map(|(_, command)| command.to_string())
        .unwrap_or_else(|| name.to_string())
}

/// What the CLI intends to do, resolved purely from arguments: connecting
/// and sending it is a separate step, so this can be built and checked
/// without a live control channel.
#[derive(Debug, PartialEq)]
enum Intent {
    /// `hello`: the handshake already ran during connect, so this only
    /// reports the identity it produced.
    Identity,
    Call(String, Value),
    Wait(String, Value),
}

fn require_name<'a>(name: Option<&'a str>, usage: &str) -> Result<&'a str, String> {
    match name.map(str::trim) {
        Some(name) if !name.is_empty() => Ok(name),
        _ => Err(usage.to_string()),
    }
}

/// Parses `--params` into the JSON object `command` sends. Missing or blank
/// text is no parameters at all; text that is not a JSON object is a usage
/// error caught here rather than surfacing later as a confusing protocol
/// refusal.
fn parse_params(text: Option<&str>) -> Result<Value, String> {
    let text = text.map(str::trim).unwrap_or("");
    if text.is_empty() {
        return Ok(json!({}));
    }
    match serde_json::from_str::<Value>(text) {
        Ok(Value::Object(map)) => Ok(Value::Object(map)),
        Ok(other) => Err(format!("--params must be a JSON object, got: {other}")),
        Err(error) => Err(format!("--params is not valid JSON: {error}")),
    }
}

/// Parses `--where key=value` pairs into the filter object `wait` matches an
/// event's data against.
fn parse_where(entries: &[String]) -> Result<Value, String> {
    let mut filter = Map::new();
    for pair in entries {
        let Some((key, value)) = pair.split_once('=') else {
            return Err(format!("--where entries must be key=value, got '{pair}'"));
        };
        filter.insert(key.to_string(), Value::String(value.to_string()));
    }
    Ok(Value::Object(filter))
}

fn build_intent(args: &ControlArgs) -> Result<Intent, String> {
    let name = args.name.as_deref();
    match args.verb {
        Verb::Hello => Ok(Intent::Identity),
        Verb::Capabilities => Ok(Intent::Call("system.capabilities".to_string(), json!({}))),
        Verb::Describe => Ok(Intent::Call("ipc.describe".to_string(), json!({}))),
        Verb::State => Ok(Intent::Call("ui.getState".to_string(), json!({}))),
        Verb::Query => {
            let domains: Vec<&str> = QUERY_DOMAINS.iter().map(|(domain, _)| *domain).collect();
            let name = require_name(
                name,
                &format!("query needs a domain: {}", domains.join(", ")),
            )?;
            Ok(Intent::Call(resolve_query_command(name), json!({})))
        }
        Verb::Command => {
            let name = require_name(name, "command needs a command name")?;
            let params = parse_params(args.params.as_deref())?;
            Ok(Intent::Call(name.to_string(), params))
        }
        Verb::Wait => {
            let name = require_name(name, "wait needs an event name")?;
            let filter = parse_where(&args.filters)?;
            Ok(Intent::Wait(name.to_string(), filter))
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Outcome {
    Success,
    Usage,
    ConnectFailed,
    Refused,
    TimedOut,
}

fn exit_code(outcome: Outcome) -> ExitCode {
    match outcome {
        Outcome::Success => ExitCode::from(0),
        Outcome::Usage => ExitCode::from(2),
        Outcome::ConnectFailed => ExitCode::from(3),
        Outcome::Refused => ExitCode::from(4),
        Outcome::TimedOut => ExitCode::from(5),
    }
}

fn refusal_value(refusal: &Refusal) -> Value {
    json!({
        "code": refusal.code,
        "message": refusal.message,
        "requires": refusal.requires,
        "actual": refusal.actual,
    })
}

/// Whether `error` is `Client::request`'s own "no response within timeout"
/// failure for `command`, as opposed to the control channel having gone
/// away. The two share the `Result` error type, so the CLI tells them apart
/// by the fixed message `request` always uses for a timeout.
fn is_response_timeout(command: &str, error: &anyhow::Error) -> bool {
    error
        .to_string()
        .starts_with(&format!("no response to {command} "))
}

fn print_result(value: &Value) {
    match serde_json::to_string_pretty(value) {
        Ok(text) => println!("{text}"),
        Err(error) => eprintln!("could not render the result as JSON: {error}"),
    }
}

pub fn run(args: &ControlArgs) -> ExitCode {
    let intent = match build_intent(args) {
        Ok(intent) => intent,
        Err(message) => {
            eprintln!("{message}");
            return exit_code(Outcome::Usage);
        }
    };

    let mut client = match control::Client::connect_protocol(
        "LiveVerify",
        &args.run_id,
        Duration::from_secs(args.timeout_seconds),
        args.protocol,
    ) {
        Ok(client) => client,
        Err(error) => {
            eprintln!("{error:#}");
            return exit_code(Outcome::ConnectFailed);
        }
    };

    match intent {
        Intent::Identity => {
            print_result(&client.identity);
            exit_code(Outcome::Success)
        }
        Intent::Call(command, params) => {
            match client.request(&command, params, Duration::from_secs(args.timeout_seconds)) {
                Ok(Ok(result)) => {
                    print_result(&result);
                    exit_code(Outcome::Success)
                }
                Ok(Err(refusal)) => {
                    print_result(&refusal_value(&refusal));
                    exit_code(Outcome::Refused)
                }
                Err(error) => {
                    eprintln!("{error:#}");
                    if is_response_timeout(&command, &error) {
                        exit_code(Outcome::TimedOut)
                    } else {
                        exit_code(Outcome::ConnectFailed)
                    }
                }
            }
        }
        Intent::Wait(event, filter) => {
            match client.wait_event(&event, &filter, Duration::from_secs(args.timeout_seconds)) {
                Ok(Some(observed)) => {
                    print_result(&observed);
                    exit_code(Outcome::Success)
                }
                Ok(None) => {
                    eprintln!(
                        "Timed out after {}s waiting for '{event}'",
                        args.timeout_seconds
                    );
                    exit_code(Outcome::TimedOut)
                }
                Err(error) => {
                    eprintln!("{error:#}");
                    exit_code(Outcome::ConnectFailed)
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn args(verb: Verb, name: Option<&str>) -> ControlArgs {
        ControlArgs {
            verb,
            name: name.map(str::to_string),
            run_id: "run-id".to_string(),
            params: None,
            filters: Vec::new(),
            timeout_seconds: 30,
            protocol: control::PROTOCOL,
        }
    }

    #[test]
    fn query_sugar_resolves_known_domains_and_passes_through_the_rest() {
        assert_eq!(resolve_query_command("record"), "record.snapshot");
        assert_eq!(resolve_query_command("result"), "record.result");
        assert_eq!(resolve_query_command("ui"), "ui.getState");
        assert_eq!(
            resolve_query_command("diagnostics.results"),
            "diagnostics.results"
        );
    }

    #[test]
    fn hello_needs_no_name_and_reports_the_handshake_identity() {
        assert_eq!(
            build_intent(&args(Verb::Hello, None)).unwrap(),
            Intent::Identity
        );
    }

    #[test]
    fn capabilities_describe_and_state_map_to_fixed_commands() {
        assert_eq!(
            build_intent(&args(Verb::Capabilities, None)).unwrap(),
            Intent::Call("system.capabilities".to_string(), json!({}))
        );
        assert_eq!(
            build_intent(&args(Verb::Describe, None)).unwrap(),
            Intent::Call("ipc.describe".to_string(), json!({}))
        );
        assert_eq!(
            build_intent(&args(Verb::State, None)).unwrap(),
            Intent::Call("ui.getState".to_string(), json!({}))
        );
    }

    #[test]
    fn query_without_a_domain_is_a_usage_error() {
        assert!(build_intent(&args(Verb::Query, None)).is_err());
        assert!(build_intent(&args(Verb::Query, Some("   "))).is_err());
    }

    #[test]
    fn query_with_a_domain_resolves_through_the_sugar_table() {
        assert_eq!(
            build_intent(&args(Verb::Query, Some("record"))).unwrap(),
            Intent::Call("record.snapshot".to_string(), json!({}))
        );
    }

    #[test]
    fn command_without_a_name_is_a_usage_error() {
        assert!(build_intent(&args(Verb::Command, None)).is_err());
    }

    #[test]
    fn command_carries_its_parsed_params() {
        let mut a = args(Verb::Command, Some("record.pause"));
        a.params = Some(r#"{"marker": true}"#.to_string());
        assert_eq!(
            build_intent(&a).unwrap(),
            Intent::Call("record.pause".to_string(), json!({ "marker": true }))
        );
    }

    #[test]
    fn command_with_no_params_sends_an_empty_object() {
        let a = args(Verb::Command, Some("record.pause"));
        assert_eq!(
            build_intent(&a).unwrap(),
            Intent::Call("record.pause".to_string(), json!({}))
        );
    }

    #[test]
    fn params_that_are_not_a_json_object_are_a_usage_error() {
        let mut a = args(Verb::Command, Some("record.pause"));
        a.params = Some("[1, 2]".to_string());
        assert!(build_intent(&a).is_err());
        a.params = Some("{not json".to_string());
        assert!(build_intent(&a).is_err());
    }

    #[test]
    fn wait_without_an_event_name_is_a_usage_error() {
        assert!(build_intent(&args(Verb::Wait, None)).is_err());
    }

    #[test]
    fn wait_builds_its_filter_from_where_pairs() {
        let mut a = args(Verb::Wait, Some("record.stateChanged"));
        a.filters = vec!["stateText=Paused".to_string()];
        assert_eq!(
            build_intent(&a).unwrap(),
            Intent::Wait(
                "record.stateChanged".to_string(),
                json!({ "stateText": "Paused" })
            )
        );
    }

    #[test]
    fn a_where_entry_without_an_equals_sign_is_a_usage_error() {
        let mut a = args(Verb::Wait, Some("record.stateChanged"));
        a.filters = vec!["stateText".to_string()];
        assert!(build_intent(&a).is_err());
    }

    #[test]
    fn exit_codes_match_the_documented_contract() {
        assert_eq!(exit_code(Outcome::Success), ExitCode::from(0));
        assert_eq!(exit_code(Outcome::Usage), ExitCode::from(2));
        assert_eq!(exit_code(Outcome::ConnectFailed), ExitCode::from(3));
        assert_eq!(exit_code(Outcome::Refused), ExitCode::from(4));
        assert_eq!(exit_code(Outcome::TimedOut), ExitCode::from(5));
    }

    #[test]
    fn a_request_timeout_is_told_apart_from_a_lost_channel() {
        let timeout = anyhow::anyhow!("no response to record.pause within 30 s");
        assert!(is_response_timeout("record.pause", &timeout));

        let lost = anyhow::anyhow!("control pipe closed: end of stream");
        assert!(!is_response_timeout("record.pause", &lost));

        // A different command's timeout must not be mistaken for this one's.
        let other = anyhow::anyhow!("no response to record.stop within 30 s");
        assert!(!is_response_timeout("record.pause", &other));
    }

    #[test]
    fn refusal_value_carries_every_protocol_field() {
        let refusal = Refusal {
            code: "invalid_state".to_string(),
            message: "record.start is not available".to_string(),
            requires: json!(true),
            actual: json!(false),
        };
        assert_eq!(
            refusal_value(&refusal),
            json!({
                "code": "invalid_state",
                "message": "record.start is not available",
                "requires": true,
                "actual": false,
            })
        );
    }
}
