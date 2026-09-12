/**
 * Backend host definitions and weighted random selection.
 */

export interface Host {
  addr: string;
  weight: number;
}

export interface Backend {
  hosts: Host[];
  totalWeight: number;
}

export function createBackend(hosts: Host[]): Backend {
  const totalWeight = hosts.reduce((sum, h) => sum + h.weight, 0);
  return { hosts, totalWeight };
}

/**
 * Thread-safe backend storage with hot-reload support.
 * Node.js is single-threaded, so no locking is needed, but this
 * provides a clean interface consistent with other implementations.
 */
export class BackendStore {
  private backend: Backend;

  constructor(initial: Backend) {
    this.backend = initial;
  }

  get(): Backend {
    return this.backend;
  }

  update(newBackend: Backend): void {
    this.backend = newBackend;
  }
}

/**
 * Weighted random host selection. Returns null if totalWeight <= 0.
 */
export function chooseHost(backend: Backend): Host | null {
  if (backend.totalWeight <= 0 || backend.hosts.length === 0) {
    return null;
  }
  let v = Math.floor(Math.random() * backend.totalWeight);
  for (const host of backend.hosts) {
    if (v < host.weight) {
      return host;
    }
    v -= host.weight;
  }
  return null;
}
