//! Hyper-V socket (`AF_HYPERV`) streams.
//!
//! A guest listener is reachable only from its parent partition, and the host
//! can only connect to a service id that is registered under
//! `HKLM\...\Virtualization\GuestCommunicationServices`, which needs an
//! administrator once. That reachability is the channel's authentication.

use anyhow::{Context, Result, bail};
use std::io::{Read, Write};
use std::sync::Once;
use std::time::Duration;
use windows::Win32::Networking::WinSock::{
    ADDRESS_FAMILY, AF_HYPERV, INVALID_SOCKET, SEND_RECV_FLAGS, SO_RCVTIMEO, SO_SNDTIMEO,
    SOCK_STREAM, SOCKADDR, SOCKET, SOCKET_ERROR, SOL_SOCKET, WSADATA, WSAGetLastError, WSAStartup,
    accept, bind, closesocket, connect, listen, recv, send, setsockopt, socket,
};
use windows::Win32::System::Hypervisor::{HV_PROTOCOL_RAW, SOCKADDR_HV};
use windows::core::GUID;

/// `HVSOCKET_CONNECT_TIMEOUT` from hvsocket.h: milliseconds a connect waits.
const HVSOCKET_CONNECT_TIMEOUT: i32 = 1;
/// Any partition. Inside a guest only the parent can actually connect.
const HV_GUID_WILDCARD: GUID = GUID::zeroed();

fn startup() {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| unsafe {
        let mut data = WSADATA::default();
        let _ = WSAStartup(0x0202, &mut data);
    });
}

pub fn parse_guid(text: &str) -> Result<GUID> {
    let bare = text.trim_matches(|c| c == '{' || c == '}');
    let groups: Vec<usize> = bare.split('-').map(str::len).collect();
    let hex: String = bare.chars().filter(|c| *c != '-').collect();
    if groups != [8, 4, 4, 4, 12] {
        bail!("'{text}' is not a GUID");
    }
    let value =
        u128::from_str_radix(&hex, 16).map_err(|_| anyhow::anyhow!("'{text}' is not a GUID"))?;
    Ok(GUID::from_u128(value))
}

fn address(vm_id: GUID, service_id: GUID) -> SOCKADDR_HV {
    SOCKADDR_HV {
        Family: ADDRESS_FAMILY(AF_HYPERV),
        Reserved: 0,
        VmId: vm_id,
        ServiceId: service_id,
    }
}

fn new_socket() -> Result<SOCKET> {
    startup();
    let s = unsafe { socket(AF_HYPERV as i32, SOCK_STREAM, HV_PROTOCOL_RAW as i32) }
        .context("create Hyper-V socket")?;
    if s == INVALID_SOCKET {
        bail!("create Hyper-V socket: {:?}", unsafe { WSAGetLastError() });
    }
    Ok(s)
}

fn set_u32(s: SOCKET, level: i32, name: i32, value: u32) -> Result<()> {
    let rc = unsafe { setsockopt(s, level, name, Some(&value.to_le_bytes())) };
    if rc == SOCKET_ERROR {
        bail!("setsockopt {level}/{name}: {:?}", unsafe {
            WSAGetLastError()
        });
    }
    Ok(())
}

pub struct HvStream(SOCKET);

impl HvStream {
    /// Connects to `service_id` inside the VM `vm_id` (the VM's Hyper-V id).
    pub fn connect(vm_id: GUID, service_id: GUID, timeout: Duration) -> Result<HvStream> {
        let s = new_socket()?;
        let stream = HvStream(s);
        set_u32(
            s,
            HV_PROTOCOL_RAW as i32,
            HVSOCKET_CONNECT_TIMEOUT,
            timeout.as_millis().min(u32::MAX as u128) as u32,
        )?;
        let addr = address(vm_id, service_id);
        let rc = unsafe {
            connect(
                s,
                &addr as *const SOCKADDR_HV as *const SOCKADDR,
                size_of::<SOCKADDR_HV>() as i32,
            )
        };
        if rc == SOCKET_ERROR {
            bail!("connect to guest agent: {:?}", unsafe { WSAGetLastError() });
        }
        Ok(stream)
    }

    /// Bounds every blocking read and write, so a vanished peer surfaces as
    /// an error instead of a hang.
    pub fn set_timeout(&self, timeout: Duration) -> Result<()> {
        let ms = timeout.as_millis().clamp(1, u32::MAX as u128) as u32;
        set_u32(self.0, SOL_SOCKET, SO_RCVTIMEO, ms)?;
        set_u32(self.0, SOL_SOCKET, SO_SNDTIMEO, ms)
    }
}

impl Drop for HvStream {
    fn drop(&mut self) {
        unsafe {
            closesocket(self.0);
        }
    }
}

impl Read for HvStream {
    fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
        let n = unsafe { recv(self.0, buf, SEND_RECV_FLAGS(0)) };
        if n == SOCKET_ERROR {
            return Err(std::io::Error::other(format!("recv: {:?}", unsafe {
                WSAGetLastError()
            })));
        }
        Ok(n as usize)
    }
}

impl Write for HvStream {
    fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
        let n = unsafe { send(self.0, buf, SEND_RECV_FLAGS(0)) };
        if n == SOCKET_ERROR {
            return Err(std::io::Error::other(format!("send: {:?}", unsafe {
                WSAGetLastError()
            })));
        }
        Ok(n as usize)
    }
    fn flush(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}

pub struct HvListener(SOCKET);

impl HvListener {
    pub fn bind(service_id: GUID) -> Result<HvListener> {
        let s = new_socket()?;
        let listener = HvListener(s);
        let addr = address(HV_GUID_WILDCARD, service_id);
        let rc = unsafe {
            bind(
                s,
                &addr as *const SOCKADDR_HV as *const SOCKADDR,
                size_of::<SOCKADDR_HV>() as i32,
            )
        };
        if rc == SOCKET_ERROR {
            bail!("bind guest agent service: {:?}", unsafe {
                WSAGetLastError()
            });
        }
        if unsafe { listen(s, 4) } == SOCKET_ERROR {
            bail!("listen: {:?}", unsafe { WSAGetLastError() });
        }
        Ok(listener)
    }

    pub fn accept(&self) -> Result<HvStream> {
        let s = unsafe { accept(self.0, None, None) }.context("accept")?;
        Ok(HvStream(s))
    }
}

impl Drop for HvListener {
    fn drop(&mut self) {
        unsafe {
            closesocket(self.0);
        }
    }
}
