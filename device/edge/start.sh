#!/bin/sh
set -eu
app=/data/doubao-edge
self=$app/start.sh
supervisor=/tmp/doubao-edge-supervisor.pid
worker=/tmp/doubao-edge-worker.pid
[ -f "$app/enabled" ] || exit 0
if [ "${1:-start}" = run ]; then
    trap '' HUP
    cd "$app"
    while [ -f "$app/enabled" ]; do
        "$app/lib/ld-linux-armhf.so.3" --library-path "$app/lib" "$app/doubao-edge" -c "$app/config.ini" >>/tmp/doubao-edge.log 2>&1 </dev/null &
        child=$!
        echo "$child" > "$worker"
        wait "$child" || true
        rm -f "$worker"
        [ -f "$app/enabled" ] || break
        sleep 3
    done
    rm -f "$supervisor"
    exit 0
fi
if [ -f "$supervisor" ]; then
    previous=$(cat "$supervisor")
    case "$previous" in ''|*[!0-9]*) ;; *)
        if kill -0 "$previous" 2>/dev/null && grep -q "$self" "/proc/$previous/cmdline"; then exit 0; fi ;;
    esac
fi
cp "$app/lease.sh" /tmp/doubao-voice-lease.sh
chmod 700 /tmp/doubao-voice-lease.sh
sh "$self" run </dev/null >/dev/null 2>&1 &
echo "$!" > "$supervisor"
