#!/usr/bin/env node

/**
 * gopf_ts — a lightweight TCP layer-4 load balancer in TypeScript / Node.js.
 *
 * Usage:
 *   node dist/main.js [--listen <port>] <config_file>
 *   npx ts-node src/main.ts [--listen <port>] <config_file>
 */

import { Backend, BackendStore } from './backend';
import { loadConfig } from './config';
import { startProxy } from './proxy';

const DEFAULT_PORT = 1248;

interface CliArgs {
  listen: number;
  configFile: string | null;
}

function parseArgs(argv: string[]): CliArgs {
  let listen = DEFAULT_PORT;
  let configFile: string | null = null;

  let i = 0;
  while (i < argv.length) {
    const arg = argv[i];
    if (arg === '--listen' || arg === '-l') {
      i++;
      if (i >= argv.length) {
        usage();
        process.exit(1);
      }
      const port = parseInt(argv[i], 10);
      if (isNaN(port) || port < 1 || port > 65535) {
        console.error(`invalid port: ${argv[i]}`);
        process.exit(1);
      }
      listen = port;
    } else if (arg.startsWith('--listen=')) {
      const portStr = arg.substring('--listen='.length);
      const port = parseInt(portStr, 10);
      if (isNaN(port) || port < 1 || port > 65535) {
        console.error(`invalid port: ${portStr}`);
        process.exit(1);
      }
      listen = port;
    } else {
      configFile = arg;
    }
    i++;
  }

  return { listen, configFile };
}

function usage(): void {
  console.error('usage: gopf_ts [--listen <port>] <config_file>');
}

function main(): void {
  // Skip node binary and script path
  const args = parseArgs(process.argv.slice(2));

  if (args.configFile === null) {
    usage();
    process.exit(1);
  }

  const configPath = args.configFile;

  // Load initial config
  let initialBackend: Backend;
  try {
    initialBackend = loadConfig(configPath);
  } catch (err) {
    console.error(`load config failed: ${(err as Error).message}`);
    process.exit(1);
  }

  const store = new BackendStore(initialBackend);

  // SIGHUP → reload config
  process.on('SIGHUP', () => {
    try {
      const newBackend = loadConfig(configPath);
      store.update(newBackend);
      console.log('reload succeed');
    } catch (err) {
      console.error(`reload failed: ${(err as Error).message}`);
    }
  });

  // Graceful shutdown
  process.on('SIGINT', () => {
    console.log('\nshutting down');
    process.exit(0);
  });

  process.on('SIGTERM', () => {
    console.log('shutting down');
    process.exit(0);
  });

  // Start proxy (blocks via event loop)
  startProxy(args.listen, store);
}

main();
