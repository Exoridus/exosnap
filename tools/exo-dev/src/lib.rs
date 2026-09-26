//! ExoSnap repository verification: which checks run for a change, in what order,
//! what each depends on, and what a run may claim afterwards.

pub mod evidence;
pub mod executor;
pub mod git;
pub mod hook;
pub mod host_lock;
pub mod msvc;
pub mod plan;
pub mod process;
pub mod profile;
pub mod report;
pub mod run;
pub mod scope;
pub mod step;
