const std = @import("std");

pub const Host = struct {
    addr: []const u8,
    weight: i32,
};

pub const Backend = struct {
    hosts: []Host,
    total_weight: i32,
    allocator: std.mem.Allocator,

    pub fn deinit(self: *Backend) void {
        for (self.hosts) |host| {
            self.allocator.free(host.addr);
        }
        self.allocator.free(self.hosts);
    }
};

/// Spin until mutex is acquired (std.atomic.Mutex has no blocking lock).
fn spinLock(m: *std.atomic.Mutex) void {
    while (!m.tryLock()) {
        std.Thread.yield() catch {};
    }
}

/// Thread-safe global backend storage with hot-reload support.
pub const BackendStore = struct {
    current: Backend,
    mutex: std.atomic.Mutex = .unlocked,
    allocator: std.mem.Allocator,

    pub fn init(allocator: std.mem.Allocator, initial: Backend) BackendStore {
        return .{
            .current = initial,
            .allocator = allocator,
        };
    }

    pub fn get(self: *BackendStore) Backend {
        spinLock(&self.mutex);
        defer self.mutex.unlock();
        return self.current;
    }

    pub fn update(self: *BackendStore, new_backend: Backend) void {
        spinLock(&self.mutex);
        defer self.mutex.unlock();
        var old = self.current;
        self.current = new_backend;
        old.deinit();
    }
};

/// Weighted random host selection. Returns null if total_weight <= 0.
pub fn chooseHost(rng: std.Random, backend: *const Backend) ?*const Host {
    if (backend.total_weight <= 0 or backend.hosts.len == 0) return null;
    var v = rng.intRangeAtMost(i32, 0, backend.total_weight - 1);
    for (backend.hosts) |*host| {
        if (v < host.weight) return host;
        v -= host.weight;
    }
    return null;
}
