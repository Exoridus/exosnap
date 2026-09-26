//! Release preparation: moving the product version across every packaging
//! surface that repeats it, checking that they still agree, assembling the
//! changelog, rendering release notes, and reading packaging publication drift.
//!
//! Publishing itself never lives here. Every function in this module reports
//! or writes local files; the decision to submit a package or push a tag is
//! made by a person, following docs/release-checklist.md.

pub mod changelog;
pub mod feed_drift;
pub mod version;
