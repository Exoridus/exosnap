//! Automated guards for the privacy promises in `PRIVACY.md` and
//! `docs/product-spec.md`: no undocumented network egress point lands
//! unnoticed, and the crash-report tag allowlist never drifts from what those
//! documents describe as sent.

pub mod allowlist;
pub mod network_egress;
