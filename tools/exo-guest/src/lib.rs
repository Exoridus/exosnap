//! Host-to-guest control channel for disposable Hyper-V verification VMs.
//!
//! The channel is deliberately small: it starts, observes and stops processes
//! and moves files. It is not remote administration software. The transport is
//! a Hyper-V socket, which only the parent partition can reach and which never
//! touches a network stack.

pub mod agent;
pub mod frame;
#[cfg(windows)]
pub mod hvsock;
pub mod protocol;

pub use protocol::{PROTOCOL_VERSION, Request, Response, SERVICE_ID};
