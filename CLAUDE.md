# CLAUDE.md

Loaded into every Claude Code session in this repo. Keep it short and generic: this repo is public.

## Build and test

- Configure + build: `cmake -B build && cmake --build build -j`
- Tests: `ctest --test-dir build` (coretest, persisttest, mcptest)
- Linux builds with gcc `-Wall -Wextra -Werror`; a warning is a broken build. Check both gcc and
  MSVC-sensitive code paths when touching `persist/` (POSIX vs Win32 fsync/rename branches).
- Windows without CMake: `build-direct.bat`.

## Layout

- `core/` store, query, tags · `persist/` WAL, snapshot, history · `codec/` JSON
- `http/` REST routes (Crow) · `mcp/` MCP tool surface · `web/Dashboard.h` the dashboard, embedded
  in the binary, so a dashboard edit only shows after a rebuild and restart
- `jot/` the small POST-a-jot sidecar · `packaging/` install/update scripts · `vendor/` deps

## Rules

- Public repo: no personal data, machine names, LAN IPs or personal paths in commits. Use neutral
  placeholders. `docs/PLAN.md` and `.claude/` are gitignored on purpose.
- Keep the REST and MCP surfaces in step: a new query capability usually belongs in both, and in
  README.md's endpoint list.
- Match the surrounding C++ style; no new dependencies without asking.

Machine-specific deploy instructions, if any, live in `CLAUDE.local.md` (gitignored).
