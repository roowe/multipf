defmodule Gopf.ProxyTest do
  use ExUnit.Case, async: false

  alias Gopf.Backend
  alias Gopf.Backend.Host
  alias Gopf.BackendStore

  # -------------------------------------------------------------------------
  # Helpers
  # -------------------------------------------------------------------------

  defp start_echo_server do
    {:ok, listen_socket} =
      :gen_tcp.listen(0, [:binary, active: false, packet: :raw, reuseaddr: true, exit_on_close: false])

    {:ok, port} = :inet.port(listen_socket)

    pid =
      spawn_link(fn ->
        echo_accept_loop(listen_socket)
      end)

    %{port: port, pid: pid, socket: listen_socket}
  end

  defp echo_accept_loop(listen_socket) do
    case :gen_tcp.accept(listen_socket) do
      {:ok, socket} ->
        spawn_link(fn -> echo_handler(socket) end)
        echo_accept_loop(listen_socket)

      {:error, _} ->
        :ok
    end
  end

  defp echo_handler(socket) do
    case :gen_tcp.recv(socket, 0) do
      {:ok, data} ->
        :gen_tcp.send(socket, data)
        echo_handler(socket)

      {:error, _} ->
        :gen_tcp.close(socket)
    end
  end

  defp pick_free_port do
    {:ok, socket} = :gen_tcp.listen(0, [:binary, active: false, reuseaddr: true])
    {:ok, port} = :inet.port(socket)
    :gen_tcp.close(socket)
    port
  end

  defp start_proxy(port) do
    pid = spawn(fn -> Gopf.Proxy.start(port) end)
    # Give the listener time to bind.
    Process.sleep(200)
    pid
  end

  defp set_backend(addrs) do
    hosts = Enum.map(addrs, fn addr -> %Host{addr: addr, weight: 100} end)
    weight = length(hosts) * 100
    BackendStore.update(%Backend{hosts: hosts, weight: weight})
  end

  defp connect_and_echo(proxy_port, message, timeout \\ 5000) do
    {:ok, socket} =
      :gen_tcp.connect(~c"127.0.0.1", proxy_port, [:binary, active: false, packet: :raw])

    :gen_tcp.send(socket, message)
    :gen_tcp.shutdown(socket, :write)
    result = recv_all(socket, [], timeout)
    :gen_tcp.close(socket)
    result
  end

  defp recv_all(socket, acc, timeout) do
    case :gen_tcp.recv(socket, 0, timeout) do
      {:ok, data} -> recv_all(socket, [data | acc], timeout)
      {:error, _} -> IO.iodata_to_binary(Enum.reverse(acc))
    end
  end

  # -------------------------------------------------------------------------
  # forward/2 and forward_both/2 (unit-level)
  # -------------------------------------------------------------------------

  test "forward relays data source -> dest and exits on source EOF" do
    # Create a socket pair via a listener. exit_on_close: false is required
    # so that closing one direction does not fully close the socket.
    {:ok, listen_socket} =
      :gen_tcp.listen(0, [:binary, active: false, packet: :raw, reuseaddr: true, exit_on_close: false])

    {:ok, port} = :inet.port(listen_socket)

    {:ok, source} =
      :gen_tcp.connect(~c"127.0.0.1", port, [:binary, active: false, packet: :raw, exit_on_close: false])

    {:ok, dest} = :gen_tcp.accept(listen_socket)

    # Switch dest to active mode so the test process receives {:tcp, ...}
    # messages instead of competing with forward on passive recv.
    :inet.setopts(dest, active: true)

    pid = spawn_link(fn -> Gopf.Proxy.forward(source, dest) end)

    # Data sent to source arrives at dest.
    :gen_tcp.send(source, "hello")
    assert_receive {:tcp, ^dest, "hello"}, 2000

    :gen_tcp.send(source, "world")
    assert_receive {:tcp, ^dest, "world"}, 2000

    # Close dest's write side so that source's read side receives EOF.
    # forward/2 is reading from source, so this makes its recv return EOF,
    # which triggers half-close of dest's write side and then forward exits.
    :gen_tcp.shutdown(dest, :write)

    # Wait for the forward process to unwind (it exits after source EOF).
    Process.sleep(100)
    refute Process.alive?(pid)

    :gen_tcp.close(source)
    :gen_tcp.close(dest)
    :gen_tcp.close(listen_socket)
  end

  # -------------------------------------------------------------------------
  # End-to-end through a real proxy listener
  # -------------------------------------------------------------------------

  test "end-to-end echo through proxy" do
    echo = start_echo_server()
    set_backend(["127.0.0.1:#{echo.port}"])

    proxy_port = pick_free_port()
    _proxy_pid = start_proxy(proxy_port)

    result = connect_and_echo(proxy_port, "hello gopf proxy!")
    assert result == "hello gopf proxy!"
  end

  test "large data (1 MB) is forwarded intact" do
    echo = start_echo_server()
    set_backend(["127.0.0.1:#{echo.port}"])

    proxy_port = pick_free_port()
    _proxy_pid = start_proxy(proxy_port)

    size = 1024 * 1024
    message = for i <- 0..(size - 1), into: <<>>, do: <<rem(i, 256)>>

    result = connect_and_echo(proxy_port, message, 15000)
    assert byte_size(result) == size
    assert result == message
  end

  test "multiple concurrent connections all succeed" do
    echo = start_echo_server()
    set_backend(["127.0.0.1:#{echo.port}"])

    proxy_port = pick_free_port()
    _proxy_pid = start_proxy(proxy_port)

    n = 50

    results =
      Enum.map(1..n, fn i ->
        Task.async(fn ->
          message = "message-#{i}"
          {i, connect_and_echo(proxy_port, message)}
        end)
      end)
      |> Enum.map(&Task.await/1)

    for {i, result} <- results do
      assert result == "message-#{i}", "connection #{i} got #{inspect(result)}"
    end
  end

  test "backend unreachable closes client connection" do
    # Port 1 requires root to bind, so connection is refused immediately.
    set_backend(["127.0.0.1:1"])

    proxy_port = pick_free_port()
    _proxy_pid = start_proxy(proxy_port)

    {:ok, socket} =
      :gen_tcp.connect(~c"127.0.0.1", proxy_port, [:binary, active: false, packet: :raw])

    # Proxy should close the connection after failing to reach the backend.
    result = :gen_tcp.recv(socket, 0, 10000)
    assert match?({:error, _}, result)
    :gen_tcp.close(socket)
  end

  test "multiple backends configured — connections succeed" do
    echo1 = start_echo_server()
    echo2 = start_echo_server()
    set_backend(["127.0.0.1:#{echo1.port}", "127.0.0.1:#{echo2.port}"])

    proxy_port = pick_free_port()
    _proxy_pid = start_proxy(proxy_port)

    for i <- 1..20 do
      message = "test-#{i}"
      result = connect_and_echo(proxy_port, message)
      assert result == message
    end
  end

  test "no backend configured (weight 0) closes connection" do
    BackendStore.update(%Backend{hosts: [], weight: 0})

    proxy_port = pick_free_port()
    _proxy_pid = start_proxy(proxy_port)

    {:ok, socket} =
      :gen_tcp.connect(~c"127.0.0.1", proxy_port, [:binary, active: false, packet: :raw])

    result = :gen_tcp.recv(socket, 0, 5000)
    assert match?({:error, _}, result)
    :gen_tcp.close(socket)
  end
end
