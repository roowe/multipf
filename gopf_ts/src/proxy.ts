/**
 * TCP proxy core — listener, connection handling, bidirectional forwarding.
 *
 * Key: allowHalfOpen: true on both server and client sockets.
 * By default Node.js auto-destroys the socket when the peer sends FIN.
 * allowHalfOpen: true keeps the write side open, enabling TCP half-close
 * (same as Go's CloseWrite / Elixir's exit_on_close: false).
 */

import * as net from 'net';
import { BackendStore, chooseHost } from './backend';

const CONNECT_TIMEOUT_MS = 5000;
const RECV_BUF_SIZE = 64 * 1024;

/**
 * Start the TCP proxy listener. Returns the server instance.
 */
export function startProxy(port: number, store: BackendStore): net.Server {
  const server = net.createServer({ allowHalfOpen: true });

  server.on('connection', (client: net.Socket) => {
    handleConnection(client, store);
  });

  server.on('error', (err: Error) => {
    console.error(`server error: ${err.message}`);
  });

  server.listen(port, '0.0.0.0', () => {
    console.log(`gopf_ts listening on port ${port}`);
  });

  return server;
}

function handleConnection(client: net.Socket, store: BackendStore): void {
  const backend = store.get();
  const host = chooseHost(backend);

  if (host === null) {
    console.error('choose host failed');
    client.destroy();
    return;
  }

  // Parse "host:port"
  const lastColon = host.addr.lastIndexOf(':');
  const targetHost = host.addr.substring(0, lastColon);
  const targetPort = parseInt(host.addr.substring(lastColon + 1), 10);

  const upstream = net.createConnection({
    host: targetHost,
    port: targetPort,
    allowHalfOpen: true,
  });

  upstream.setTimeout(CONNECT_TIMEOUT_MS);

  upstream.on('connect', () => {
    upstream.setTimeout(0); // clear connect timeout
    setKeepAlive(client);
    setKeepAlive(upstream);
    forwardBoth(client, upstream);
  });

  upstream.on('timeout', () => {
    console.error(`connect to ${host.addr} timed out`);
    upstream.destroy();
    client.destroy();
  });

  upstream.on('error', (err: Error) => {
    console.error(`connect to ${host.addr} failed: ${err.message}`);
    client.destroy();
  });
}

function setKeepAlive(socket: net.Socket): void {
  socket.setKeepAlive(true, 60000);
}

/**
 * Bidirectional forwarding between client and upstream.
 * Uses data events for explicit control over half-close behavior.
 */
function forwardBoth(client: net.Socket, upstream: net.Socket): void {
  let clientClosed = false;
  let upstreamClosed = false;

  const tryDestroy = () => {
    if (clientClosed && upstreamClosed) {
      client.destroy();
      upstream.destroy();
    }
  };

  // client -> upstream
  client.on('data', (data: Buffer) => {
    if (!upstreamClosed) {
      upstream.write(data);
    }
  });

  client.on('end', () => {
    clientClosed = true;
    // Half-close: signal upstream that no more data is coming
    if (!upstreamClosed) {
      upstream.end();
    }
    tryDestroy();
  });

  client.on('error', () => {
    clientClosed = true;
    tryDestroy();
  });

  // upstream -> client
  upstream.on('data', (data: Buffer) => {
    if (!clientClosed) {
      client.write(data);
    }
  });

  upstream.on('end', () => {
    upstreamClosed = true;
    // Half-close: signal client that no more data is coming
    if (!clientClosed) {
      client.end();
    }
    tryDestroy();
  });

  upstream.on('error', () => {
    upstreamClosed = true;
    tryDestroy();
  });
}
