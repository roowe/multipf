use crate::backend::{choose_host, BackendStore};
use rand::rngs::StdRng;
use rand::SeedableRng;
use std::io::{self, Read, Write};
use std::net::{TcpListener, TcpStream};
use std::os::unix::io::FromRawFd;
use std::thread;
use std::time::Duration;

const CONNECT_TIMEOUT: Duration = Duration::from_secs(5);
const RECV_BUF_SIZE: usize = 64 * 1024;
const LISTEN_BACKLOG: i32 = 4096;

/// Create a TCP listener with a large backlog to handle bursty connections.
/// Rust's default TcpListener::bind uses backlog=128, which drops incoming
/// connections under high concurrency. We use libc directly to set 4096.
fn create_listener(port: u16) -> io::Result<TcpListener> {
    unsafe {
        let fd = libc::socket(libc::AF_INET, libc::SOCK_STREAM, 0);
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }

        let optval: libc::c_int = 1;
        libc::setsockopt(
            fd,
            libc::SOL_SOCKET,
            libc::SO_REUSEADDR,
            &optval as *const _ as *const libc::c_void,
            std::mem::size_of::<libc::c_int>() as libc::socklen_t,
        );

        let addr = libc::sockaddr_in {
            sin_len: std::mem::size_of::<libc::sockaddr_in>() as u8,
            sin_family: libc::AF_INET as libc::sa_family_t,
            sin_port: port.to_be(),
            sin_addr: libc::in_addr {
                s_addr: libc::INADDR_ANY,
            },
            sin_zero: [0; 8],
        };

        if libc::bind(
            fd,
            &addr as *const _ as *const libc::sockaddr,
            std::mem::size_of::<libc::sockaddr_in>() as libc::socklen_t,
        ) < 0
        {
            libc::close(fd);
            return Err(io::Error::last_os_error());
        }

        if libc::listen(fd, LISTEN_BACKLOG) < 0 {
            libc::close(fd);
            return Err(io::Error::last_os_error());
        }

        Ok(TcpListener::from_raw_fd(fd))
    }
}

/// Start the TCP proxy listener. Blocks the calling thread in the accept loop.
pub fn start_proxy(port: u16, store: BackendStore) -> io::Result<()> {
    let listener = create_listener(port)?;
    eprintln!("gopf_rust listening on port {} (backlog={})", port, LISTEN_BACKLOG);

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

    // Connect with timeout to avoid hanging threads under load
    let sock_addr = match host.addr.parse::<std::net::SocketAddr>() {
        Ok(addr) => addr,
        Err(_) => {
            eprintln!("invalid backend addr: {}", host.addr);
            return;
        }
    };
    let upstream = match TcpStream::connect_timeout(&sock_addr, CONNECT_TIMEOUT) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("connect to {} failed: {}", host.addr, e);
            return;
        }
    };

    // NOTE: No read/write timeouts. Previously a 5s read timeout caused
    // mass connection resets under high concurrency when CPU scheduling
    // delayed data delivery. The forwarding loops rely on peer FIN/EOF
    // to terminate naturally, matching the Go reference implementation.

    set_keepalive(&client);
    set_keepalive(&upstream);

    forward_both(client, upstream);
}

fn set_keepalive(stream: &TcpStream) {
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
