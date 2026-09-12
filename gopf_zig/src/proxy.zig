const std = @import("std");
const net = @import("net.zig");
const backend_mod = @import("backend.zig");
const BackendStore = backend_mod.BackendStore;
const chooseHost = backend_mod.chooseHost;

const CONNECT_TIMEOUT_SECS = 5;
const RECV_BUF_SIZE = 64 * 1024;
const LISTEN_BACKLOG: c_int = 4096;

/// Start the TCP proxy listener. Blocks the calling thread in the accept loop.
pub fn startProxy(port: u16, store: *BackendStore) !void {
    const listen_fd = try net.socketErr(net.AF_INET, net.SOCK_STREAM, net.IPPROTO_TCP);
    defer net.closeErr(listen_fd);

    // SO_REUSEADDR
    const reuse: c_int = 1;
    try net.setsockoptErr(
        listen_fd,
        net.SOL_SOCKET,
        net.SO_REUSEADDR,
        &reuse,
        @sizeOf(c_int),
    );

    var addr: net.sockaddr_in = .{
        .sin_family = net.AF_INET,
        .sin_port = net.htons(port),
        .sin_addr = .{ .s_addr = net.htonl(0) },
        .sin_zero = [_]u8{0} ** 8,
    };
    try net.bindErr(listen_fd, @ptrCast(&addr), @sizeOf(net.sockaddr_in));
    try net.listenErr(listen_fd, LISTEN_BACKLOG);

    std.log.info("gopf_zig listening on port {}", .{port});

    while (true) {
        var client_addr: net.sockaddr_in = undefined;
        var client_addr_len: net.socklen_t = @sizeOf(net.sockaddr_in);
        const client_fd = net.acceptErr(
            listen_fd,
            @ptrCast(&client_addr),
            &client_addr_len,
        ) catch |err| {
            std.log.err("accept failed: {}", .{err});
            continue;
        };

        const args = try std.heap.c_allocator.create(ConnArgs);
        args.* = .{
            .client_fd = client_fd,
            .store = store,
        };

        const thread = std.Thread.spawn(.{}, handleConnection, .{args}) catch |err| {
            std.log.err("spawn handler failed: {}", .{err});
            net.closeErr(client_fd);
            std.heap.c_allocator.destroy(args);
            continue;
        };
        thread.detach();
    }
}

const ConnArgs = struct {
    client_fd: net.fd_t,
    store: *BackendStore,
};

fn handleConnection(args: *ConnArgs) void {
    defer std.heap.c_allocator.destroy(args);
    const client_fd = args.client_fd;
    const store = args.store;

    // Per-connection local RNG (thread-safe, no global contention)
    var local_prng = std.Random.DefaultPrng.init(@intFromPtr(args) ^ 0xdeadbeef);
    const rng = local_prng.random();

    const backend = store.get();
    const host = chooseHost(rng, &backend) orelse {
        std.log.err("choose host failed", .{});
        net.closeErr(client_fd);
        return;
    };

    const upstream_fd = connectBackend(host.addr) catch |err| {
        std.log.err("connect to {s} failed: {}", .{ host.addr, err });
        net.closeErr(client_fd);
        return;
    };

    setKeepAlive(client_fd);
    setKeepAlive(upstream_fd);

    forwardBoth(client_fd, upstream_fd);

    net.closeErr(client_fd);
    net.closeErr(upstream_fd);
}

fn connectBackend(addr_str: []const u8) !net.fd_t {
    // Parse "host:port"
    const colon = std.mem.indexOfScalar(u8, addr_str, ':') orelse return error.InvalidAddr;
    const host = addr_str[0..colon];
    const port_str = addr_str[colon + 1 ..];
    const port = try std.fmt.parseInt(u16, port_str, 10);

    // Resolve hostname
    const ip = try resolveHost(host);

    const fd = try net.socketErr(net.AF_INET, net.SOCK_STREAM, net.IPPROTO_TCP);
    errdefer net.closeErr(fd);

    var addr: net.sockaddr_in = .{
        .sin_family = net.AF_INET,
        .sin_port = net.htons(port),
        .sin_addr = .{ .s_addr = net.htonl(ip) },
        .sin_zero = [_]u8{0} ** 8,
    };

    // Set connect timeout via SO_SNDTIMEO
    const timeout: net.timeval = .{
        .tv_sec = CONNECT_TIMEOUT_SECS,
        .tv_usec = 0,
    };
    net.setsockoptErr(
        fd,
        net.SOL_SOCKET,
        net.SO_SNDTIMEO,
        &timeout,
        @sizeOf(net.timeval),
    ) catch {};

    try net.connectErr(fd, @ptrCast(&addr), @sizeOf(net.sockaddr_in));
    return fd;
}

fn resolveHost(host: []const u8) !u32 {
    // Try inet_addr for IPv4 literals first
    const host_z = try std.heap.c_allocator.dupeZ(u8, host);
    defer std.heap.c_allocator.free(host_z);

    const ip = net.c.inet_addr(host_z.ptr);
    if (ip != net.c.INADDR_NONE) {
        return std.mem.bigToNative(u32, ip);
    }

    // Fallback: getaddrinfo for hostname resolution
    var hints: net.c.addrinfo = .{
        .ai_family = net.AF_INET,
        .ai_socktype = net.SOCK_STREAM,
        .ai_flags = 0,
        .ai_protocol = 0,
        .ai_addrlen = 0,
        .ai_addr = null,
        .ai_canonname = null,
        .ai_next = null,
    };
    var result: ?*net.c.addrinfo = null;
    const rc = net.c.getaddrinfo(host_z.ptr, null, &hints, &result);
    if (rc != 0) return error.ResolveFailed;
    defer if (result) |r| net.c.freeaddrinfo(r);

    const res = result orelse return error.NoAddress;
    if (res.ai_addr == null) return error.NoAddress;
    const addr_in: *net.sockaddr_in = @ptrCast(@alignCast(res.ai_addr.?));
    return std.mem.bigToNative(u32, addr_in.sin_addr.s_addr);
}

fn setKeepAlive(fd: net.fd_t) void {
    const keepalive: c_int = 1;
    net.setsockoptErr(
        fd,
        net.SOL_SOCKET,
        net.SO_KEEPALIVE,
        &keepalive,
        @sizeOf(c_int),
    ) catch {};
}

/// Bidirectional forwarding: spawn a thread for each direction, wait for both.
fn forwardBoth(fd_a: net.fd_t, fd_b: net.fd_t) void {
    const args_a = std.heap.c_allocator.create(ForwardArgs) catch return;
    args_a.* = .{ .source = fd_a, .dest = fd_b };
    const thread_a = std.Thread.spawn(.{}, forward, .{args_a}) catch {
        std.heap.c_allocator.destroy(args_a);
        return;
    };

    const args_b = std.heap.c_allocator.create(ForwardArgs) catch {
        thread_a.join();
        return;
    };
    args_b.* = .{ .source = fd_b, .dest = fd_a };
    const thread_b = std.Thread.spawn(.{}, forward, .{args_b}) catch {
        std.heap.c_allocator.destroy(args_b);
        thread_a.join();
        return;
    };

    thread_a.join();
    thread_b.join();
}

const ForwardArgs = struct {
    source: net.fd_t,
    dest: net.fd_t,
};

/// Forward data from source to dest until source closes its write side.
/// On source EOF, half-closes dest's write side (TCP half-close).
fn forward(args: *ForwardArgs) void {
    defer std.heap.c_allocator.destroy(args);
    var buf: [RECV_BUF_SIZE]u8 = undefined;

    while (true) {
        const n = net.recvErr(args.source, &buf, 0) catch break;
        if (n == 0) break; // peer closed write side

        var sent: usize = 0;
        while (sent < n) {
            const s = net.sendErr(args.dest, buf[sent..n], 0) catch break;
            if (s == 0) break;
            sent += s;
        }
    }

    // Half-close: signal dest that no more data is coming
    net.shutdownErr(args.dest, net.SHUT_WR);
}
