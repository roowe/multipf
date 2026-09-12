defmodule Gopf.BackendStore do
  @moduledoc """
  Agent that holds the current active backend configuration.

  Allows hot-reload without restarting the proxy. Connection handlers
  read the latest backend on every new connection.
  """

  use Agent

  @doc "Start the store with an initial backend."
  @spec start_link(Gopf.Backend.t()) :: Agent.on_start()
  def start_link(backend) do
    Agent.start_link(fn -> backend end, name: __MODULE__)
  end

  @doc "Get the current backend."
  @spec get() :: Gopf.Backend.t()
  def get do
    Agent.get(__MODULE__, & &1)
  end

  @doc "Replace the current backend (used by config reload)."
  @spec update(Gopf.Backend.t()) :: :ok
  def update(backend) do
    Agent.update(__MODULE__, fn _ -> backend end)
  end
end
