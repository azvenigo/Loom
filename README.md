# Loom

<img src="docs/icon.png" width="120" align="right" alt="">

A lightweight in-RAM service for "jots" — short structured notes shared between a person and
several AI agents. Every jot lives in memory for fast ranked search, with a write-ahead log and
periodic snapshot so nothing is lost on restart. Talk to it over REST, over MCP, or through the
built-in dashboard.

```
{"id":1756661962123456,"text":"Ordered 50ft of ethernet cable today"}
```

A bare note like that is all a jot has to be. It can optionally grow a stable `name` (a slug, for
addressing it later), a `summary` (weighted above the body in search), `tags`, and `links` to other
jots — which is what turns a pile of notes into a small durable memory store.

## Why

- **Fast.** An inverted index over summary and body, ranked with BM25 plus a recency multiplier.
  ~20-100µs per query at realistic scale.
- **Durable.** Every write is queued to a JSONL write-ahead log inside the same lock that applied
  it, so log order always matches apply order. Snapshots are written tmp → fsync → rename →
  truncate, so a crash mid-write never corrupts anything on disk.
- **Multi-agent.** Optimistic concurrency (`expect_updated`) means two writers editing the same jot
  get a conflict instead of a silent overwrite. Tag near-duplicates ("infra" vs "infrastructure")
  are flagged in the write response, not silently allowed to fragment the vocabulary.
- **Three front doors, one core.** REST, MCP (11 tools over Streamable HTTP), and an embedded
  dashboard all go through the same operations layer, so they can never drift apart.

## Building

Requires a C++20 compiler. Vendors its dependencies under `vendor/` (Crow, standalone asio,
nlohmann/json) — no package manager needed.

**CMake:**
```
cmake -B build && cmake --build build --config Release
ctest --test-dir build
```

**Windows, no CMake:** run `build-direct.bat` (edit `VSDIR` at the top if Visual Studio is
installed somewhere other than the default path). Output lands in `build-direct/`.

Targets: `loom` (the service), `loombench` (in-process benchmark), `coretest` / `persisttest` /
`mcptest` (144 assertions total).

**Linux:** not yet verified. The gcc `-Wall -Wextra -Werror` path and the POSIX branch of the
fsync/rename logic in `persist/` have not been exercised — treat as untested until proven.

## Running

```
loom --port=7700 --data=./data
```

- Dashboard: `http://127.0.0.1:7700/`
- REST: `GET/POST /jots`, `GET /tags`, `GET /tags/similar`, `POST /tags/merge`, `GET /stats`, …
- MCP: `POST /mcp` (Streamable HTTP). Connect an agent with:
  ```
  claude mcp add --transport http loom http://127.0.0.1:7700/mcp
  ```
- `--seed` populates a few sample jots, only if the store loads empty.
- `--bind=0.0.0.0` to listen beyond loopback — pair it with `--token=SECRET` unless the network is
  fully trusted. Default bind is `127.0.0.1`.
- **Address allow list.** `GET /acl` and `PUT /acl`, or the shield beside the connection state at
  the foot of the dashboard's rail. When enabled, Loom answers only the listed addresses (exact or
  CIDR, v4 and v6) and refuses everything else — dashboard, REST, MCP and `/health` alike. It
  composes with `--token` rather than replacing it. **Loopback is always allowed and cannot be
  removed**, so a list that locks out the network can always be repaired from the machine itself;
  `PUT /acl` additionally refuses a list excluding the caller unless you pass `?force=1`. Stored in
  `DIR/loom.acl.json`.

Run `loom --help` for the full flag list.

### Undo and restore

Every mutation is also appended to `DIR/loom.history` — which, unlike the WAL, is **never truncated
by a snapshot**. Because a journal put already carries the complete record, the previous entry for a
jot *is* its before-image, so restoring is just re-applying a line that is already in the log.

- `GET /history?limit=&offset=&id=` — every change, newest first.
- `POST /history/restore` `{"seq":N}` — put that version back. A `del` entry restores whatever was in
  force immediately before it, which is what "undo this delete" means.
- The dashboard's **History** view is the same thing with buttons.

**A patch that changes nothing is not a mutation.** Front ends send whole records and whole tag
arrays rather than diffs — the dashboard's Save, its snooze buttons, an agent re-asserting a memory
it already wrote — so re-submitting an unchanged state is the common case, not an edge one. Those
calls still succeed and still return the record, but nothing is written: no `updated` bump (so other
agents' `expect_updated` tokens stay valid), no WAL line, and no history point. The response carries
`"no_change": true` when that happened.

The id is kept (so links to the jot survive, and a deleted jot comes back at the address others still
reference) but `updated` is stamped now, because the restore is a change and it happened now. A
restore is refused if the slug has since been taken by a different jot.

### Purge

`DELETE /jots/<id>` removes a jot from RAM and appends a tombstone. The text is still in the
snapshot, still in the WAL above the tombstone, and now also in the history log — which exists
precisely so deletions can be undone. If what you wrote down was a credential, "deleted" is not a
description of where it is.

Purge is the other thing, and it is deliberately a two-step procedure:

1. `POST /purge/request` `{"ids":[...],"reason":"..."}` — or the History view — writes
   `DIR/loom.purge-request.json` naming the jots, why, and what they looked like at the time.
   **Nothing is erased.** `DELETE /purge/request` cancels it.
2. `loom --purge=DIR` does the work with the service **stopped**. On its own it is a dry run; add
   `--yes` to erase. It rewrites the snapshot, empties the WAL and scrubs both history generations.

It cannot run inside a live server — the snapshot, WAL and history log are all open and being
appended to, and there is no correct ordering for rewriting them underneath that. The interlock is
the data lock itself, so a purge attempted against a running instance refuses rather than corrupting
anything. The request carries labels captured *before* the purge so that "yes, those ones" stays
answerable after the content is gone, which is the point of the confirmation step.

## Status

Working: core store, persistence, REST, MCP, dashboard, an importer for simple `{"ts","entry"}`
JSONL logs, a runtime address allow list, and an append-only history log with per-jot restore and
an offline purge. Not yet built: a Linux build, and a design for backing a shared markdown-based memory
store (files-as-source-of-truth, offline reconcile, conflict review) sketched but not implemented.

## License

MIT. See [LICENSE](LICENSE) — vendored third-party code under `vendor/` keeps its own license.
