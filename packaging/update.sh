#!/usr/bin/env bash
# Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.
#
# Replace a running loom with a new build, and put the old one back if the new one does not come up.
#
#   sudo packaging/update.sh [path/to/loom]
#
# THE ORDER IS THE WHOLE POINT, and it is the part most update scripts get wrong:
#
#   1. Verify the new binary runs AT ALL, before anything is stopped. A --help that fails is a bad
#      download or a wrong architecture, and finding that out after the service is down is a
#      needless outage.
#   2. Stop the service and WAIT for it to actually be gone. loom takes an exclusive lock on its
#      data directory for its whole life (persist/DataLock.h), so a replacement started while the
#      old process still holds it exits immediately rather than corrupting anything - which turns
#      "stop then start too fast" from silent data loss into a refusal, but a refusal is still an
#      outage. systemctl stop is synchronous, so this is really a guard against a process that
#      ignored SIGTERM.
#   3. Keep the old binary as loom.prev BEFORE overwriting.
#   4. Start, then wait on a health check that reads /stats - not just a listening socket.
#   5. If that check fails, put loom.prev back and start it again. The data directory is never
#      touched by any of this, so a rollback is genuinely a rollback.
#
# The config file is not read, written or consulted for anything but the health check's address.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NEW="${1:-$REPO/build/loom}"
PREFIX=/usr/local/bin
UNIT=/etc/systemd/system/loom.service

[[ $EUID -eq 0 ]] || { echo "run as root: sudo $0 $*" >&2; exit 1; }
[[ -x "$NEW" ]] || { echo "no loom binary at $NEW" >&2; exit 1; }
[[ -x "$PREFIX/loom" ]] || { echo "no installed loom at $PREFIX/loom - run install.sh first" >&2; exit 1; }

source "$REPO/packaging/health.sh"

echo "checking the new binary runs..."
"$NEW" --help >/dev/null || { echo "the new binary does not run - nothing has been changed" >&2; exit 1; }

if cmp -s "$NEW" "$PREFIX/loom"; then
    echo "already running this exact binary - nothing to do"
    exit 0
fi

echo "stopping loom..."
systemctl stop loom
for _ in $(seq 1 30); do
    systemctl is-active --quiet loom || break
    sleep 1
done
if systemctl is-active --quiet loom; then
    echo "loom did not stop - refusing to replace a binary that is still executing" >&2
    exit 1
fi

echo "keeping the current binary as $PREFIX/loom.prev"
cp -p "$PREFIX/loom" "$PREFIX/loom.prev"

install -m 0755 "$NEW" "$PREFIX/loom"
install -m 0644 "$REPO/packaging/systemd/loom.service" "$UNIT"
systemctl daemon-reload

echo "starting loom..."
systemctl start loom || true

if loom_wait_healthy 30; then
    echo "update complete - the previous binary is at $PREFIX/loom.prev"
    exit 0
fi

echo "the new build did not come up healthy - ROLLING BACK" >&2
journalctl -u loom -n 30 --no-pager >&2 || true

systemctl stop loom || true
install -m 0755 "$PREFIX/loom.prev" "$PREFIX/loom"
systemctl start loom || true

if loom_wait_healthy 30; then
    echo "rolled back; the previous build is running again. The failed binary was NOT kept." >&2
else
    echo "ROLLBACK ALSO FAILED - loom is down. journalctl -u loom -n 50" >&2
fi
exit 1
