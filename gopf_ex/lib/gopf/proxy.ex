defmodule Gopf.Proxy do
  @moduledoc """
  TCP layer-4 proxy with weighted round-robin backend selection.

  Each incoming connection is handled by a dedicated lightweight process.
  Bidirectional data forwarding uses two tasks (one per direction) with
  TCP half-close support, matching the behaviour of the Go reference
  implementation.

  ## Half-close note

  Erlang's `:gen_tcp` defaults to `exit_on_close: true`, which fully closes
  a socket as soon as the peer sends FIN — breaking TCP half-close. We set
  `exit_on_close: false` on every socket so that after receiving a peer FIN
  we can still transmit pending data in the reverse direction.
  """

  require Logger

  @default_port 1248
  @connect_timeout 5000

  # `exit_on_close: false` is essential for TCP half-close support.
  # `backlog: 4096` prevents connection drops under high concurrency
  # (Erlang's default backlog is only 5).
  @listen_options [:binary, active: false, packet: :raw, reuseaddr: true, exit_on_close: false, backlog: 4096]
  @connect_options [:binary, active: false, packet: :raw, exit_on_close: false]

  @doc """
  Start the proxy listener. Blocks the calling process in the accept loop.
  """
  @spec start(non_neg_integer()) :: no_return()
  def start(port \\ @default_port) do
    case :gen_tcp.listen(port, @listen_options) do
      {:ok, listen_socket} ->
        Logger.info("gopf listening on port #{port}")
        accept_loop(listen_socket)

      {:error, reason} ->
        Logger.error("build listener failed: #{inspect(reason)}")
        System.halt(1)
    end
  end

  defp accept_loop(listen_socket) do
    case :gen_tcp.accept(listen_socket) do
      {:ok, client_socket} ->
        # Spawn the handler first, then transfer socket ownership to it.
        # The handler waits for :ready before touching the socket, avoiding
        # a race between ownership transfer and socket operations.
        pid =
          spawn_link(fn ->
            receive do
              :ready -> handle_connection(client_socket)
            end
          end)

        :ok = :gen_tcp.controlling_process(client_socket, pid)
        send(pid, :ready)
        accept_loop(listen_socket)

      {:error, reason} ->
        Logger.error("accept failed: #{inspect(reason)}")
        accept_loop(listen_socket)
    end
  end

  defp handle_connection(client_socket) do
    backend = Gopf.BackendStore.get()

    case Gopf.Backend.choose(backend) do
      nil ->
        Logger.error("choose host failed")
        :gen_tcp.close(client_socket)

      host ->
        connect_and_forward(client_socket, host)
    end
  end

  defp connect_and_forward(client_socket, host) do
    {upstream_host, upstream_port} = parse_addr(host.addr)

    case :gen_tcp.connect(upstream_host, upstream_port, @connect_options, @connect_timeout) do
      {:ok, upstream_socket} ->
        set_keepalive(client_socket)
        set_keepalive(upstream_socket)
        forward_both(client_socket, upstream_socket)

      {:error, reason} ->
        Logger.error("connect to #{host.addr} failed: #{inspect(reason)}")
        :gen_tcp.close(client_socket)
    end
  end

  defp parse_addr(addr) do
    case String.split(addr, ":", parts: 2) do
      [host, port_str] ->
        {String.to_charlist(host), String.to_integer(port_str)}

      _ ->
        raise ArgumentError, "invalid backend address: #{addr}"
    end
  end

  defp set_keepalive(socket) do
    :inet.setopts(socket, keepalive: true)
  end

  @doc """
  Bidirectional forwarding: spawn one task per direction, wait for both to
  finish, then close both sockets.

  Socket A is the client-side connection; socket B is the upstream (backend)
  connection.
  """
  def forward_both(socket_a, socket_b) do
    task_a_to_b = Task.async(fn -> forward(socket_a, socket_b) end)
    task_b_to_a = Task.async(fn -> forward(socket_b, socket_a) end)

    Task.await(task_a_to_b, :infinity)
    Task.await(task_b_to_a, :infinity)

    :gen_tcp.close(socket_a)
    :gen_tcp.close(socket_b)
  end

  @doc """
  Forward data from `source` to `dest` until `source` closes its write side.

  On source EOF, half-closes `dest`'s write side so the peer knows no more
  data is coming, while the reverse direction may still be transmitting.
  Requires `exit_on_close: false` on both sockets for proper half-close.
  """
  def forward(source, dest) do
    case :gen_tcp.recv(source, 0) do
      {:ok, data} ->
        case :gen_tcp.send(dest, data) do
          :ok -> forward(source, dest)
          {:error, _reason} -> :ok
        end

      {:error, _reason} ->
        # Source closed its write side; signal dest that we are done writing.
        :gen_tcp.shutdown(dest, :write)
        :ok
    end
  end
end
