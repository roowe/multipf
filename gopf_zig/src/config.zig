const std = @import("std");
const posix = std.posix;
const backend_mod = @import("backend.zig");
const Backend = backend_mod.Backend;
const Host = backend_mod.Host;

extern "c" fn close(fd: posix.fd_t) c_int;

const ConfigHost = struct {
    Addr: []const u8,
    Weight: i32,
};

const Config = struct {
    Hosts: []ConfigHost,
};

/// Read entire file into allocated buffer using POSIX API.
fn readFileAlloc(allocator: std.mem.Allocator, path: []const u8) ![]u8 {
    const fd = try posix.openat(posix.AT.FDCWD, path, .{}, 0);
    defer _ = close(fd);

    var buf = try allocator.alloc(u8, 4096);
    var total: usize = 0;

    while (true) {
        if (total >= buf.len) {
            buf = try allocator.realloc(buf, buf.len * 2);
        }
        const n = try posix.read(fd, buf[total..]);
        if (n == 0) break;
        total += n;
    }

    return try allocator.realloc(buf, total);
}

/// Load and parse a JSON config file. Returns a Backend with owned memory.
pub fn loadConfig(allocator: std.mem.Allocator, path: []const u8) !Backend {
    const content = try readFileAlloc(allocator, path);
    defer allocator.free(content);

    const parsed = try std.json.parseFromSlice(Config, allocator, content, .{});
    defer parsed.deinit();

    const hosts = try allocator.alloc(Host, parsed.value.Hosts.len);
    var total_weight: i32 = 0;

    for (parsed.value.Hosts, 0..) |config_host, i| {
        const addr = try allocator.dupe(u8, config_host.Addr);
        hosts[i] = .{
            .addr = addr,
            .weight = config_host.Weight,
        };
        total_weight += config_host.Weight;
    }

    return .{
        .hosts = hosts,
        .total_weight = total_weight,
        .allocator = allocator,
    };
}
