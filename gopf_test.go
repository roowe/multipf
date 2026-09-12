package main

import (
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"sync"
	"testing"
	"time"
)

// ---------------------------------------------------------------------------
// chooseHost
// ---------------------------------------------------------------------------

func TestChooseHost_ZeroWeight(t *testing.T) {
	hosts := []Host{{Addr: "127.0.0.1:80", Weight: 100}}
	if got := chooseHost(0, hosts); got != nil {
		t.Errorf("chooseHost(0, ...) = %v, want nil", got)
	}
	if got := chooseHost(-1, hosts); got != nil {
		t.Errorf("chooseHost(-1, ...) = %v, want nil", got)
	}
}

func TestChooseHost_EmptyHosts(t *testing.T) {
	if got := chooseHost(100, nil); got != nil {
		t.Errorf("chooseHost(100, nil) = %v, want nil", got)
	}
	if got := chooseHost(100, []Host{}); got != nil {
		t.Errorf("chooseHost(100, []) = %v, want nil", got)
	}
}

func TestChooseHost_SingleHost(t *testing.T) {
	host := Host{Addr: "127.0.0.1:8080", Weight: 50}
	hosts := []Host{host}
	for i := 0; i < 1000; i++ {
		got := chooseHost(50, hosts)
		if got == nil {
			t.Fatalf("iteration %d: got nil, want non-nil", i)
		}
		if got.Addr != host.Addr {
			t.Errorf("iteration %d: got Addr=%s, want %s", i, got.Addr, host.Addr)
		}
	}
}

// TestChooseHost_WeightDistribution verifies that weighted random selection
// produces a distribution close to the configured weights.
func TestChooseHost_WeightDistribution(t *testing.T) {
	hosts := []Host{
		{Addr: "A", Weight: 1},
		{Addr: "B", Weight: 2},
		{Addr: "C", Weight: 3},
	}
	const totalWeight = 6
	const N = 60000
	counts := map[string]int{}

	for i := 0; i < N; i++ {
		h := chooseHost(totalWeight, hosts)
		if h == nil {
			t.Fatalf("iteration %d: got nil", i)
		}
		counts[h.Addr]++
	}

	expected := map[string]float64{
		"A": 1.0 / 6.0,
		"B": 2.0 / 6.0,
		"C": 3.0 / 6.0,
	}
	for addr, expRatio := range expected {
		gotRatio := float64(counts[addr]) / float64(N)
		diff := gotRatio - expRatio
		if diff < -0.03 || diff > 0.03 {
			t.Errorf("host %s: got ratio=%.4f, want ~%.4f (diff=%.4f, count=%d)",
				addr, gotRatio, expRatio, diff, counts[addr])
		}
	}
}

// TestChooseHost_DistinctPointers is a regression test for the loop-variable
// address bug: returned pointers must point at distinct slice elements.
func TestChooseHost_DistinctPointers(t *testing.T) {
	hosts := []Host{
		{Addr: "A", Weight: 1},
		{Addr: "B", Weight: 1},
	}
	seenA, seenB := false, false
	for i := 0; i < 2000; i++ {
		h := chooseHost(2, hosts)
		if h == nil {
			continue
		}
		if h.Addr == "A" {
			seenA = true
		}
		if h.Addr == "B" {
			seenB = true
		}
	}
	if !seenA {
		t.Error("host A was never selected")
	}
	if !seenB {
		t.Error("host B was never selected")
	}
}

// ---------------------------------------------------------------------------
// reloadConfig
// ---------------------------------------------------------------------------

func writeTempConfig(t *testing.T, content string) string {
	t.Helper()
	dir := t.TempDir()
	path := filepath.Join(dir, "settings.conf")
	if err := os.WriteFile(path, []byte(content), 0644); err != nil {
		t.Fatalf("write temp config: %v", err)
	}
	return path
}

func TestReloadConfig_Success(t *testing.T) {
	content := `{
		"Hosts": [
			{"Addr": "127.0.0.1:8080", "Weight": 100},
			{"Addr": "127.0.0.1:8081", "Weight": 200}
		]
	}`
	path := writeTempConfig(t, content)

	options.config = path
	if err := reloadConfig(); err != nil {
		t.Fatalf("reloadConfig() error = %v", err)
	}

	backendMu.RLock()
	defer backendMu.RUnlock()
	if len(options.backend.Hosts) != 2 {
		t.Fatalf("got %d hosts, want 2", len(options.backend.Hosts))
	}
	if options.backend.weight != 300 {
		t.Errorf("got weight=%d, want 300", options.backend.weight)
	}
	if options.backend.Hosts[0].Addr != "127.0.0.1:8080" {
		t.Errorf("host[0].Addr = %s, want 127.0.0.1:8080", options.backend.Hosts[0].Addr)
	}
	if options.backend.Hosts[1].Weight != 200 {
		t.Errorf("host[1].Weight = %d, want 200", options.backend.Hosts[1].Weight)
	}
}

func TestReloadConfig_FileNotFound(t *testing.T) {
	options.config = "/nonexistent/path/settings.conf"
	if err := reloadConfig(); err == nil {
		t.Error("expected error for nonexistent file, got nil")
	}
}

func TestReloadConfig_InvalidJSON(t *testing.T) {
	path := writeTempConfig(t, `{invalid json`)
	options.config = path
	if err := reloadConfig(); err == nil {
		t.Error("expected error for invalid JSON, got nil")
	}
}

func TestReloadConfig_EmptyHosts(t *testing.T) {
	path := writeTempConfig(t, `{"Hosts": []}`)
	options.config = path
	if err := reloadConfig(); err != nil {
		t.Fatalf("reloadConfig() error = %v", err)
	}
	backendMu.RLock()
	w := options.backend.weight
	backendMu.RUnlock()
	if w != 0 {
		t.Errorf("got weight=%d, want 0 for empty hosts", w)
	}
}

func TestReloadConfig_WeightAccumulation(t *testing.T) {
	content := `{
		"Hosts": [
			{"Addr": "h1", "Weight": 10},
			{"Addr": "h2", "Weight": 20},
			{"Addr": "h3", "Weight": 30},
			{"Addr": "h4", "Weight": 40}
		]
	}`
	path := writeTempConfig(t, content)
	options.config = path
	if err := reloadConfig(); err != nil {
		t.Fatalf("reloadConfig() error = %v", err)
	}
	backendMu.RLock()
	w := options.backend.weight
	backendMu.RUnlock()
	if w != 100 {
		t.Errorf("got weight=%d, want 100", w)
	}
}

// ---------------------------------------------------------------------------
// End-to-end proxy tests
// ---------------------------------------------------------------------------

// startEchoServer starts a TCP echo server and returns its address.
func startEchoServer(t *testing.T) string {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("echo server listen: %v", err)
	}
	go func() {
		for {
			conn, err := ln.Accept()
			if err != nil {
				return
			}
			go func(c net.Conn) {
				defer c.Close()
				io.Copy(c, c)
			}(conn)
		}
	}()
	t.Cleanup(func() { ln.Close() })
	return ln.Addr().String()
}

// setBackend configures the global backend used by handleConn.
func setBackend(t *testing.T, addrs ...string) {
	t.Helper()
	hosts := make([]Host, len(addrs))
	weight := 0
	for i, addr := range addrs {
		hosts[i] = Host{Addr: addr, Weight: 100}
		weight += 100
	}
	backendMu.Lock()
	options.backend = Backend{Hosts: hosts, weight: weight}
	backendMu.Unlock()
}

// startProxyListener starts a TCP listener that dispatches each connection to
// handleConn, and returns the listener address.
func startProxyListener(t *testing.T) string {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("proxy listen: %v", err)
	}
	go func() {
		for {
			conn, err := ln.Accept()
			if err != nil {
				return
			}
			go handleConn(conn.(*net.TCPConn))
		}
	}()
	t.Cleanup(func() { ln.Close() })
	return ln.Addr().String()
}

func TestProxy_EndToEndEcho(t *testing.T) {
	backendAddr := startEchoServer(t)
	setBackend(t, backendAddr)
	proxyAddr := startProxyListener(t)

	conn, err := net.Dial("tcp", proxyAddr)
	if err != nil {
		t.Fatalf("dial proxy: %v", err)
	}
	defer conn.Close()

	msg := []byte("hello gopf proxy!")
	if _, err := conn.Write(msg); err != nil {
		t.Fatalf("write: %v", err)
	}
	if tcpConn, ok := conn.(*net.TCPConn); ok {
		tcpConn.CloseWrite()
	}

	conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	buf := make([]byte, len(msg))
	if _, err := io.ReadFull(conn, buf); err != nil {
		t.Fatalf("read: %v", err)
	}
	if string(buf) != string(msg) {
		t.Errorf("got %q, want %q", string(buf), string(msg))
	}
}

func TestProxy_LargeData(t *testing.T) {
	backendAddr := startEchoServer(t)
	setBackend(t, backendAddr)
	proxyAddr := startProxyListener(t)

	conn, err := net.Dial("tcp", proxyAddr)
	if err != nil {
		t.Fatalf("dial proxy: %v", err)
	}
	defer conn.Close()

	// 1 MB payload with a repeating pattern.
	const size = 1024 * 1024
	msg := make([]byte, size)
	for i := range msg {
		msg[i] = byte(i % 256)
	}

	if _, err := conn.Write(msg); err != nil {
		t.Fatalf("write: %v", err)
	}
	if tcpConn, ok := conn.(*net.TCPConn); ok {
		tcpConn.CloseWrite()
	}

	conn.SetReadDeadline(time.Now().Add(15 * time.Second))
	buf := make([]byte, size)
	if _, err := io.ReadFull(conn, buf); err != nil {
		t.Fatalf("read: %v", err)
	}
	for i := range msg {
		if buf[i] != msg[i] {
			t.Fatalf("byte %d mismatch: got %d, want %d", i, buf[i], msg[i])
		}
	}
}

func TestProxy_MultipleConcurrentConnections(t *testing.T) {
	backendAddr := startEchoServer(t)
	setBackend(t, backendAddr)
	proxyAddr := startProxyListener(t)

	const N = 50
	var wg sync.WaitGroup
	wg.Add(N)
	errs := make(chan error, N)

	for i := 0; i < N; i++ {
		go func(idx int) {
			defer wg.Done()
			conn, err := net.Dial("tcp", proxyAddr)
			if err != nil {
				errs <- fmt.Errorf("conn %d dial: %w", idx, err)
				return
			}
			defer conn.Close()

			msg := []byte(fmt.Sprintf("message-%d", idx))
			if _, err := conn.Write(msg); err != nil {
				errs <- fmt.Errorf("conn %d write: %w", idx, err)
				return
			}
			if tcpConn, ok := conn.(*net.TCPConn); ok {
				tcpConn.CloseWrite()
			}

			conn.SetReadDeadline(time.Now().Add(5 * time.Second))
			buf := make([]byte, len(msg))
			if _, err := io.ReadFull(conn, buf); err != nil {
				errs <- fmt.Errorf("conn %d read: %w", idx, err)
				return
			}
			if string(buf) != string(msg) {
				errs <- fmt.Errorf("conn %d got %q, want %q", idx, string(buf), string(msg))
			}
		}(i)
	}
	wg.Wait()
	close(errs)

	for err := range errs {
		t.Error(err)
	}
}

func TestProxy_BackendUnreachable(t *testing.T) {
	// Port 1 requires root to bind, so connection is refused immediately.
	setBackend(t, "127.0.0.1:1")
	proxyAddr := startProxyListener(t)

	conn, err := net.Dial("tcp", proxyAddr)
	if err != nil {
		t.Fatalf("dial proxy: %v", err)
	}
	defer conn.Close()

	// Proxy should close the connection after failing to reach the backend.
	conn.SetReadDeadline(time.Now().Add(10 * time.Second))
	buf := make([]byte, 10)
	n, err := conn.Read(buf)
	if err == nil && n > 0 {
		t.Errorf("expected connection close (EOF), got %d bytes", n)
	}
}

func TestProxy_MultipleBackendsConfigured(t *testing.T) {
	backend1 := startEchoServer(t)
	backend2 := startEchoServer(t)
	setBackend(t, backend1, backend2)
	proxyAddr := startProxyListener(t)

	const N = 20
	for i := 0; i < N; i++ {
		conn, err := net.Dial("tcp", proxyAddr)
		if err != nil {
			t.Fatalf("iteration %d dial: %v", i, err)
		}
		msg := []byte(fmt.Sprintf("test-%d", i))
		if _, err := conn.Write(msg); err != nil {
			t.Fatalf("iteration %d write: %v", i, err)
		}
		if tcpConn, ok := conn.(*net.TCPConn); ok {
			tcpConn.CloseWrite()
		}
		conn.SetReadDeadline(time.Now().Add(5 * time.Second))
		buf := make([]byte, len(msg))
		if _, err := io.ReadFull(conn, buf); err != nil {
			conn.Close()
			t.Fatalf("iteration %d read: %v", i, err)
		}
		conn.Close()
		if string(buf) != string(msg) {
			t.Errorf("iteration %d got %q, want %q", i, string(buf), string(msg))
		}
	}
}

// TestProxy_NoBackendConfigured verifies that when weight is zero (no usable
// backends), the proxy closes the client connection cleanly.
func TestProxy_NoBackendConfigured(t *testing.T) {
	backendMu.Lock()
	options.backend = Backend{Hosts: nil, weight: 0}
	backendMu.Unlock()
	proxyAddr := startProxyListener(t)

	conn, err := net.Dial("tcp", proxyAddr)
	if err != nil {
		t.Fatalf("dial proxy: %v", err)
	}
	defer conn.Close()

	conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	buf := make([]byte, 10)
	n, err := conn.Read(buf)
	if err == nil && n > 0 {
		t.Errorf("expected connection close, got %d bytes", n)
	}
}
