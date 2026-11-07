#!/usr/bin/env python3
"""Prints the dashboard's current campaign status as a short table."""
import json
import sys
import urllib.request

d = json.load(urllib.request.urlopen("http://localhost:8800/api/campaign"))
print("running:", d["running"], "current:", d["current"], "title:", d["title"])
if d["error"]:
    print("ERROR", d["error"])
for r in d["results"]:
    m = r.get("measured", {})
    print(f"{r['verdict']:7} {r['fault']:20} detect={m.get('detect_ms')} back={m.get('recover_ms')} "
          f"late={m.get('max_late_ms')} hold={m.get('hold_ms')} alarm={m.get('alarm_ms')} "
          f"dose={m.get('dose_err_pct')}% {'; '.join(r['reasons'])}")
print("report:", d["report"])
sys.exit(0 if not d["running"] else 3)
