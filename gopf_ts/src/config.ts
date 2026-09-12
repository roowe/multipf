/**
 * JSON config file loading.
 */

import * as fs from 'fs';
import { Backend, Host, createBackend } from './backend';

interface ConfigHost {
  Addr: string;
  Weight: number;
}

interface Config {
  Hosts: ConfigHost[];
}

/**
 * Load and parse a JSON config file.
 */
export function loadConfig(path: string): Backend {
  const content = fs.readFileSync(path, 'utf-8');
  const config: Config = JSON.parse(content);

  const hosts: Host[] = config.Hosts.map((h) => ({
    addr: h.Addr,
    weight: h.Weight,
  }));

  return createBackend(hosts);
}
