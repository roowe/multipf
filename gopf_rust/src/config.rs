use crate::backend::{Backend, Host};
use serde::Deserialize;
use std::fs;

#[derive(Debug, Deserialize)]
struct ConfigHost {
    #[serde(rename = "Addr")]
    addr: String,
    #[serde(rename = "Weight")]
    weight: i32,
}

#[derive(Debug, Deserialize)]
struct Config {
    #[serde(rename = "Hosts")]
    hosts: Vec<ConfigHost>,
}

/// Load and parse a JSON config file.
pub fn load_config(path: &str) -> Result<Backend, Box<dyn std::error::Error>> {
    let content = fs::read_to_string(path)?;
    let config: Config = serde_json::from_str(&content)?;

    let hosts: Vec<Host> = config
        .hosts
        .into_iter()
        .map(|h| Host {
            addr: h.addr,
            weight: h.weight,
        })
        .collect();

    Ok(Backend::new(hosts))
}
