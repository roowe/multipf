use clap::Parser;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpListener;

/// High-performance TCP echo server for benchmarking gopf proxies.
///
/// Every connection is handled concurrently on the tokio runtime.
/// The server reads data and echoes it back unchanged, which lets the
/// benchmark client measure round-trip latency and throughput through
/// the proxy under test.
#[derive(Parser, Debug)]
#[command(name = "bench-backend", version, about)]
struct Args {
    /// Address to listen on
    #[arg(short, long, default_value = "127.0.0.1:9000")]
    listen: String,

    /// Response mode: echo (send back what was received) or fixed (send a
    /// fixed-size payload on every connection).
    #[arg(short, long, default_value = "echo")]
    mode: String,

    /// Fixed response size in bytes (only used when --mode fixed)
    #[arg(short, long, default_value_t = 1024)]
    size: usize,
}

#[tokio::main(flavor = "multi_thread", worker_threads = 4)]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args = Args::parse();

    let listener = TcpListener::bind(&args.listen).await?;
    eprintln!("bench-backend listening on {} (mode={})", args.listen, args.mode);

    let fixed_payload: Option<Vec<u8>> = if args.mode == "fixed" {
        Some(vec![b'X'; args.size])
    } else {
        None
    };

    loop {
        let (mut socket, addr) = listener.accept().await?;
        let payload = fixed_payload.clone();

        tokio::spawn(async move {
            let mut buf = vec![0u8; 64 * 1024];

            if let Some(p) = payload {
                // Fixed mode: send payload once, then close
                let _ = socket.write_all(&p).await;
                let _ = socket.shutdown().await;
                return;
            }

            // Echo mode: loop until client closes
            loop {
                match socket.read(&mut buf).await {
                    Ok(0) => break, // client closed
                    Ok(n) => {
                        if socket.write_all(&buf[..n]).await.is_err() {
                            break;
                        }
                    }
                    Err(_) => break,
                }
            }
            let _ = socket.shutdown().await;
            let _ = addr;
        });
    }
}
