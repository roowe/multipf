//! Direct C socket API via @cImport for maximum compatibility with Zig 0.16.
pub const c = @cImport({
    @cInclude("sys/socket.h");
    @cInclude("netinet/in.h");
    @cInclude("arpa/inet.h");
    @cInclude("unistd.h");
    @cInclude("sys/time.h");
    @cInclude("fcntl.h");
    @cInclude("netdb.h");
});

pub const fd_t = c_int;
pub const socklen_t = c.socklen_t;
pub const sockaddr = c.sockaddr;
pub const sockaddr_in = c.sockaddr_in;
pub const timeval = c.timeval;

// Constants
pub const AF_INET = c.AF_INET;
pub const SOCK_STREAM = c.SOCK_STREAM;
pub const IPPROTO_TCP = c.IPPROTO_TCP;
pub const SO_REUSEADDR = c.SO_REUSEADDR;
pub const SO_KEEPALIVE = c.SO_KEEPALIVE;
pub const SO_SNDTIMEO = c.SO_SNDTIMEO;
pub const SOL_SOCKET = c.SOL_SOCKET;
pub const SHUT_WR = c.SHUT_WR;
pub const INADDR_ANY = c.INADDR_ANY;

pub fn socketErr(domain: c_int, sock_type: c_int, protocol: c_int) !fd_t {
    const rc = c.socket(domain, sock_type, protocol);
    if (rc < 0) return error.SocketFailed;
    return @intCast(rc);
}

pub fn closeErr(fd: fd_t) void {
    _ = c.close(fd);
}

pub fn setsockoptErr(fd: fd_t, level: c_int, optname: c_int, optval: *const anyopaque, optlen: socklen_t) !void {
    if (c.setsockopt(fd, level, optname, optval, optlen) < 0) return error.SetSockOptFailed;
}

pub fn bindErr(fd: fd_t, addr: *const sockaddr, addrlen: socklen_t) !void {
    if (c.bind(fd, addr, addrlen) < 0) return error.BindFailed;
}

pub fn listenErr(fd: fd_t, backlog: c_int) !void {
    if (c.listen(fd, backlog) < 0) return error.ListenFailed;
}

pub fn acceptErr(fd: fd_t, addr: ?*sockaddr, addrlen: ?*socklen_t) !fd_t {
    const rc = c.accept(fd, addr, addrlen);
    if (rc < 0) return error.AcceptFailed;
    return @intCast(rc);
}

pub fn connectErr(fd: fd_t, addr: *const sockaddr, addrlen: socklen_t) !void {
    if (c.connect(fd, addr, addrlen) < 0) return error.ConnectFailed;
}

pub fn recvErr(fd: fd_t, buf: []u8, flags: c_int) !usize {
    const rc = c.recv(fd, buf.ptr, buf.len, flags);
    if (rc < 0) return error.RecvFailed;
    return @intCast(rc);
}

pub fn sendErr(fd: fd_t, buf: []const u8, flags: c_int) !usize {
    const rc = c.send(fd, buf.ptr, buf.len, flags);
    if (rc < 0) return error.SendFailed;
    return @intCast(rc);
}

pub fn shutdownErr(fd: fd_t, how: c_int) void {
    _ = c.shutdown(fd, how);
}

pub fn htons(port: u16) u16 {
    return c.htons(port);
}

pub fn htonl(ip: u32) u32 {
    return c.htonl(ip);
}
