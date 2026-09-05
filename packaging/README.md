# Packaging and service install

Loom builds to a single binary that runs in the foreground. These scripts make it a service you can
install, update and roll back without thinking about it.

| | Linux (systemd) | Windows |
|---|---|---|
| install | `sudo packaging/install.sh` | `.\packaging\windows\loom-service.ps1 install` |
| update | `sudo packaging/update.sh` | `.\packaging\windows\loom-service.ps1 update` |
| remove | `sudo packaging/uninstall.sh` | `.\packaging\windows\loom-service.ps1 uninstall` |
| config | `/etc/loom/loom.conf` | `C:\ProgramData\Loom\loom.conf` |
| data | `/var/lib/loom` | `C:\ProgramData\Loom\data` |

Both platforms behave the same way on purpose. Two update paths with different semantics is how one
of them quietly stops being tested.

## What an update actually does

1. **Checks the new binary runs before stopping the old one.** A bad build or a wrong architecture
   is found while the service is still up, not after.
2. **Stops the service and waits for it to be gone.** Loom takes an exclusive lock on its data
   directory for its whole life (`persist/DataLock.h`), so a replacement started too early exits
   rather than interleaving writes into the WAL. That turns the classic "stop, then start too fast"
   corruption into a refusal — but a refusal is still an outage, so the script waits.
3. **Keeps the old binary** as `loom.prev` before overwriting.
4. **Waits on a real health check.** Not a listening socket — `GET /stats`, which only answers once
   the snapshot is loaded and the store is serving, and whose jot count a half-started process
   cannot produce.
5. **Rolls back** to `loom.prev` if that check fails, and says so.

The data directory is never touched by any of this, so a rollback is genuinely a rollback.

## Configuration survives updates

The config file holds every setting — bind address, port, data directory, and an opaque `LOOM_EXTRA`
for anything else you would pass on the command line (`--token=SECRET`, `--sync=always`,
`--threads=N`). The unit file and the service registration carry none of it.

That is the whole reason it is a separate file: an update replaces the binary and the unit
wholesale, and is not permitted to write the config. `install.sh` will create it if it is missing
and never overwrites one that exists, so it is safe to re-run.

## Uninstalling keeps your data

Both uninstallers remove the service and the binary and leave `/var/lib/loom` (or
`C:\ProgramData\Loom\data`) and the config alone. Uninstalling a service is not a request to erase
what it was holding. Deleting the data is a separate, deliberate act.

## Health checking by hand

```
curl -s http://127.0.0.1:7700/stats
```

`/health` answers as soon as the socket is listening, which is necessary but not sufficient — the
interesting failure is a process that binds and then finds its data directory locked or its
snapshot unreadable. `/stats` is the one that proves the store is up.
