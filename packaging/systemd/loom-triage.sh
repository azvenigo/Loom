#!/usr/bin/env bash
# loom-triage - invoked directly by Loom's own Watcher thread (persist/Watcher.h) when there is
# unprocessed work AND every guardrail (cooldown, hourly cap, 24h spend cap, circuit breaker)
# allows it. NOT scheduled by cron/systemd/a timer of any kind any more - Watcher polls watched
# files and the store itself every --watch-interval seconds and decides when to run this, in
# process, with no model call spent on the decision. See main.cpp's --on-new-jots.
#
# ARGUMENTS: one Loom jot id per argument, decimal, already selected and bounded (capped by
# --triage-max-ids) by Watcher - this script/prompt touches ONLY these ids, nothing else in the
# store, on purpose (see loom-triage-prompt.txt).
#
# Runs as the OWNING HUMAN (alex), not a dedicated service account: `claude` reuses alex's own
# Claude Pro OAuth login (~/.claude/.credentials.json), which a dedicated `loom` service user would
# not have. Watcher execs this with its own PATH, which under systemd does NOT include npm's global
# bin dir the way an interactive shell's does (confirmed live: exec fails with "claude: not found"
# without this) - resolved once, here, rather than in the systemd unit, so this still works when
# run by hand for testing.
#
# --output-format json rather than plain text: Watcher parses total_cost_usd, duration_api_ms and
# usage.* straight out of this on stdout to populate the run ledger (GET /triage/runs, the
# dashboard's Triage usage card) - see ParseClaudeJson in Watcher.cpp. Do not change this to
# anything else without updating that parser to match.
#
# --mcp-config + --strict-mcp-config rather than relying on `cd`-into-the-repo for project-scoped
# MCP config: this should not depend on a dev checkout staying at a fixed path, and
# --strict-mcp-config keeps this run from picking up whatever else happens to be configured for
# alex's other projects.
set -euo pipefail

export PATH="/home/alex/.npm-global/bin:$PATH"

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ "$#" -eq 0 ]; then
    echo "usage: $0 <jot-id> [jot-id ...]" >&2
    exit 2
fi

PROMPT="$(cat "$DIR/loom-triage-prompt.txt")
Ids for this run: $*
Current local time (use this for \"ingestion\"/\"now\"/relative due dates - triage runs shortly
after a jot is created, so this is close enough): $(date '+%Y-%m-%d %H:%M')"

exec claude -p "$PROMPT" \
    --model haiku \
    --output-format json \
    --mcp-config "$DIR/loom-triage-mcp.json" \
    --strict-mcp-config \
    --tools ""   # this task only ever calls loom_* MCP tools - Code's own built-in belt
                 # (Bash/Read/Edit/...) would otherwise still be loaded into context and paid
                 # for on every single invocation for zero benefit. Measured ~40% fewer cached
                 # tokens per run with this on.
