#!/bin/sh
set -eu
app=/data/doubao-edge
worker=/tmp/doubao-edge-worker.pid
supervisor=/tmp/doubao-edge-supervisor.pid
rm -f "$app/enabled"
if [ -f "$worker" ]; then
    pid=$(cat "$worker")
    case "$pid" in ''|*[!0-9]*) ;; *)
        if [ -r "/proc/$pid/cmdline" ] && grep -q "$app/doubao-edge" "/proc/$pid/cmdline"; then
            kill -TERM "$pid" 2>/dev/null || true
            n=0
            while kill -0 "$pid" 2>/dev/null && [ "$n" -lt 40 ]; do sleep 0.2; n=$((n+1)); done
            if kill -0 "$pid" 2>/dev/null && [ -r "/proc/$pid/cmdline" ] && grep -q "$app/doubao-edge" "/proc/$pid/cmdline"; then kill -KILL "$pid" 2>/dev/null || true; fi
        fi ;;
    esac
fi
if [ -f "$supervisor" ]; then
    pid=$(cat "$supervisor")
    case "$pid" in ''|*[!0-9]*) ;; *)
        if [ -r "/proc/$pid/cmdline" ] && grep -q "$app/start.sh" "/proc/$pid/cmdline"; then kill -TERM "$pid" 2>/dev/null || true; fi ;;
    esac
fi
n=0
while [ -d /tmp/doubao-voice-lease ] && [ "$n" -lt 15 ]; do sleep 1; n=$((n+1)); done
[ ! -d /tmp/doubao-voice-lease ] || { echo edge_lease_still_active; exit 1; }
echo edge_stopped
