const std = @import("std");
const posix = std.posix;
const backend_mod = @import("backend.zig");
const config_mod = @import("config.zig");
const proxy_mod = @import("proxy.zig");

const BackendStore = backend_mod.BackendStore;

var global_store: ?BackendStore = null;
var global_config_path: ?[]const u8 = null;
const allocator = std.heap.c_allocator;

fn reloadConfig(_: posix.SIG) callconv(.c) void {
    const path = global_config_path orelse return;
    const store = if (global_store) |*s| s else return;

    const new_backend = config_mod.loadConfig(allocator, path) catch |err| {
        std.log.err("reload failed: {}", .{err});
        return;
    };
    store.update(new_backend);
    std.log.info("reload succeed", .{});
}

fn printStatus(_: posix.SIG) callconv(.c) void {
    std.log.info("gopf_zig running", .{});
}

fn setupSignalHandlers() void {
    // SIGHUP -> reload config
    var sa_hup: posix.Sigaction = .{
        .handler = .{ .handler = reloadConfig },
        .mask = std.mem.zeroes(posix.sigset_t),
        .flags = 0,
    };
    posix.sigaction(posix.SIG.HUP, &sa_hup, null);

    // SIGUSR1 -> print status
    var sa_usr1: posix.Sigaction = .{
        .handler = .{ .handler = printStatus },
        .mask = std.mem.zeroes(posix.sigset_t),
        .flags = 0,
    };
    posix.sigaction(posix.SIG.USR1, &sa_usr1, null);
}

fn usage() void {
    std.debug.print("usage: gopf_zig [--listen <port>] <config_file>\n", .{});
}

pub fn main(init: std.process.Init.Minimal) !void {
    const argv = init.args.vector;
    var i: usize = 1; // skip program name

    var listen_port: u16 = 1248;
    var config_path: ?[]const u8 = null;

    while (i < argv.len) : (i += 1) {
        const arg = std.mem.span(argv[i]);
        if (std.mem.eql(u8, arg, "--listen") or std.mem.eql(u8, arg, "-l")) {
            i += 1;
            if (i >= argv.len) {
                usage();
                return error.MissingArg;
            }
            const port_str = std.mem.span(argv[i]);
            listen_port = std.fmt.parseInt(u16, port_str, 10) catch {
                std.log.err("invalid port: {s}", .{port_str});
                return error.InvalidPort;
            };
        } else {
            config_path = try allocator.dupe(u8, arg);
        }
    }

    const path = config_path orelse {
        usage();
        return error.MissingConfig;
    };

    // Load initial config
    const initial_backend = config_mod.loadConfig(allocator, path) catch |err| {
        std.log.err("load config failed: {}", .{err});
        return error.LoadConfigFailed;
    };

    global_store = BackendStore.init(allocator, initial_backend);
    global_config_path = path;

    setupSignalHandlers();

    try proxy_mod.startProxy(listen_port, &global_store.?);
}
