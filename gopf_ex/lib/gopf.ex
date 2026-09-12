defmodule Gopf do
  @moduledoc """
  Gopf — a lightweight TCP layer-4 load balancer written in Elixir.

  This is an Elixir port of the original Go `gopf` proxy. It forwards TCP
  connections to a weighted set of backend hosts, with hot-config reload via
  SIGHUP.

  ## Architecture

  - `Gopf.Backend` — backend config struct and weighted random host selection
  - `Gopf.ConfigLoader` — JSON config file parsing (OTP 27+ `:json`)
  - `Gopf.BackendStore` — Agent holding the active backend, supports hot reload
  - `Gopf.Proxy` — TCP listener, per-connection tasks, bidirectional forwarding
  - `Gopf.CLI` — command-line entry point (escript)

  ## Usage

      mix escript.build
      ./bin/gopf_ex --listen 1248 settings.conf
  """
end
