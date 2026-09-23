//! The scenario registry, organised by product capability.

pub mod app;
pub mod common;
pub mod dist;

use crate::scenario::Scenario;

pub fn registry() -> Vec<Scenario> {
    let mut all = Vec::new();
    all.extend(dist::scenarios());
    all.extend(app::scenarios());
    all
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashSet;

    #[test]
    fn ids_are_unique_and_well_formed() {
        let mut seen = HashSet::new();
        for s in registry() {
            assert!(seen.insert(s.id), "duplicate scenario id {}", s.id);
            assert!(
                s.id.contains('.')
                    && s.id.chars().all(|c| c.is_ascii_lowercase()
                        || c.is_ascii_digit()
                        || c == '.'
                        || c == '-'),
                "{}",
                s.id
            );
            assert!(s.revision >= 1);
            assert!(!s.claim.is_empty() && !s.title.is_empty());
            assert!(!s.also.contains(&s.lane));
        }
    }

    #[test]
    fn development_lanes_never_own_release_evidence() {
        for s in registry() {
            if matches!(
                s.lane,
                crate::scenario::Lane::Quick | crate::scenario::Lane::Preflight
            ) {
                assert_eq!(
                    s.tier,
                    crate::plan::Tier::Recommended,
                    "{} is a development check",
                    s.id
                );
            }
        }
    }
}
