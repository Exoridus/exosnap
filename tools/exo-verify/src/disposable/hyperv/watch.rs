//! Bounded read-only observation over the native guest transport.

use anyhow::{Context, Result, bail};
use base64::Engine as _;
use exo_guest::protocol::Request;
use std::io::Write;
use std::process::ExitCode;
use std::time::{Duration, Instant};

use super::GuestClient;

#[derive(Debug, clap::Args)]
pub struct Args {
    #[arg(long)]
    pub vm_id: String,
    #[arg(long)]
    pub exit_file: String,
    #[arg(long)]
    pub log_file: Option<String>,
    #[arg(long, default_value_t = 5400, value_parser = clap::value_parser!(u64).range(1..))]
    pub timeout_seconds: u64,
    #[arg(long, default_value_t = 5, value_parser = clap::value_parser!(u64).range(1..=600))]
    pub poll_seconds: u64,
    #[arg(long, default_value_t = 4, value_parser = clap::value_parser!(u32).range(1..=100))]
    pub max_consecutive_failures: u32,
}

#[derive(Default)]
struct Cursor {
    offset: u64,
    failures: u32,
}

impl Cursor {
    fn observed(&mut self, bytes: usize) {
        self.offset += bytes as u64;
    }
    fn succeeded(&mut self) {
        self.failures = 0;
    }
    fn failed(&mut self, limit: u32) -> bool {
        self.failures += 1;
        self.failures >= limit
    }
}

fn read(client: &mut GuestClient, path: &str, offset: u64) -> Result<Vec<u8>> {
    let reply = client.call(Request::ReadFile {
        path: path.into(),
        offset,
        max_len: 64 * 1024,
    })?;
    base64::engine::general_purpose::STANDARD
        .decode(
            reply["data"]
                .as_str()
                .context("guest file response lacks data")?,
        )
        .context("guest file data is invalid base64")
}

pub fn run(args: Args) -> Result<ExitCode> {
    exo_guest::hvsock::parse_guid(&args.vm_id)?;
    let deadline = Instant::now() + Duration::from_secs(args.timeout_seconds);
    let mut cursor = Cursor::default();
    let mut client = None;
    loop {
        if Instant::now() >= deadline {
            eprintln!("watch deadline expired");
            return Ok(ExitCode::from(2));
        }
        let observed = (|| -> Result<Option<Vec<u8>>> {
            if client.is_none() {
                client = Some(GuestClient::connect(
                    &args.vm_id,
                    deadline.min(Instant::now() + Duration::from_secs(15)),
                )?);
            }
            let client = client.as_mut().context("watch connection absent")?;
            client.deadline = Some(deadline.min(Instant::now() + Duration::from_secs(30)));
            if let Some(log) = &args.log_file {
                let exists = client.call(Request::Exists { path: log.clone() })?;
                if exists["exists"].as_bool() == Some(true) {
                    let len = exists["len"]
                        .as_u64()
                        .context("log length was not reported")?;
                    if len < cursor.offset {
                        bail!("guest log was truncated during observation");
                    }
                    while cursor.offset < len {
                        let bytes = read(client, log, cursor.offset)?;
                        if bytes.is_empty() {
                            bail!("guest log read made no progress");
                        }
                        std::io::stdout().write_all(&bytes)?;
                        std::io::stdout().flush()?;
                        cursor.observed(bytes.len());
                    }
                }
            }
            let exists = client.call(Request::Exists {
                path: args.exit_file.clone(),
            })?;
            if exists["exists"].as_bool() == Some(true) {
                return Ok(Some(read(client, &args.exit_file, 0)?));
            }
            Ok(None)
        })();
        match observed {
            Ok(Some(exit)) => {
                println!(
                    "campaign finished; exit file: {}",
                    String::from_utf8_lossy(&exit).trim()
                );
                return Ok(ExitCode::SUCCESS);
            }
            Ok(None) => cursor.succeeded(),
            Err(error) => {
                client = None;
                if Instant::now() >= deadline {
                    eprintln!("watch deadline expired: {error:#}");
                    return Ok(ExitCode::from(2));
                }
                if cursor.failed(args.max_consecutive_failures) {
                    eprintln!("guest stopped answering: {error:#}");
                    return Ok(ExitCode::from(3));
                }
            }
        }
        std::thread::sleep(
            Duration::from_secs(args.poll_seconds)
                .min(deadline.saturating_duration_since(Instant::now())),
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn byte_offsets_preserve_order_and_success_resets_failure_budget() {
        let mut cursor = Cursor::default();
        cursor.observed(7);
        cursor.observed(9);
        assert_eq!(cursor.offset, 16);
        assert!(!cursor.failed(2));
        cursor.succeeded();
        assert!(!cursor.failed(2));
        assert!(cursor.failed(2));
        assert_eq!(cursor.offset, 16);
    }

    #[test]
    fn partial_log_progress_does_not_hide_repeated_failed_exit_reads() {
        let mut cursor = Cursor::default();
        assert!(!cursor.failed(2));
        cursor.observed(7);
        assert!(cursor.failed(2));
        assert_eq!(cursor.offset, 7);
    }
}
