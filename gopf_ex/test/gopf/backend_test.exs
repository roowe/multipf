defmodule Gopf.BackendTest do
  use ExUnit.Case, async: true

  alias Gopf.Backend
  alias Gopf.Backend.Host

  # -------------------------------------------------------------------------
  # from_map/1
  # -------------------------------------------------------------------------

  test "from_map builds backend with correct hosts and total weight" do
    data = %{
      "Hosts" => [
        %{"Addr" => "127.0.0.1:8080", "Weight" => 100},
        %{"Addr" => "127.0.0.1:8081", "Weight" => 200}
      ]
    }

    backend = Backend.from_map(data)

    assert length(backend.hosts) == 2
    assert backend.weight == 300
    assert Enum.at(backend.hosts, 0).addr == "127.0.0.1:8080"
    assert Enum.at(backend.hosts, 0).weight == 100
    assert Enum.at(backend.hosts, 1).addr == "127.0.0.1:8081"
    assert Enum.at(backend.hosts, 1).weight == 200
  end

  test "from_map handles empty hosts list" do
    backend = Backend.from_map(%{"Hosts" => []})
    assert backend.hosts == []
    assert backend.weight == 0
  end

  test "from_map handles missing Hosts key" do
    backend = Backend.from_map(%{})
    assert backend.hosts == []
    assert backend.weight == 0
  end

  test "from_map accumulates weights across many hosts" do
    data = %{
      "Hosts" => [
        %{"Addr" => "h1", "Weight" => 10},
        %{"Addr" => "h2", "Weight" => 20},
        %{"Addr" => "h3", "Weight" => 30},
        %{"Addr" => "h4", "Weight" => 40}
      ]
    }

    backend = Backend.from_map(data)
    assert backend.weight == 100
  end

  test "from_map preserves TrafficUrl" do
    data = %{"Hosts" => [], "TrafficUrl" => "http://example.com"}
    backend = Backend.from_map(data)
    assert backend.traffic_url == "http://example.com"
  end

  # -------------------------------------------------------------------------
  # choose/1
  # -------------------------------------------------------------------------

  test "choose returns nil for zero weight" do
    backend = %Backend{hosts: [%Host{addr: "a", weight: 0}], weight: 0}
    assert Backend.choose(backend) == nil
  end

  test "choose returns nil for negative weight" do
    backend = %Backend{hosts: [], weight: -1}
    assert Backend.choose(backend) == nil
  end

  test "choose returns nil for empty hosts" do
    backend = %Backend{hosts: [], weight: 0}
    assert Backend.choose(backend) == nil
  end

  test "choose single host always returns it" do
    host = %Host{addr: "127.0.0.1:8080", weight: 50}
    backend = %Backend{hosts: [host], weight: 50}

    for _ <- 1..1000 do
      assert Backend.choose(backend) == host
    end
  end

  test "choose weight distribution matches configured ratios" do
    hosts = [
      %Host{addr: "A", weight: 1},
      %Host{addr: "B", weight: 2},
      %Host{addr: "C", weight: 3}
    ]
    backend = %Backend{hosts: hosts, weight: 6}

    n = 60_000
    counts =
      Enum.reduce(1..n, %{"A" => 0, "B" => 0, "C" => 0}, fn _, acc ->
        host = Backend.choose(backend)
        Map.update!(acc, host.addr, &(&1 + 1))
      end)

    expected = %{"A" => 1 / 6, "B" => 2 / 6, "C" => 3 / 6}

    for {addr, exp_ratio} <- expected do
      got_ratio = counts[addr] / n
      diff = got_ratio - exp_ratio

      assert diff > -0.03 and diff < 0.03,
             "host #{addr}: got ratio=#{:erlang.float_to_binary(got_ratio, decimals: 4)}, " <>
               "want ~#{:erlang.float_to_binary(exp_ratio, decimals: 4)} " <>
               "(diff=#{:erlang.float_to_binary(diff, decimals: 4)}, count=#{counts[addr]})"
    end
  end

  test "choose can select every host (regression for loop-variable bug)" do
    hosts = [
      %Host{addr: "A", weight: 1},
      %Host{addr: "B", weight: 1}
    ]
    backend = %Backend{hosts: hosts, weight: 2}

    seen =
      Enum.reduce(1..2000, MapSet.new(), fn _, acc ->
        host = Backend.choose(backend)
        MapSet.put(acc, host.addr)
      end)

    assert MapSet.member?(seen, "A"), "host A was never selected"
    assert MapSet.member?(seen, "B"), "host B was never selected"
  end
end
