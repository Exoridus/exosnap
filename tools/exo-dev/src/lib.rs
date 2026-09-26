//! ExoSnap repository verification: which checks run for a change, in what order,
//! what each depends on, and what a run may claim afterwards.

pub mod build_artifacts;
pub mod cli_flags;
pub mod commit_policy;
pub mod drift;
pub mod evidence;
pub mod executor;
pub mod git;
pub mod hook;
pub mod host_lock;
pub mod lint;
pub mod msvc;
pub mod plan;
pub mod pr;
pub mod privacy;
pub mod process;
pub mod profile;
pub mod report;
pub mod rulesets;
pub mod run;
pub mod scope;
pub mod source_hygiene;
pub mod step;
pub mod test;

/// Behind a Cargo feature, never a plain `cfg(test)`, so a file under `tests/`
/// (a separate crate) can see it too. See `test_support`'s module doc.
#[cfg(any(test, feature = "test-support"))]
pub mod test_support;
