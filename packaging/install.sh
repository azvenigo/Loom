#!/usr/bin/env bash
# Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.
#
# First-time install of loom as a systemd service.
#
# IT IS SAFE TO RE-RUN. Everything it creates is created only if absent - the user, the data
# directory, the config file - so running it against an existing install upgrades the binary and
# the unit and leaves the settings and the data alone. That makes "did I already install this?" a
# question nobody has to answer correctly.
#
#   sudo packaging/install.sh [path/to/loom]
#
# The binary defaults to build/loom relative to the repository root.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BINARY="${1:-$REPO/build/loom}"

PREFIX=/usr/local/bin
CONFDIR=/etc/loom
DATADIR=/var/lib/loom
UNIT=/etc/systemd/system/loom.service
SVCUSER=loom

[[ $EUID -eq 0 ]] || { echo "run as root: sudo $0 $*" >&2; exit 1; }
[[ -x "$BINARY" ]] || { echo "no loom binary at $BINARY - build it first, or pass the path" >&2; exit 1; }
command -v systemctl >/dev/null || { echo "no systemctl - this installer is for systemd hosts" >&2; exit 1; }

# A service account with no login and no home. Loom needs a socket and one directory.
if ! id -u "$SVCUSER" >/dev/null 2>&1; then
    echo "creating service user $SVCUSER"
    useradd --system --no-create-home --shell /usr/sbin/nologin "$SVCUSER"
fi

install -d -o "$SVCUSER" -g "$SVCUSER" -m 0750 "$DATADIR"
install -d -m 0755 "$CONFDIR"

# THE CONFIG IS NEVER OVERWRITTEN. It is the one file here that holds decisions somebody made.
if [[ ! -f "$CONFDIR/loom.conf" ]]; then
    install -m 0640 -o root -g "$SVCUSER" "$REPO/packaging/systemd/loom.conf.example" "$CONFDIR/loom.conf"
    echo "wrote $CONFDIR/loom.conf - edit it before opening this beyond loopback"
else
    echo "keeping existing $CONFDIR/loom.conf"
fi

install -m 0755 "$BINARY" "$PREFIX/loom"
install -m 0644 "$REPO/packaging/systemd/loom.service" "$UNIT"

systemctl daemon-reload
systemctl enable loom
systemctl restart loom

# Wait for the service to answer before claiming success, using the same health check the update
# path uses - an install that reports success while the service is crash-looping is worse than one
# that fails.
source "$REPO/packaging/health.sh"
if loom_wait_healthy 30; then
    echo
    echo "loom is running on $(loom_origin)"
    echo "  config   $CONFDIR/loom.conf"
    echo "  data     $DATADIR"
    echo "  logs     journalctl -u loom -f"
    echo "  update   sudo packaging/update.sh"
else
    echo "loom did not come up healthy - journalctl -u loom -n 50" >&2
    exit 1
fi
