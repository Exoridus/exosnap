//! Read-only environment classification and alias checks against envctl's own catalogue.

use serde_json::Value;
use std::process::Command;

use super::common::secs;
use crate::capability::Capability;
use crate::context::Context;
use crate::plan::Tier;
use crate::scenario::{Lane, Scenario, Step, Stop};
use crate::{infra_ensure, product_ensure};

pub fn scenarios() -> Vec<Scenario> {
    vec![
        Scenario {
            id: "environment.classification",
            revision: 1,
            title: "Environment properties have explicit capability classes and mechanisms",
            claim: "the complete environment catalogue classifies every property and names its read mechanism and any supported mutation mechanism",
            lane: Lane::CiCore,
            also: &[],
            tier: Tier::Required,
            requires: &[Capability::Windows],
            timeout: secs(30.0),
            run: classification,
        },
        Scenario {
            id: "environment.aliases",
            revision: 1,
            title: "Configured device aliases resolve without ambiguity",
            claim: "the configured alias profile contains at least one binding and none resolves ambiguously",
            lane: Lane::Hardware,
            also: &[],
            tier: Tier::Required,
            requires: &[Capability::Windows],
            timeout: secs(30.0),
            run: aliases,
        },
    ]
}

fn envctl(command: &str) -> Step<Value> {
    let exe = crate::tools::resolve("exosnap-envctl")
        .ok_or_else(|| Stop::unavailable("exosnap-envctl is not available"))?;
    let output = crate::tools::run(Command::new(exe).arg(command), secs(20.0))?;
    let value: Value = serde_json::from_str(&output.stdout).map_err(|error| {
        Stop::infra(format!(
            "exosnap-envctl {command} emitted invalid JSON: {error}"
        ))
    })?;
    infra_ensure!(
        value["command"] == command,
        "exosnap-envctl {command} answered for another command: {}",
        value["command"]
    );
    if command == "describe" {
        infra_ensure!(
            output.success() && value["ok"] == true,
            "exosnap-envctl describe failed: {}",
            output.stderr
        );
    }
    Ok(value)
}

fn classification(ctx: &mut Context) -> Step {
    let document = envctl("describe")?;
    ctx.evidence.put("capabilities", document.clone());
    let (classified, mutable) = judge_classification(&document)?;
    ctx.evidence.put("classifiedProperties", classified);
    ctx.evidence.put("safelyMutableProperties", mutable);
    Ok(())
}

fn judge_classification(document: &Value) -> Step<(usize, usize)> {
    let catalogue = document["catalogue"]
        .as_array()
        .ok_or_else(|| Stop::infra("envctl describe has no catalogue array"))?;
    product_ensure!(
        !catalogue.is_empty(),
        "the environment capability catalogue is empty"
    );
    let mut mutable = 0;
    for (index, entry) in catalogue.iter().enumerate() {
        let class = entry["capability"].as_str().unwrap_or("");
        product_ensure!(
            matches!(
                class,
                "ENV_READ"
                    | "ENV_MUTATE_SAFE"
                    | "ENV_MUTATE_TESTONLY"
                    | "ENV_HUMAN"
                    | "PHYSICAL"
                    | "SECURE"
                    | "UNAVAILABLE"
            ),
            "catalogue entry {index} has an unknown capability class '{class}'"
        );
        product_ensure!(
            entry["readMechanism"]
                .as_str()
                .is_some_and(|s| !s.trim().is_empty()),
            "catalogue entry {index} names no read mechanism"
        );
        if matches!(class, "ENV_MUTATE_SAFE" | "ENV_MUTATE_TESTONLY") {
            product_ensure!(
                entry["mutateMechanism"]
                    .as_str()
                    .is_some_and(|s| !s.trim().is_empty()),
                "mutable catalogue entry {index} names no mutation mechanism"
            );
        }
        mutable += usize::from(class == "ENV_MUTATE_SAFE");
    }
    Ok((catalogue.len(), mutable))
}

fn aliases(ctx: &mut Context) -> Step {
    let document = envctl("resolve-aliases")?;
    ctx.evidence.put("aliases", document.clone());
    let (bound, unavailable) = judge_aliases(&document)?;
    ctx.evidence.put("boundAliases", bound);
    ctx.evidence.put("unavailableAliases", unavailable);
    Ok(())
}

fn judge_aliases(document: &Value) -> Step<(usize, usize)> {
    let bindings = document["bindings"]
        .as_array()
        .ok_or_else(|| Stop::infra("envctl resolve-aliases has no bindings array"))?;
    let errors = document["errors"]
        .as_array()
        .ok_or_else(|| Stop::infra("envctl resolve-aliases has no errors array"))?;
    let ambiguous = bindings
        .iter()
        .chain(errors)
        .filter(|entry| {
            entry["status"] == "ambiguous_device" || entry["code"] == "ambiguous_device"
        })
        .count();
    product_ensure!(ambiguous == 0, "{ambiguous} device aliases are ambiguous");
    if bindings.is_empty() && errors.is_empty() {
        return Err(Stop::unavailable("no device alias profile is configured"));
    }
    let bound = bindings
        .iter()
        .filter(|entry| {
            matches!(
                entry["status"].as_str(),
                Some("ok" | "friendly_name_changed")
            )
        })
        .count();
    let missing = bindings
        .iter()
        .filter(|entry| entry["status"] == "device_not_present")
        .count()
        + errors.len();
    infra_ensure!(
        bound + missing == bindings.len() + errors.len(),
        "envctl resolve-aliases reported an unrecognized binding status"
    );
    Ok((bound, missing))
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn classification_rejects_empty_catalogue_and_missing_mutation_mechanism() {
        assert!(matches!(
            judge_classification(&json!({"catalogue": []})),
            Err(Stop::Fail(_))
        ));
        let invalid = json!({"catalogue": [{"capability": "ENV_MUTATE_SAFE", "readMechanism": "read", "mutateMechanism": ""}]});
        assert!(matches!(judge_classification(&invalid), Err(Stop::Fail(_))));
    }

    #[test]
    fn classification_accepts_read_and_mutable_entries_with_mechanisms() {
        let valid = json!({"catalogue": [
            {"capability": "ENV_READ", "readMechanism": "query"},
            {"capability": "ENV_MUTATE_SAFE", "readMechanism": "query", "mutateMechanism": "set"}
        ]});
        assert_eq!(judge_classification(&valid).unwrap(), (2, 1));
    }

    #[test]
    fn aliases_reject_ambiguity_and_empty_profiles() {
        let ambiguous = json!({"bindings": [{"alias": "display.main", "status": "ambiguous_device"}], "errors": [], "candidates": []});
        assert!(matches!(judge_aliases(&ambiguous), Err(Stop::Fail(_))));
        let empty = json!({"bindings": [], "errors": [], "candidates": [{}]});
        assert!(matches!(judge_aliases(&empty), Err(Stop::Unavailable(_))));
    }

    #[test]
    fn aliases_count_bound_and_missing_devices_without_inventing_success() {
        let report = json!({"bindings": [
            {"alias": "a", "status": "ok"},
            {"alias": "b", "status": "friendly_name_changed"},
            {"alias": "c", "status": "device_not_present"}
        ], "errors": [], "candidates": []});
        assert_eq!(judge_aliases(&report).unwrap(), (2, 1));
    }
}
