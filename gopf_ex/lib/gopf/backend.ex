defmodule Gopf.Backend do
  @moduledoc """
  Backend configuration and weighted random host selection.
  """

  defmodule Host do
    @moduledoc false
    defstruct [:addr, :weight]
  end

  defstruct [:traffic_url, hosts: [], weight: 0]

  @type t :: %__MODULE__{
          traffic_url: String.t() | nil,
          hosts: [Host.t()],
          weight: non_neg_integer()
        }

  @doc """
  Build a Backend from a decoded JSON map.

  Expects keys `"Hosts"` (list of `%{"Addr" => ..., "Weight" => ...}`)
  and optionally `"TrafficUrl"`.
  """
  @spec from_map(map()) :: t()
  def from_map(data) do
    hosts_raw = Map.get(data, "Hosts", [])

    hosts =
      Enum.map(hosts_raw, fn h ->
        %Host{
          addr: Map.get(h, "Addr"),
          weight: Map.get(h, "Weight", 0)
        }
      end)

    total_weight = Enum.reduce(hosts, 0, fn h, acc -> acc + h.weight end)

    %__MODULE__{
      traffic_url: Map.get(data, "TrafficUrl"),
      hosts: hosts,
      weight: total_weight
    }
  end

  @doc """
  Select a host by weighted random choice.

  Returns `nil` when total weight is <= 0 or hosts is empty.
  """
  @spec choose(t()) :: Host.t() | nil
  def choose(%__MODULE__{weight: weight}) when weight <= 0, do: nil
  def choose(%__MODULE__{weight: weight, hosts: hosts}) do
    # :rand.uniform returns 1..weight; subtract 1 to get 0..weight-1
    v = :rand.uniform(weight) - 1
    do_choose(hosts, v)
  end

  defp do_choose([], _v), do: nil

  defp do_choose([host | rest], v) do
    if v < host.weight do
      host
    else
      do_choose(rest, v - host.weight)
    end
  end
end
