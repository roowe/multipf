defmodule Gopf.ConfigLoaderTest do
  use ExUnit.Case, async: true

  alias Gopf.ConfigLoader

  defp write_temp_config(content) do
    path = Path.join(System.tmp_dir!(), "gopf_test_#{:erlang.unique_integer([:positive])}.conf")
    File.write!(path, content)
    on_exit(fn -> File.rm(path) end)
    path
  end

  test "load valid config returns ok backend" do
    content = ~s({
      "Hosts": [
        {"Addr": "127.0.0.1:8080", "Weight": 100},
        {"Addr": "127.0.0.1:8081", "Weight": 200}
      ]
    })

    path = write_temp_config(content)
    {:ok, backend} = ConfigLoader.load(path)

    assert length(backend.hosts) == 2
    assert backend.weight == 300
  end

  test "load nonexistent file returns error" do
    assert {:error, _} = ConfigLoader.load("/nonexistent/path/settings.conf")
  end

  test "load invalid json returns error" do
    path = write_temp_config("{invalid json")
    assert {:error, _} = ConfigLoader.load(path)
  end

  test "load config with empty hosts" do
    path = write_temp_config(~s({"Hosts": []}))
    {:ok, backend} = ConfigLoader.load(path)
    assert backend.hosts == []
    assert backend.weight == 0
  end

  test "load config accumulates weights correctly" do
    content = ~s({
      "Hosts": [
        {"Addr": "h1", "Weight": 10},
        {"Addr": "h2", "Weight": 20},
        {"Addr": "h3", "Weight": 30},
        {"Addr": "h4", "Weight": 40}
      ]
    })

    path = write_temp_config(content)
    {:ok, backend} = ConfigLoader.load(path)
    assert backend.weight == 100
  end

  test "load config with single host" do
    content = ~s({"Hosts": [{"Addr": "127.0.0.1:80", "Weight": 100}]})
    path = write_temp_config(content)
    {:ok, backend} = ConfigLoader.load(path)

    assert length(backend.hosts) == 1
    assert backend.weight == 100
    assert hd(backend.hosts).addr == "127.0.0.1:80"
  end
end
