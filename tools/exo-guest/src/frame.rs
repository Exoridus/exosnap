//! Length-prefixed JSON framing: a little-endian `u32` byte count, then the
//! UTF-8 JSON body.

use anyhow::{Context, Result, bail};
use serde::Serialize;
use serde::de::DeserializeOwned;
use std::io::{Read, Write};

use crate::protocol::MAX_FRAME_BYTES;

pub fn write<T: Serialize>(out: &mut impl Write, value: &T) -> Result<()> {
    let body = serde_json::to_vec(value)?;
    if body.len() > MAX_FRAME_BYTES {
        bail!("frame of {} bytes exceeds the protocol limit", body.len());
    }
    out.write_all(&(body.len() as u32).to_le_bytes())?;
    out.write_all(&body)?;
    out.flush()?;
    Ok(())
}

/// Reads one frame. `Ok(None)` means the peer closed the connection cleanly
/// between frames; a close inside a frame is an error.
pub fn read<T: DeserializeOwned>(input: &mut impl Read) -> Result<Option<T>> {
    let mut prefix = [0u8; 4];
    let mut filled = 0;
    while filled < prefix.len() {
        let n = input.read(&mut prefix[filled..])?;
        if n == 0 {
            if filled == 0 {
                return Ok(None);
            }
            bail!("connection closed inside a frame header");
        }
        filled += n;
    }
    let len = u32::from_le_bytes(prefix) as usize;
    if len > MAX_FRAME_BYTES {
        bail!("peer announced a {len}-byte frame, above the protocol limit");
    }
    let mut body = vec![0u8; len];
    input
        .read_exact(&mut body)
        .context("connection closed inside a frame body")?;
    Ok(Some(
        serde_json::from_slice(&body).context("malformed frame")?,
    ))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::protocol::{Envelope, Request};

    #[test]
    fn frames_round_trip() {
        let mut buffer = Vec::new();
        let sent = Envelope {
            id: 1,
            request: Request::Capabilities,
        };
        write(&mut buffer, &sent).unwrap();
        write(&mut buffer, &sent).unwrap();
        let mut cursor = std::io::Cursor::new(buffer);
        assert_eq!(read::<Envelope>(&mut cursor).unwrap(), Some(sent.clone()));
        assert_eq!(read::<Envelope>(&mut cursor).unwrap(), Some(sent));
        assert_eq!(read::<Envelope>(&mut cursor).unwrap(), None);
    }

    #[test]
    fn an_oversized_length_is_refused_before_allocating() {
        let mut cursor = std::io::Cursor::new(u32::MAX.to_le_bytes().to_vec());
        assert!(read::<Envelope>(&mut cursor).is_err());
    }

    #[test]
    fn a_truncated_body_is_an_error() {
        let mut bytes = 10u32.to_le_bytes().to_vec();
        bytes.extend_from_slice(b"{}");
        assert!(read::<Envelope>(&mut std::io::Cursor::new(bytes)).is_err());
    }
}
