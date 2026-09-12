ExUnit.start()

# Start the global backend store so tests can use Gopf.BackendStore.update/1
{:ok, _} = Gopf.BackendStore.start_link(%Gopf.Backend{hosts: [], weight: 0})
