#!/bin/sh
# Bring up a headless X server for Wine's D2 init (D2WIN_10005 wants a window
# handle even though we never render), then hand off to mapserver. Everything
# extra is forwarded as flags so `docker run image --pool 8` works.
set -e

# Catch the most common deploy mistake: forgetting `-v /path/to/D2:/game`.
# Without the mount, workers crash with an opaque LoadLibrary error 126.
if [ ! -f /game/D2Common.dll ]; then
    echo "ERROR: /game/D2Common.dll not found." >&2
    echo "Mount your Diablo II 1.13c install with: -v /path/to/Diablo II:/game:ro" >&2
    exit 1
fi

Xvfb :99 -screen 0 320x240x16 -nolisten tcp &
export DISPLAY=:99

# Wait briefly for the X socket so the first wine spawn doesn't race Xvfb.
for _ in 1 2 3 4 5 6 7 8 9 10; do
    [ -e /tmp/.X11-unix/X99 ] && break
    sleep 0.2
done

# Auto-enable /api/lookup when a level<id>.idx is mounted at /index. Use the
# first matching index file; ops can override with --trielookup-index. Don't
# fail when there's no index — the lookup route just stays off in that case.
LOOKUP_ARGS=""
if [ -d /index ]; then
    IDX=""
    for f in /index/level*.idx; do
        [ -f "$f" ] || continue
        IDX="$f"
        break
    done
    if [ -n "$IDX" ]; then
        LOOKUP_ARGS="--trielookup /app/bin/trielookup --trielookup-index $IDX"
        echo "lookup index: $IDX" >&2
    else
        echo "no /index/level*.idx found; /api/lookup disabled" >&2
    fi
fi

/app/mapserver \
    --mapdump /app/bin/mapdump.exe \
    --runner wine \
    --static-dir /app/static \
    $LOOKUP_ARGS \
    "$@" &
MAPSERVER_PID=$!

trap 'kill -TERM "$MAPSERVER_PID" 2>/dev/null' TERM INT

# Watchdog: poll /healthz; after 4 consecutive failures (~60s with 15s
# interval) kill mapserver so docker's --restart=always brings the container
# back fresh. Docker's HEALTHCHECK only labels containers unhealthy — it does
# not restart them on standalone Docker.
(
    sleep 60  # grace period: first wineboot + worker spawn
    fails=0
    while kill -0 "$MAPSERVER_PID" 2>/dev/null; do
        if curl -fsS --max-time 5 http://127.0.0.1:8080/healthz >/dev/null 2>&1; then
            fails=0
        else
            fails=$((fails + 1))
            echo "watchdog: /healthz failed ($fails/4)" >&2
            if [ "$fails" -ge 4 ]; then
                echo "watchdog: killing mapserver to trigger container restart" >&2
                kill -TERM "$MAPSERVER_PID" 2>/dev/null || true
                break
            fi
        fi
        sleep 15
    done
) &

wait "$MAPSERVER_PID"
