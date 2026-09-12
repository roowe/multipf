defmodule Gopf.CLI do
  @moduledoc """
  Command-line entry point for the gopf proxy.

  Usage: gopf_ex [--listen <port>] <config_file>
  """

  @default_port 1248

  def main(args) do
    {opts, positional, _} =
      OptionParser.parse(args,
        switches: [listen: :string],
        aliases: [l: :listen]
      )

    port =
      case opts[:listen] do
        nil -> @default_port
        str -> String.to_integer(str)
      end

    case positional do
      [config_path | _] ->
        run(config_path, port)

      [] ->
        IO.puts(:stderr, "usage: gopf_ex [--listen <port>] <config_file>")
        System.halt(1)
    end
  end

  defp run(config_path, port) do
    case Gopf.ConfigLoader.load(config_path) do
      {:ok, backend} ->
        {:ok, _} = Gopf.BackendStore.start_link(backend)
        Gopf.SignalHandler.install(config_path)
        Gopf.Proxy.start(port)

      {:error, reason} ->
        IO.puts(:stderr, "load config failed: #{inspect(reason)}")
        System.halt(1)
    end
  end
end
