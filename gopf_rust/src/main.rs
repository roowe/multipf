mod backend;
mod config;
mod proxy;

use backend::BackendStore;
use signal_hook::{consts::SIGHUP, flag};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::Duration;

const DEFAULT_PORT: u16 = 1248;

fn main() {
    let args: Vec<String> = std::env::args().collect();

    let mut listen_port = DEFAULT_PORT;
    let mut config_path: Option<String> = None;

    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "--listen" | "-l" => {
                i += 1;
                if i >= args.len() {
                    usage();
                    std::process::exit(1);
                }
                listen_port = match args[i].parse::<u16>() {
                    Ok(p) => p,
                    Err(_) => {
                        eprintln!("invalid port: {}", args[i]);
                        std::process::exit(1);
                    }
                };
            }
            _ => {
                config_path = Some(args[i].clone());
            }
        }
        i += 1;
    }

    let path = match config_path {
        Some(p) => p,
        None => {
            usage();
            std::process::exit(1);
        }
    };

    // Load initial config
    let initial_backend = match config::load_config(&path) {
        Ok(b) => b,
        Err(e) => {
            eprintln!("load config failed: {}", e);
            std::process::exit(1);
        }
    };

    let store = BackendStore::new(initial_backend);

    // Setup SIGHUP handler for config reload
    let reload_flag = Arc::new(AtomicBool::new(false));
    flag::register(SIGHUP, reload_flag.clone()).expect("failed to register SIGHUP handler");

    // Background thread for config reload
    let store_clone = store.clone();
    let path_clone = path.clone();
    let reload_clone = reload_flag.clone();
    thread::spawn(move || loop {
        thread::sleep(Duration::from_secs(1));
        if reload_clone.swap(false, Ordering::Relaxed) {
            match config::load_config(&path_clone) {
                Ok(new_backend) => {
                    store_clone.update(new_backend);
                    eprintln!("reload succeed");
                }
                Err(e) => {
                    eprintln!("reload failed: {}", e);
                }
            }
        }
    });

    // Start proxy (blocks)
    if let Err(e) = proxy::start_proxy(listen_port, store) {
        eprintln!("proxy error: {}", e);
        std::process::exit(1);
    }
}

fn usage() {
    eprintln!("usage: gopf_rust [--listen <port>] <config_file>");
}
