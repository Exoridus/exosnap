//! Scenario definitions and the verdict discipline every scenario follows.
//!
//! A scenario body returns `Step<T>`. Harness and oracle problems propagate
//! through `?` as `Stop::Infra` and become INFRA_ERROR. Only an explicit
//! product check (`product_ensure!`, `Stop::fail`) can produce FAIL, and only
//! an explicit capability check can produce UNAVAILABLE. A body that returns
//! `Ok` has demonstrated its contract and is a PASS.

use serde_json::Value;
use std::collections::BTreeMap;
use std::fmt;
use std::time::Duration;

use crate::capability::Capability;
use crate::context::Context;
use crate::plan::Tier;

#[derive(Debug)]
pub enum Stop {
    /// The product was exercised and violated its contract.
    Fail(String),
    /// A required capability or precondition is absent on this machine.
    Unavailable(String),
    /// The harness, environment or oracle failed; the product was not judged.
    Infra(anyhow::Error),
}

impl Stop {
    pub fn fail(message: impl Into<String>) -> Self {
        Stop::Fail(message.into())
    }
    pub fn unavailable(message: impl Into<String>) -> Self {
        Stop::Unavailable(message.into())
    }
    pub fn infra(message: impl fmt::Display) -> Self {
        Stop::Infra(anyhow::anyhow!("{message}"))
    }
}

impl<E: Into<anyhow::Error>> From<E> for Stop {
    fn from(error: E) -> Self {
        Stop::Infra(error.into())
    }
}

pub type Step<T = ()> = Result<T, Stop>;

/// Fails the scenario as a product defect when `cond` is false.
#[macro_export]
macro_rules! product_ensure {
    ($cond:expr, $($arg:tt)+) => {
        if !$cond {
            return Err($crate::scenario::Stop::Fail(format!($($arg)+)));
        }
    };
}

/// Stops the scenario as INFRA_ERROR when `cond` is false: a precondition or
/// ground truth the verdict depends on could not be established.
#[macro_export]
macro_rules! infra_ensure {
    ($cond:expr, $($arg:tt)+) => {
        if !$cond {
            return Err($crate::scenario::Stop::Infra(anyhow::anyhow!($($arg)+)));
        }
    };
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Lane {
    /// Local, no bundle: seconds-scale inner-loop checks.
    Quick,
    /// Local, no bundle: short hardware sanity before pushing.
    Preflight,
    CiCore,
    CiInstall,
    CiUpdate,
    Gpu,
    Hardware,
    Nightly,
}

impl Lane {
    pub const ALL: [Lane; 8] = [
        Lane::Quick,
        Lane::Preflight,
        Lane::CiCore,
        Lane::CiInstall,
        Lane::CiUpdate,
        Lane::Gpu,
        Lane::Hardware,
        Lane::Nightly,
    ];

    pub fn name(self) -> &'static str {
        match self {
            Lane::Quick => "quick",
            Lane::Preflight => "preflight",
            Lane::CiCore => "release-ci-core",
            Lane::CiInstall => "release-ci-install",
            Lane::CiUpdate => "release-ci-update",
            Lane::Gpu => "release-gpu",
            Lane::Hardware => "release-hardware",
            Lane::Nightly => "nightly",
        }
    }

    pub fn parse(name: &str) -> Option<Lane> {
        Lane::ALL.into_iter().find(|l| l.name() == name)
    }

    /// Release lanes judge a CI-built bundle; development lanes judge local bytes.
    pub fn is_release(self) -> bool {
        matches!(
            self,
            Lane::CiCore | Lane::CiInstall | Lane::CiUpdate | Lane::Gpu | Lane::Hardware
        )
    }
}

pub struct Scenario {
    pub id: &'static str,
    /// Bumped when the contract, oracle or stimulus changes materially.
    pub revision: u32,
    pub title: &'static str,
    /// The product claim, in one sentence.
    pub claim: &'static str,
    pub lane: Lane,
    /// Development lanes that also run this scenario against local bytes.
    pub also: &'static [Lane],
    pub tier: Tier,
    pub requires: &'static [Capability],
    pub timeout: Duration,
    pub run: fn(&mut Context) -> Step,
}

impl Scenario {
    pub fn runs_in(&self, lane: Lane) -> bool {
        self.lane == lane || self.also.contains(&lane)
    }
}

/// Measured facts attached to a result. Keys are stable; values are JSON.
#[derive(Debug, Default, Clone)]
pub struct Evidence(pub BTreeMap<String, Value>);

impl Evidence {
    pub fn put(&mut self, key: &str, value: impl Into<Value>) {
        self.0.insert(key.to_string(), value.into());
    }
}
