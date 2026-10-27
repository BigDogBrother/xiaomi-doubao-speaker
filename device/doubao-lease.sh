#!/bin/sh
# Heartbeat-protected ownership of native wake mute. Never stops alarm/player services.
set -eu
lease=/tmp/doubao-voice-lease
self=/tmp/doubao-voice-lease.sh
token=${2:-}
case "$token" in ''|*[!a-f0-9]*) exit 30 ;; esac
[ "${#token}" -eq 32 ] || exit 30
is_owner() { [ -f "$lease/token" ] && [ "$(cat "$lease/token")" = "$token" ]; }
now_s() { cut -d. -f1 /proc/uptime; }
beat() { now_s > "$lease/heartbeat.new"; mv "$lease/heartbeat.new" "$lease/heartbeat"; }

restore() {
    if [ -f "$lease/owned" ] && is_owner; then
        player_pidfile=/tmp/doubao-aplay-$token.pid
        if [ -f "$player_pidfile" ]; then
            player_pid=$(cat "$player_pidfile")
            case "$player_pid" in ''|*[!0-9]*) ;; *)
                if [ -r "/proc/$player_pid/cmdline" ] && grep -q "$player_pidfile" "/proc/$player_pid/cmdline"; then
                    kill -KILL "$player_pid" 2>/dev/null || true
                fi ;;
            esac
            rm -f "$player_pidfile"
        fi
        if [ -f /tmp/mipns/mute ]; then
            ubus -t2 -S call pnshelper event_notify '{"src":3,"event":8}' >/dev/null 2>&1 || return 1
        fi
        [ ! -f /tmp/mipns/mute ] || return 1
        rm -f "$lease/owned" "$lease/heartbeat" "$lease/heartbeat.new" "$lease/created" "$lease/ready" "$lease/active" "$lease/token"
        rmdir "$lease" 2>/dev/null || true
    fi
}

case "${1:-}" in
    begin)
        [ ! -f /tmp/mipns/mute ] || exit 21
        mkdir "$lease" || exit 22
        chmod 700 "$lease"
        echo "$token" > "$lease/token"
        beat
        cp "$lease/heartbeat" "$lease/created"
        : > "$lease/owned"
        # Start fail-safe before muting; dead PC/network restores within ~8 s.
        sh "$self" watch "$token" >/tmp/doubao-voice-watchdog.log 2>&1 </dev/null &
        watcher=$!
        count=0
        while [ ! -f "$lease/ready" ] && [ "$count" -lt 20 ]; do
            kill -0 "$watcher" 2>/dev/null || { restore; exit 26; }
            sleep 0.1
            count=$((count + 1))
        done
        [ -f "$lease/ready" ] || { restore; exit 27; }
        if ! ubus -t2 -S call pnshelper event_notify '{"src":3,"event":7}' >/dev/null 2>&1; then
            restore
            exit 23
        fi
        [ -f /tmp/mipns/mute ] || { restore; exit 24; }
        : > "$lease/active"
        kill -0 "$watcher" 2>/dev/null || { restore; exit 28; }
        echo acquired
        ;;
    heartbeat)
        is_owner || exit 29
        [ -f "$lease/owned" ] && [ -f /tmp/mipns/mute ] || exit 25
        beat
        echo alive
        ;;
    end)
        if [ -d "$lease" ]; then is_owner || exit 29; fi
        restore
        echo restored
        ;;
    watch)
        trap '' HUP
        trap 'restore' EXIT INT TERM
        : > "$lease/ready"
        while [ -f "$lease/owned" ] && is_owner; do
            sleep 1
            [ -f "$lease/heartbeat" ] || break
            now=$(now_s)
            last=$(cat "$lease/heartbeat")
            created=$(cat "$lease/created")
            [ "$((now - last))" -le 7 ] || break
            # User/native control unmuted: release ownership, do not mute again.
            if [ -f "$lease/active" ]; then [ -f /tmp/mipns/mute ] || break; fi
        done
        ;;
    *) exit 2 ;;
esac
