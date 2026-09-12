use rand::Rng;
use std::sync::{Arc, RwLock};

#[derive(Debug, Clone)]
pub struct Host {
    pub addr: String,
    pub weight: i32,
}

#[derive(Debug, Clone)]
pub struct Backend {
    pub hosts: Vec<Host>,
    pub total_weight: i32,
}

impl Backend {
    pub fn new(hosts: Vec<Host>) -> Self {
        let total_weight = hosts.iter().map(|h| h.weight).sum();
        Backend {
            hosts,
            total_weight,
        }
    }
}

/// Thread-safe backend storage with hot-reload support.
#[derive(Clone)]
pub struct BackendStore {
    inner: Arc<RwLock<Backend>>,
}

impl BackendStore {
    pub fn new(backend: Backend) -> Self {
        BackendStore {
            inner: Arc::new(RwLock::new(backend)),
        }
    }

    pub fn get(&self) -> Backend {
        self.inner.read().unwrap().clone()
    }

    pub fn update(&self, new_backend: Backend) {
        let mut guard = self.inner.write().unwrap();
        *guard = new_backend;
    }
}

/// Weighted random host selection. Returns None if total_weight <= 0.
pub fn choose_host<'a, R: Rng>(rng: &mut R, backend: &'a Backend) -> Option<&'a Host> {
    if backend.total_weight <= 0 || backend.hosts.is_empty() {
        return None;
    }
    let mut v = rng.gen_range(0..backend.total_weight);
    for host in &backend.hosts {
        if v < host.weight {
            return Some(host);
        }
        v -= host.weight;
    }
    None
}
