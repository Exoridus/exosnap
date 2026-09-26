//! The frontend recording benchmark: physical display topology checks, one
//! measured run, an alternating campaign of runs, cross-run comparison, and
//! Superposition scene calibration.
//!
//! Business logic (topology assertion, acceptance criteria, comparability
//! rules, CSV statistics) is plain functions over plain data so it can be
//! tested without a display, a running Superposition process or a built
//! ExoSnap binary. Only the thin edges (Win32 enumeration, WMI, child
//! processes) touch the real machine.

pub mod campaign;
pub mod compare;
pub mod run;
pub mod scene_survey;
pub mod topology;

pub use run::Frontend;
