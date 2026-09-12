use crate::backend::{choose_host, BackendStore};
use rand::rngs::StdRng;
use rand::SeedableRng;
use std::io::{self, Read, Write};
use std::net::{TcpListener, TcpStream};
use std::thread;
use std::time::Duration;

const CONNECT_TIMEOUT: Duration = Duration::from_secs(5);
const RECV_BUF_SIZE: usize = 64 * 1024;

/// Start the TCP proxy listener. Blocks the calling thread in the accept loop.
pub fn start_proxy(port: u16, store: BackendStore) -> io::Result<()> {
    let listener = TcpListener::bind(("0.0.0.0", port))?;
    eprintln!("gopf_rust listening on port {}", port);

    for stream in listener.incoming() {
        match stream {
            Ok(client) => {
                let store = store.clone();
                thread::spawn(move || {
                    handle_connection(client, store);
                });
            }
            Err(e) => {
                eprintln!("accept failed: {}", e);
            }
        }
    }
    Ok(())
}

fn handle_connection(client: TcpStream, store: BackendStore) {
    // Per-connection RNG (thread-safe, no global contention)
    let mut rng = StdRng::from_entropy();

    let backend = store.get();
    let host = match choose_host(&mut rng, &backend) {
        Some(h) => h.clone(),
        None => {
            eprintln!("choose host failed");
            return;
        }
    };

    let upstream = match TcpStream::connect(&host.addr) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("connect to {} failed: {}", host.addr, e);
            return;
        }
    };

    // Set timeouts to avoid hanging
    let _ = client.set_read_timeout(Some(CONNECT_TIMEOUT));
    let _ = upstream.set_read_timeout(Some(CONNECT_TIMEOUT));

    set_keepalive(&client);
    set_keepalive(&upstream);

    forward_both(client, upstream);
}

fn set_keepalive(stream: &TcpStream) {
    // socket2 would be cleaner, but use setsockopt via raw fd
    #[cfg(unix)]
    {
        use std::os::unix::io::AsRawFd;
        let fd = stream.as_raw_fd();
        let optval: libc::c_int = 1;
        unsafe {
            libc::setsockopt(
                fd,
                libc::SOL_SOCKET,
                libc::SO_KEEPALIVE,
                &optval as *const _ as *const libc::c_void,
                std::mem::size_of::<libc::c_int>() as libc::socklen_t,
            );
        }
    }
}

/// Bidirectional forwarding: spawn a thread for each direction, wait for both.
fn forward_both(client: TcpStream, upstream: TcpStream) {
    let client_clone = match client.try_clone() {
        Ok(c) => c,
        Err(_) => return,
    };
    let upstream_clone = match upstream.try_clone() {
        Ok(u) => u,
        Err(_) => return,
    };

    // client -> upstream
    let handle_a = thread::spawn(move || {
        forward(client_clone, upstream);
    });

    // upstream -> client
    let handle_b = thread::spawn(move || {
        forward(upstream_clone, client);
    });

    let _ = handle_a.join();
    let _ = handle_b.join();
}

/// Forward data from source to dest until source closes its read side.
/// On source EOF, half-closes dest's write side (TCP half-close).
fn forward(mut source: TcpStream, mut dest: TcpStream) {
    let mut buf = [0u8; RECV_BUF_SIZE];

    loop {
        match source.read(&mut buf) {
            Ok(0) => break, // peer closed write side
            Ok(n) => {
                if dest.write_all(&buf[..n]).is_err() {
                    break;
                }
            }
            Err(_) => break,
        }
    }

    // Half-close: signal dest that no more data is coming
    let _ = dest.shutdown(std::net::Shutdown::Write);
}
