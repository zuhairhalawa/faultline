#!/usr/bin/env bash
# Read-only health check of the dashboard server and the target link.
pgrep -af 'dashboard/server.py' || echo "server not running"
curl -s -m 3 http://localhost:8800/api/snapshot | python3 -c 'import json,sys; d=json.load(sys.stdin); print("connected", d["connected"], "events", len(d["events"]), "history", len(d["history"]), "state", bool(d["state"]))'
tail -5 /tmp/fl-dash.log
pgrep -a qemu-system | cut -c1-80
