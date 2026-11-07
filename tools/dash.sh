#!/usr/bin/env bash
# Starts or stops the dashboard server in the background, or runs a campaign from the shell.
#   tools/dash.sh start | stop | campaign [name]
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PIDF=/tmp/fl-dash.pid
case "${1:-}" in
start)
    [ -f "$PIDF" ] && kill "$(cat "$PIDF")" 2>/dev/null
    setsid nohup python3 "$ROOT/dashboard/server.py" > /tmp/fl-dash.log 2>&1 < /dev/null &
    echo $! > "$PIDF"
    sleep 1.5
    cat /tmp/fl-dash.log
    ;;
stop)
    [ -f "$PIDF" ] && kill "$(cat "$PIDF")" 2>/dev/null; rm -f "$PIDF"
    ;;
campaign)
    name="${2:-pump}"
    curl -s -X POST -d "{\"name\":\"$name\"}" http://localhost:8800/api/campaign; echo
    sleep 2
    until python3 "$ROOT/tools/show_campaign.py" > /tmp/fl-camp.txt; do sleep 2; done
    cat /tmp/fl-camp.txt
    ;;
*) echo "usage: dash.sh start|stop|campaign [name]"; exit 2 ;;
esac
