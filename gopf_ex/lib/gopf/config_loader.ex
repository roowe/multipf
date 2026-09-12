defmodule Gopf.ConfigLoader do
  @moduledoc """
  Load and parse the JSON backend configuration file.
  """

  @doc """
  Read a JSON config file and return `{:ok, backend}` or `{:error, reason}`.
  """
  @spec load(Path.t()) :: {:ok, Gopf.Backend.t()} | {:error, term()}
  def load(path) do
    with {:ok, content} <- File.read(path),
         {:ok, data} <- decode_json(content) do
      {:ok, Gopf.Backend.from_map(data)}
    end
  end

  defp decode_json(content) do
    # OTP 27+ :json.decode returns the term directly on success and raises
    # on failure, so we normalise to {:ok, term} | {:error, reason}.
    try do
      {:ok, :json.decode(content)}
    rescue
      e in ErlangError -> {:error, e.original}
    end
  end
end
