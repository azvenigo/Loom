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

Run `loom --help` for the full flag list.

## Status

Working: core store, persistence, REST, MCP, dashboard, an importer for simple `{"ts","entry"}`
JSONL logs. Not yet built: a Linux build, and a design for backing a shared markdown-based memory
store (files-as-source-of-truth, offline reconcile, conflict review) sketched but not implemented.

## License

MIT. See [LICENSE](LICENSE) — vendored third-party code under `vendor/` keeps its own license.
