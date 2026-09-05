# Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.
#
# The health check, shared by install and update so there is exactly one definition of "up".
# Sourced, not executed.
#
# WHAT COUNTS AS HEALTHY. /health answers as soon as the socket is listening, which is necessary but
# nowhere near sufficient: the interesting failure is a service that binds, then finds its snapshot
# unreadable or its data directory locked by a process that has not exited yet. So this asks
# /stats, which only answers once the store is loaded and serving, and reads the jot count out of
# it - a number that a half-started process cannot produce.

loom_conf() { sed -n "s/^$1=//p" /etc/loom/loom.conf 2>/dev/null | tail -1; }

loom_origin() {
    local bind port
    bind="$(loom_conf LOOM_BIND)"; port="$(loom_conf LOOM_PORT)"
    # A wildcard bind is not an address anything can connect to; loopback always reaches it.
    [[ -z "$bind" || "$bind" == "0.0.0.0" || "$bind" == "::" ]] && bind=127.0.0.1
    echo "http://${bind}:${port:-7700}"
}

# Echoes the store's jot count, or nothing if it is not serving.
loom_jot_count() {
    local body
    body="$(curl -fsS --max-time 3 "$(loom_origin)/stats" 2>/dev/null)" || return 1
    # No jq dependency - one integer out of a flat JSON object is not worth one.
    sed -n 's/.*"jots":\([0-9]*\).*/\1/p' <<<"$body"
}

# loom_wait_healthy <seconds>
loom_wait_healthy() {
    local deadline=$(( SECONDS + ${1:-30} )) n
    while (( SECONDS < deadline )); do
        if n="$(loom_jot_count)" && [[ -n "$n" ]]; then
            echo "healthy: serving $n jots"
            return 0
        fi
        sleep 1
    done
    return 1
}
