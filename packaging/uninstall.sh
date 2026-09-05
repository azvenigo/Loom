#!/usr/bin/env bash
# Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.
#
# Remove the service. THE DATA DIRECTORY AND THE CONFIG ARE LEFT ALONE - uninstalling a service is
# not a request to erase what it was holding, and /var/lib/loom is somebody's memory. Removing that
# is a separate, deliberate act:  rm -rf /var/lib/loom /etc/loom
set -euo pipefail
[[ $EUID -eq 0 ]] || { echo "run as root: sudo $0" >&2; exit 1; }

systemctl stop loom    2>/dev/null || true
systemctl disable loom 2>/dev/null || true
rm -f /etc/systemd/system/loom.service
systemctl daemon-reload
rm -f /usr/local/bin/loom /usr/local/bin/loom.prev

echo "service removed. Data is still in /var/lib/loom and settings in /etc/loom."
