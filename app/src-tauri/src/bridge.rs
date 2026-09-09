//! The client half of the C0.1 pipe, in Rust.
//!
//! `python-proto/physics_sim/c01_client.py` is the reference, and this is a
//! deliberate translation of it rather than a fresh design. Everything here was
//! already paid for in C0.1 and C0.2:
//!
//!   - **Two half-duplex pipes, not one duplex pipe.** TX is AE writing and us
//!     reading; RX is us writing and AE reading. Opened TX first, because that
//!     is the order the AEGP creates them.
//!   - **One request per connection.** The Python client reconnects for every
//!     request and that is the shape C0.1 and C0.2 were measured on. A
//!     persistent connection may well work; it is not what was verified, and
//!     the saving is a `CreateFile` against a 16 ms round trip.
//!
//! THE RE-ARM RACE, WHICH COST AN AFTERNOON
//! ----------------------------------------
//! The server holds **one instance** of each pipe (`nMaxInstances = 1`), and
//! between clients it runs `DisconnectNamedPipe` + `CloseHandle` and then
//! `CreateNamedPipe` again. A `CreateFile` that lands inside that window
//! succeeds against the instance the server is about to tear down. The server
//! then logs `pipe: client gone`, and the reply -- which is produced later, on
//! the UI thread, by the idle hook -- is written to nobody:
//!
//!     pipe: client connected
//!     rx: cmd=ping ...
//!     pipe: client gone
//!     write: no client connected, 23 bytes dropped
//!
//! 23 bytes is exactly `{"ok":true,"pong":true}`. The request was received and
//! answered; the answer had nowhere to go.
//!
//! The first version of this file turned that into a five-second timeout whose
//! message blamed a modal dialog open in After Effects -- a confident, wrong
//! diagnosis of a condition that clears itself in milliseconds. So a closed
//! connection with no reply is now **retried**, and only a genuine silence is
//! reported as one. `c01_client.py` never hit this because a human runs it
//! once; an application pings on launch and then reads, back to back.
//!   - **Read until the newline, not until the first chunk.** A scene document
//!     is far larger than one pipe read. C0.2 exists because treating the first
//!     chunk as the whole answer is exactly how a payload-size question gets a
//!     false pass -- and at 32 MB the reply arrives in hundreds of pieces.
//!
//! The pipe names are `PHYSBRIDGE_PIPE_TX` / `_RX` in `PhysBridge.h`. They are
//! repeated here rather than generated, because a build step that parses a C
//! header to produce a Rust constant is more machinery than two strings are
//! worth -- but they are the one thing in this file that can silently drift, so
//! `physbridge_names_match` in the tests reads the header and compares.

use std::io::{Read, Write};
use std::sync::mpsc;
use std::time::{Duration, Instant};

pub const PIPE_TX: &str = r"\\.\pipe\aephys_bridge_tx"; // AE writes, we read
pub const PIPE_RX: &str = r"\\.\pipe\aephys_bridge_rx"; // we write, AE reads

/// How long to keep retrying the open. AE creates the pipes when the plug-in
/// loads and re-creates them after each client disconnects, so a failure here
/// is almost always "AE is not running" rather than a race -- but the re-create
/// window is real, and it is why this retries at all.
const CONNECT_TIMEOUT: Duration = Duration::from_secs(5);
const CONNECT_RETRY: Duration = Duration::from_millis(200);

#[derive(Debug)]
pub struct BridgeError {
    pub message: String,
    /// The exchange failed in a way that says nothing about the request, so
    /// running it again is not a guess. Exactly one condition sets this: the
    /// server closed the connection before answering. See the re-arm race.
    pub retryable: bool,
}

impl BridgeError {
    fn plain(message: impl Into<String>) -> Self {
        Self { message: message.into(), retryable: false }
    }
    fn retryable(message: impl Into<String>) -> Self {
        Self { message: message.into(), retryable: true }
    }
}

impl std::fmt::Display for BridgeError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}", self.message)
    }
}

impl From<BridgeError> for String {
    fn from(e: BridgeError) -> String {
        e.message
    }
}

fn not_running(detail: &str) -> BridgeError {
    // The failure the user will actually hit, so it gets the sentence rather
    // than an OS error code. "The system cannot find the file specified" is
    // true and useless: the file is a pipe that only exists while AE is up.
    BridgeError::plain(format!(
        "could not reach the bridge ({detail}).\n\nIs After Effects running \
         with PhysBridge.aex installed? Check Window > 'PhysBridge: bridge \
         status' -- it reports whether a client is connected and how many \
         requests it has served."
    ))
}

#[cfg(windows)]
fn connect() -> Result<(std::fs::File, std::fs::File), BridgeError> {
    use std::fs::OpenOptions;

    let deadline = Instant::now() + CONNECT_TIMEOUT;
    loop {
        // TX first: the order AE creates them, so a half-open state resolves
        // rather than deadlocking on the second open.
        let why = match OpenOptions::new().read(true).open(PIPE_TX) {
            Ok(tx) => match OpenOptions::new().write(true).open(PIPE_RX) {
                Ok(rx) => return Ok((tx, rx)),
                Err(e) => e.to_string(),
            },
            Err(e) => e.to_string(),
        };
        if Instant::now() >= deadline {
            return Err(not_running(&why));
        }
        std::thread::sleep(CONNECT_RETRY);
    }
}

#[cfg(not(windows))]
fn connect() -> Result<(std::fs::File, std::fs::File), BridgeError> {
    Err(BridgeError::plain(
        "the bridge speaks Windows named pipes, and this is not Windows. The \
         AEGP half of this product is Win/macOS, but the transport has only \
         ever been built and measured on Windows."
            .into(),
    ))
}

/// How many times to re-run the whole exchange when the connection closes
/// without an answer. See the re-arm race above: the window is one server loop
/// iteration wide, so a second attempt has always been enough -- three is for
/// the case where two clients are genuinely competing for the single instance.
const EXCHANGE_ATTEMPTS: u32 = 3;

/// Send one JSON request, return the one newline-terminated reply.
///
/// `timeout` covers the whole exchange. It is generous by default because the
/// slow request is not the transport: C0.2 measured the pipe at ~79 MB/s and
/// the real bake crossing it in ~15 ms, while `AEGP_ExecuteScript` running
/// B1's reader is the part that takes real time -- and it runs on AE's UI
/// thread, so it also waits for whatever modal the user happens to have open.
pub fn request(payload: &str, timeout: Duration) -> Result<String, BridgeError> {
    let (tx_rx, thread_payload) = (mpsc::channel(), payload.to_string());

    // The read is blocking, and a hung AE would otherwise hang the caller
    // forever. The thread is detached on timeout: it is parked in a read on a
    // pipe AE owns, and it goes away when AE answers or closes the handle.
    let (send, recv) = tx_rx;
    std::thread::spawn(move || {
        let mut last = Err(BridgeError::plain("not attempted"));
        for _ in 0..EXCHANGE_ATTEMPTS {
            last = exchange(&thread_payload);
            match &last {
                // The re-arm race, and only that: the server took the request
                // and closed before its reply. Anything else -- a refusal, a
                // dropped connection mid-reply -- is an answer and stands.
                Err(e) if e.retryable => continue,
                _ => break,
            }
        }
        let _ = send.send(last);
    });

    match recv.recv_timeout(timeout) {
        Ok(result) => result,
        Err(_) => Err(BridgeError::plain(format!(
            "the bridge took the request but sent nothing back within {} s.\
             \n\nThe script runs on AE's UI thread, so a modal dialog open in \
             After Effects holds the reply until it is dismissed. A long read \
             on a large comp can also simply take longer than this.",
            timeout.as_secs()
        ))),
    }
}

fn exchange(payload: &str) -> Result<String, BridgeError> {
    let (mut tx, mut rx) = connect()?;

    let mut line = payload.to_string();
    line.push('\n');
    rx.write_all(line.as_bytes())
        .and_then(|_| rx.flush())
        // A write that fails on a torn-down instance is the same race as a
        // silent close, and reconnecting is the same answer.
        .map_err(|e| BridgeError::retryable(format!("could not send the request: {e}")))?;

    // Read until the newline. See the note at the top of the file.
    let mut buf = vec![0u8; 65536];
    let mut acc: Vec<u8> = Vec::new();
    loop {
        match tx.read(&mut buf) {
            Ok(0) => break, // AE closed the pipe
            Ok(n) => {
                acc.extend_from_slice(&buf[..n]);
                if acc.contains(&b'\n') {
                    break;
                }
            }
            Err(e) => {
                // Mid-reply is NOT the race: bytes were flowing, and a retry
                // would re-run whatever the bridge already did.
                return Err(BridgeError::plain(format!(
                    "the connection dropped after {} bytes of the reply: {e}",
                    acc.len()
                )))
            }
        }
    }

    if acc.is_empty() {
        return Err(BridgeError::retryable(
            "the bridge closed the connection without answering",
        ));
    }

    let end = acc.iter().position(|b| *b == b'\n').unwrap_or(acc.len());
    String::from_utf8(acc[..end].to_vec())
        .map_err(|e| BridgeError::plain(format!("the reply is not valid UTF-8: {e}")))
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Ping the real bridge. **Requires After Effects to be running** with
    /// PhysBridge.aex loaded, which is why it is `#[ignore]`d: a test that
    /// fails when an application is closed is a broken test, not a finding.
    ///
    ///     cargo test -- --ignored --nocapture
    ///
    /// It is worth having anyway. Everything else in this file is checked by
    /// reading it, and the one thing that cannot be is whether these two
    /// handles, opened in this order, actually round-trip against the AEGP.
    #[test]
    #[ignore = "needs After Effects running with PhysBridge.aex"]
    fn pings_a_live_bridge() {
        let t0 = Instant::now();
        let reply = request(r#"{"cmd":"ping"}"#, Duration::from_secs(5))
            .unwrap_or_else(|e| panic!("{e}"));
        eprintln!("  {reply}   ({} ms round trip)", t0.elapsed().as_millis());
        assert_eq!(reply, r#"{"ok":true,"pong":true}"#);
    }

    /// The pipe names are duplicated from `PhysBridge.h`, and a rename there
    /// would leave this file compiling and failing to connect with a message
    /// blaming After Effects. So the duplication is checked.
    #[test]
    fn physbridge_names_match() {
        let header = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../PhysBridge.h");
        let Ok(src) = std::fs::read_to_string(&header) else {
            // The app can be checked out without the SDK tree around it.
            eprintln!("skipping: {} not present", header.display());
            return;
        };
        for (define, ours) in [
            ("PHYSBRIDGE_PIPE_TX", PIPE_TX),
            ("PHYSBRIDGE_PIPE_RX", PIPE_RX),
        ] {
            let line = src
                .lines()
                .find(|l| l.contains(define))
                .unwrap_or_else(|| panic!("{define} is no longer in PhysBridge.h"));
            // The header writes the backslashes doubled for C; ours are raw.
            let want = ours.replace('\\', r"\\");
            assert!(
                line.contains(&want),
                "{define} in PhysBridge.h is {line:?}, this file says {ours:?}"
            );
        }
    }
}
