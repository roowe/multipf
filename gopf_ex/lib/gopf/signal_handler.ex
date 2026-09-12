defmodule Gopf.SignalHandler do
  @moduledoc false

  require Logger

  # gen_event handler installed on :erl_signal_server.
  # Handles SIGHUP (config reload) and SIGUSR1 (status dump).

  def install(config_path) do
    :gen_event.add_handler(:erl_signal_server, __MODULE__, config_path)
  end

  # gen_event callbacks
  def init(config_path), do: {:ok, config_path}

  def handle_event(:sighup, config_path) do
    case Gopf.ConfigLoader.load(config_path) do
      {:ok, backend} ->
        Gopf.BackendStore.update(backend)
        Logger.info("reload succeed")

      {:error, reason} ->
        Logger.error("reload failed: #{inspect(reason)}")
    end

    {:ok, config_path}
  end

  def handle_event(:sigusr1, config_path) do
    Logger.info("num processes: #{length(Process.list())}")
    {:ok, config_path}
  end

  def handle_event(_signal, config_path), do: {:ok, config_path}

  def handle_call(_request, state), do: {:ok, :ok, state}
  def handle_info(_info, state), do: {:ok, state}
  def terminate(_reason, _state), do: :ok
  def code_change(_old, state, _extra), do: {:ok, state}
end
