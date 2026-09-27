//! Messages exchanged between `exo-verify` on the host and `exo-guest` in the VM.
//!
//! Every request carries a caller-chosen id that its response echoes. A
//! response is either a JSON result or an error string; the agent never
//! reports a failed operation as a successful one with an error inside.

use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

/// Bumped on any incompatible message change. Both ends refuse a mismatch
/// during `hello` instead of guessing at the other side's shape.
pub const PROTOCOL_VERSION: u32 = 1;

/// The Hyper-V socket service the agent listens on. The host registers the
/// same id under `GuestCommunicationServices` before it can connect.
pub const SERVICE_ID: &str = "5e8f0a5c-7f1d-4c52-9b7e-3e0c5a9d2f41";

/// Upper bound for one frame, so a corrupt length prefix cannot allocate
/// unbounded memory on either side.
pub const MAX_FRAME_BYTES: usize = 64 * 1024 * 1024;

/// Upper bound for captured stdout or stderr of one `exec`.
pub const MAX_CAPTURE_BYTES: usize = 4 * 1024 * 1024;

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Envelope {
    pub id: u64,
    pub request: Request,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(tag = "op", rename_all = "kebab-case")]
pub enum Request {
    /// Must be the first request. `nonce` is echoed so the host can tell this
    /// session's agent from a stale one.
    Hello {
        protocol: u32,
        nonce: String,
    },
    /// Facts about the guest the host needs to select work.
    Capabilities,
    /// Runs a process to completion within `timeoutMs` and returns its
    /// bounded output. A timeout kills the process and is an error.
    #[serde(rename_all = "camelCase")]
    Exec {
        program: String,
        #[serde(default)]
        args: Vec<String>,
        #[serde(default)]
        cwd: Option<String>,
        #[serde(default)]
        env: BTreeMap<String, String>,
        timeout_ms: u64,
    },
    /// Starts a process and returns a handle for `wait` and `kill`.
    #[serde(rename_all = "camelCase")]
    Spawn {
        program: String,
        #[serde(default)]
        args: Vec<String>,
        #[serde(default)]
        cwd: Option<String>,
        #[serde(default)]
        env: BTreeMap<String, String>,
    },
    /// Waits up to `timeoutMs` for a spawned process. Not having exited yet
    /// is a result, not an error.
    #[serde(rename_all = "camelCase")]
    Wait {
        handle: u64,
        timeout_ms: u64,
    },
    Kill {
        handle: u64,
    },
    /// Reads at most `maxLen` bytes starting at `offset`.
    #[serde(rename_all = "camelCase")]
    ReadFile {
        path: String,
        offset: u64,
        max_len: u64,
    },
    /// Writes base64 `data`, creating parent directories.
    WriteFile {
        path: String,
        data: String,
        append: bool,
    },
    Exists {
        path: String,
    },
    /// Lists one directory level.
    ListDir {
        path: String,
    },
    /// Which session and user the agent runs in, and whether its desktop is
    /// the interactive input desktop.
    SessionInfo,
    /// Whether the guest is ready to run a campaign: an interactive user is
    /// logged on and the agent runs on that user's input desktop.
    GateReady,
    /// Restarts the guest. The connection drops; the host reconnects.
    Reboot,
    Shutdown,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Response {
    pub id: u64,
    pub ok: bool,
    #[serde(default)]
    pub result: serde_json::Value,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub error: Option<String>,
}

impl Response {
    pub fn ok(id: u64, result: serde_json::Value) -> Self {
        Response {
            id,
            ok: true,
            result,
            error: None,
        }
    }

    pub fn err(id: u64, error: impl Into<String>) -> Self {
        Response {
            id,
            ok: false,
            result: serde_json::Value::Null,
            error: Some(error.into()),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ExecResult {
    pub exit_code: Option<i32>,
    pub stdout: String,
    pub stderr: String,
    pub truncated: bool,
    pub elapsed_ms: u64,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DirEntry {
    pub name: String,
    pub is_dir: bool,
    pub len: u64,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn requests_are_tagged_by_operation() {
        let json = serde_json::to_value(Envelope {
            id: 7,
            request: Request::Wait {
                handle: 3,
                timeout_ms: 500,
            },
        })
        .unwrap();
        assert_eq!(
            json,
            serde_json::json!({"id": 7, "request": {"op": "wait", "handle": 3, "timeoutMs": 500}})
        );
    }

    #[test]
    fn an_unknown_operation_is_rejected() {
        assert!(serde_json::from_str::<Request>(r#"{"op":"format-disk"}"#).is_err());
    }

    #[test]
    fn service_id_is_a_guid() {
        let parts: Vec<_> = SERVICE_ID.split('-').map(str::len).collect();
        assert_eq!(parts, [8, 4, 4, 4, 12]);
    }
}
