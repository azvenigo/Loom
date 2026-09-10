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
- **Three front doors, one core.** REST, MCP (13 tools over Streamable HTTP), and an embedded
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
`mcptest` (220 assertions total).

**Linux:** builds and runs. The gcc `-Wall -Wextra -Werror` path and the POSIX branch of the
fsync/rename logic in `persist/` are both exercised there.

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
- **Duplicate detection on create.** `POST /jots` and `loom_add` run the new record's name and
  summary back through the ranker and answer with `duplicate_candidates` — the existing memories
  that score close to it, measured as a fraction of what the new record itself scored, so the
  threshold means the same thing in a store of sixty memories and one of sixty thousand. The MCP
  server's first instruction is "search before writing"; this is that rule being checked. It never
  blocks the write.
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

### Installing it as a service

`packaging/` has install, update and uninstall for systemd and for Windows, with the same shape on
both: settings live in a config file the update path is not allowed to write, the old binary is kept
as `loom.prev`, and the update waits on a real health check — `GET /stats`, which only answers once
the store is loaded and serving — then rolls back if it fails. See
[packaging/README.md](packaging/README.md).

```
sudo packaging/install.sh          # then: sudo packaging/update.sh
```

### Undo and restore

Every mutation is also appended to `DIR/loom.history` — which, unlike the WAL, is **never truncated
by a snapshot**. Because a journal put already carries the complete record, the previous entry for a
jot *is* its before-image, so restoring is just re-applying a line that is already in the log.

- `GET /history?limit=&offset=&id=` — every change, newest first.
- `POST /history/restore` `{"seq":N}` — put that version back. A `del` entry restores whatever was in
  force immediately before it, which is what "undo this delete" means.
- `POST /history/restore` `{"txn":N,"undo":true}` — undo a whole **multi-record operation**. See
  below.
- The dashboard's **History** view is the same thing with buttons.
- `loom_history` and `loom_restore` are the same two over MCP. An agent is the writer most likely to
  need an undo, and for a while it was the only client that did not have one — the dashboard could
  repair a bad agent write and the agent that made it had to ask a human.

**A patch that changes nothing is not a mutation.** Front ends send whole records and whole tag
arrays rather than diffs — the dashboard's Save, its snooze buttons, an agent re-asserting a memory
it already wrote — so re-submitting an unchanged state is the common case, not an edge one. Those
calls still succeed and still return the record, but nothing is written: no `updated` bump (so other
agents' `expect_updated` tokens stay valid), no WAL line, and no history point. The response carries
`"no_change": true` when that happened.

The id is kept (so links to the jot survive, and a deleted jot comes back at the address others still
reference) but `updated` is stamped now, because the restore is a change and it happened now. A
restore is refused if the slug has since been taken by a different jot.

**One act that touches many jots is logged as one act.** `POST /tags/merge` rewrites every jot
carrying a retired tag, and each rewrite lands in the log as its own entry — so undoing a merge
across forty jots used to mean finding and restoring forty of them, which is why `loom_merge_tags`
called itself irreversible. Every entry a multi-record operation produces now carries a shared
**transaction id**, and the merge returns it as `txn`:

```
POST /tags/merge      {"from":["looom"],"to":"loom"}   ->  {"changed":12,"txn":481}
POST /history/restore {"txn":481,"undo":true}          ->  puts all 12 back as they were
POST /history/restore {"txn":481}                      ->  re-applies it instead
```

The id needs no counter and no extra durability: it *is* the sequence number of the group's first
entry, and because the store holds its write lock for the whole operation, a group is a contiguous
run of sequence numbers. Each jot is attempted and reported independently — one that has been edited
or renamed since is refused on its own rather than failing the whole undo, and the response names
it. The History view shows the group as a single row that expands.

**Writes carry a server-stamped origin.** `editor` is what a writer *calls itself* and anything can
claim to be anyone; alongside it, Loom now records the address the connection actually arrived on,
on the jot and on every history entry. So the record reads "claude wrote this, from 192.168.1.30"
rather than only the unverifiable half. It is never read from a request body — the codec deliberately
ignores an `origin` key — and it is not part of a record's content, so re-asserting an unchanged
memory from a second machine is still a no-op rather than a write.

**Every jot records what last happened to it.** Alongside *when* it changed, *who* changed it and
*where from*, a jot now carries *what the change was*: `added`, `done`, `reopened`, `scheduled`,
`snoozed`, `rescheduled`, `unscheduled`, `updated`, `retagged` or `restored`. It is classified
inside the store's write lock, where both versions of the record exist, and stored on the record —
because every kind except `added` is a statement about a *difference*, and nothing reading a jot on
its own can recover it. A jot carrying `due:2026-09-20` cannot tell you whether that date was just
set or just pushed back a week.

The history log does hold before-images, but it is bounded and rotated by size: it answers "what
changed this week", not "what last happened to this jot in March". Keeping the one-byte answer on
the record is what makes the question survive rotation. Like `origin` it is server-derived and the
codec reads no `last_change` key from a request body, and like `origin` it is not part of a record's
content — so re-asserting an unchanged memory neither writes nor overwrites the label, and a todo
finished on Monday still reads `done` after any number of no-op saves. The field is optional and
absent on everything written before it existed, which is why adding it needed no migration; the
dashboard's **Recently changed** list shows it as a label down the left of every row.

**TODOs carry who they are for.** `for:human` and `for:agent` say who should *do* a piece of open
work, which is a different question from `source:` — the machine a jot was *written* from. The
dashboard's TODO panel filters on it, and a todo with neither tag stays unrouted and visible under
**All** rather than being defaulted into either bucket, so an untriaged pile shows up as the gap
between All and the other two counts.

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
JSONL logs, a runtime address allow list, an append-only history log with per-jot restore,
transaction-grouped undo for multi-record operations, server-stamped write origins, a per-jot
record of what the last change actually was, duplicate detection on create, an offline purge, and service packaging with health-checked updates and
rollback, on both Windows and Linux. Not yet built: a design for backing a shared
markdown-based memory store (files-as-source-of-truth, offline reconcile, conflict review) sketched
but not implemented.

## License

MIT. See [LICENSE](LICENSE) — vendored third-party code under `vendor/` keeps its own license.
