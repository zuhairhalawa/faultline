"""Faultline campaign runner: injects faults in order, measures, judges, reports.

A campaign file (campaigns/<name>.json) lists steps. Each step names one fault
command and the limits it must meet. The runner sends the fault, watches the
target's event log and state for the step's window, then checks:

  recover_ms     faulted process back (first heartbeat) within this time
  max_late_ms    no control tick later than this
  alarm          whether the pump may raise its alarm (false) or must (true)
  alarm_within_ms   if the alarm must happen, how soon
  dose_err_pct   delivered dose within this percent of prescribed, over the window

A step with "expect_fail": true is a check of the checker: it describes a fault
that should break a limit, and passes only if Faultline catches the breach.
"""
import datetime
import html
import json
import threading
import time
from pathlib import Path

EV_INJECT, EV_DEATH, EV_HANG, EV_RESTART, EV_READY = 2, 3, 4, 5, 6
EV_LATE, EV_HOLD, EV_ALARM, EV_RESUME = 7, 8, 9, 10


class Runner:
    def __init__(self, target, campaign_dir, report_dir):
        self.target = target
        self.campaign_dir = Path(campaign_dir)
        self.report_dir = Path(report_dir)
        self.lock = threading.Lock()
        self.running = False
        self.current = None
        self.results = []
        self.report = None
        self.error = None
        self.title = None

    def status(self):
        with self.lock:
            return {"running": self.running, "current": self.current, "title": self.title,
                    "results": list(self.results), "report": self.report, "error": self.error,
                    "campaigns": sorted(p.stem for p in self.campaign_dir.glob("*.json"))}

    def start(self, name):
        path = self.campaign_dir / (Path(name).name + ".json")
        if not path.exists():
            return {"ok": False, "error": f"no campaign named {name}"}
        with self.lock:
            if self.running:
                return {"ok": False, "error": "a campaign is already running"}
            self.running, self.results, self.report, self.error = True, [], None, None
            self.title = None
        threading.Thread(target=self._run, args=(path,), daemon=True).start()
        return {"ok": True}

    # ------------------------------------------------------------------ run
    def _run(self, path):
        try:
            spec = json.loads(path.read_text(encoding="utf-8"))
            with self.lock:
                self.title = spec.get("name", path.stem)
            started = datetime.datetime.now()
            time.sleep(spec.get("settle_ms", 1500) / 1000)
            for step in spec["steps"]:
                with self.lock:
                    self.current = step["id"]
                result = self._step(step, spec.get("settle_ms", 1500))
                with self.lock:
                    self.results.append(result)
            report = self._write_report(spec, path.stem, started)
            with self.lock:
                self.report = report
        except Exception as exc:  # report, don't crash the server
            with self.lock:
                self.error = f"{type(exc).__name__}: {exc}"
        finally:
            with self.lock:
                self.running = False
                self.current = None

    def _wait_quiet(self, settle_ms):
        """Waits until every faultable process is running again, then settles."""
        end = time.time() + 5
        while time.time() < end:
            st = self.target.snapshot()["state"]
            if st and all(p["state"] == "running" for p in st["procs"]) and st["ctrl"]["mode"] == "run":
                break
            time.sleep(0.1)
        time.sleep(settle_ms / 1000)

    def _step(self, step, settle_ms):
        self._wait_quiet(settle_ms)
        snap = self.target.snapshot()
        before = snap["state"]
        last_seq = max([e["seq"] for e in snap["events"]], default=-1)
        ack = self.target.send(step["fault"])
        window = step.get("window_ms", 1500)
        time.sleep(window / 1000)
        after = self.target.snapshot()["state"]
        events = [e for e in self.target.events_since(last_seq + 1)]

        res = {"id": step["id"], "fault": step["fault"], "title": step.get("title", step["fault"]),
               "expect_fail": bool(step.get("expect_fail")), "limits": step.get("limits", {}),
               "ack": ack.get("ok", False), "error": ack.get("error", "")}
        if not res["ack"]:
            res.update(verdict="ERROR", reasons=[f"fault not injected: {res['error']}"])
            return res

        inject = next((e for e in events if e["type"] == EV_INJECT), None)
        t0 = inject["t_ms"] if inject else (before["t_ms"] if before else 0)
        in_window = [e for e in events if e["t_ms"] <= t0 + window]
        proc = inject["proc"] if inject else ""

        failure = next((e for e in in_window if e["type"] in (EV_DEATH, EV_HANG) and e["proc"] == proc), None)
        ready = next((e for e in in_window if e["type"] == EV_READY and e["proc"] == proc), None)
        lates = [e["arg"] / 1000 for e in in_window if e["type"] == EV_LATE]
        alarm = next((e for e in in_window if e["type"] == EV_ALARM), None)
        hold = next((e for e in in_window if e["type"] == EV_HOLD), None)

        m = {
            "detect_ms": round(failure["t_ms"] - t0, 2) if failure else None,
            "detected_as": {EV_DEATH: "crash", EV_HANG: "hang"}.get(failure["type"]) if failure else None,
            "recover_ms": round(ready["arg"] / 1000, 2) if ready else None,
            "max_late_ms": round(max(lates), 2) if lates else 0.0,
            "late_ticks": len(lates),
            "hold_ms": round(hold["t_ms"] - t0, 2) if hold else None,
            "alarm_ms": round(alarm["t_ms"] - t0, 2) if alarm else None,
        }
        if before and after:
            dv = after["plant"]["volume_ml"] - before["plant"]["volume_ml"]
            de = after["ctrl"]["expected_ml"] - before["ctrl"]["expected_ml"]
            m["dose_err_pct"] = round((dv - de) / de * 100, 2) if de > 0 else None
        res["measured"] = m
        res["events"] = [f'{e["t_ms"] - t0:+.2f} ms  {e["text"]}' for e in in_window]

        reasons, lim = [], res["limits"]
        if "recover_ms" in lim:
            if m["recover_ms"] is None:
                reasons.append("did not recover within the window")
            elif m["recover_ms"] > lim["recover_ms"]:
                reasons.append(f"recovered in {m['recover_ms']} ms, limit {lim['recover_ms']} ms")
        if "max_late_ms" in lim and m["max_late_ms"] > lim["max_late_ms"]:
            reasons.append(f"control tick {m['max_late_ms']} ms late, limit {lim['max_late_ms']} ms")
        if lim.get("alarm") is False and m["alarm_ms"] is not None:
            reasons.append(f"pump alarm raised at {m['alarm_ms']} ms")
        if lim.get("alarm") is True:
            if m["alarm_ms"] is None:
                reasons.append("pump never reached its safe state")
            elif "alarm_within_ms" in lim and m["alarm_ms"] > lim["alarm_within_ms"]:
                reasons.append(f"safe state after {m['alarm_ms']} ms, limit {lim['alarm_within_ms']} ms")
        if "dose_err_pct" in lim and m.get("dose_err_pct") is not None and abs(m["dose_err_pct"]) > lim["dose_err_pct"]:
            reasons.append(f"dose off by {m['dose_err_pct']}%, limit {lim['dose_err_pct']}%")

        if res["expect_fail"]:
            res["verdict"] = "CAUGHT" if reasons else "MISSED"
        else:
            res["verdict"] = "FAIL" if reasons else "PASS"
        res["reasons"] = reasons
        return res

    # ------------------------------------------------------------------ report
    def _write_report(self, spec, stem, started):
        self.report_dir.mkdir(parents=True, exist_ok=True)
        stamp = started.strftime("%Y%m%d-%H%M%S")
        name = f"{stem}-{stamp}"
        with self.lock:
            results = list(self.results)
        ok = all(r["verdict"] in ("PASS", "CAUGHT") for r in results)

        def fmt(v, unit=" ms"):
            return "–" if v is None else f"{v}{unit}"

        md = [f"# Faultline report: {spec.get('name', stem)}", "",
              f"Run {started.strftime('%Y-%m-%d %H:%M')} against target {self.target.host}.",
              f"System: {spec.get('system', '')}",
              f"Environment: {spec.get('environment', '')}", "",
              f"**Overall: {'PASS' if ok else 'FAIL'}** "
              f"({sum(r['verdict'] in ('PASS', 'CAUGHT') for r in results)} of {len(results)} steps as required)", "",
              "| Step | Fault | Detected | Back in service | Worst tick late | Pump | Dose error | Verdict |",
              "| --- | --- | --- | --- | --- | --- | --- | --- |"]
        for r in results:
            m = r.get("measured", {})
            pump = "alarm at " + fmt(m.get("alarm_ms")) if m.get("alarm_ms") is not None else (
                "held at " + fmt(m.get("hold_ms")) if m.get("hold_ms") is not None else "kept running")
            det = (f"{m['detected_as']} in {m['detect_ms']} ms" if m.get("detect_ms") is not None else "–")
            md.append(f"| {r['title']} | `{r['fault']}` | {det} | {fmt(m.get('recover_ms'))} | "
                      f"{fmt(m.get('max_late_ms'))} | {pump} | {fmt(m.get('dose_err_pct'), '%')} | **{r['verdict']}** |")
        md += ["", "## Details", ""]
        for r in results:
            md.append(f"### {r['title']}: {r['verdict']}")
            md.append("")
            md.append(f"Fault `{r['fault']}`. Limits: " +
                      (", ".join(f"{k} {v}" for k, v in r["limits"].items()) or "none") + ".")
            if r.get("expect_fail"):
                md.append("This step is meant to break a limit; it passes when Faultline catches the breach.")
            if r["reasons"]:
                md.append("")
                md += [f"- {x}" for x in r["reasons"]]
            md += ["", "```"] + r.get("events", []) + ["```", ""]
        md += ["## Notes", "",
               "Times come from the QNX target's CLOCK_MONOTONIC. Under QEMU they are approximate:",
               "the emulator doesn't run in real time, so the same build on real hardware gives tighter numbers.",
               "The pump is a simulation used to show recovery behaviour, not a medical device.", ""]
        text = "\n".join(md)
        (self.report_dir / f"{name}.md").write_text(text, encoding="utf-8")
        (self.report_dir / f"{name}.html").write_text(_html(spec.get("name", stem), results, ok, started,
                                                            self.target.host, spec), encoding="utf-8")
        return {"name": name, "md": f"/reports/{name}.md", "html": f"/reports/{name}.html", "ok": ok}


def _html(title, results, ok, started, host, spec):
    esc = html.escape

    def cell(v, unit=" ms"):
        return "–" if v is None else f"{v}{unit}"

    rows = []
    for r in results:
        m = r.get("measured", {})
        pump = ("alarm at " + cell(m.get("alarm_ms"))) if m.get("alarm_ms") is not None else (
            ("held at " + cell(m.get("hold_ms"))) if m.get("hold_ms") is not None else "kept running")
        det = f"{m['detected_as']} in {m['detect_ms']} ms" if m.get("detect_ms") is not None else "–"
        why = "<br>".join(esc(x) for x in r["reasons"]) or ("breach caught" if r["verdict"] == "CAUGHT" else "")
        rows.append(
            f"<tr class='{r['verdict'].lower()}'><td>{esc(r['title'])}</td><td><code>{esc(r['fault'])}</code></td>"
            f"<td>{esc(det)}</td><td>{cell(m.get('recover_ms'))}</td><td>{cell(m.get('max_late_ms'))}</td>"
            f"<td>{esc(pump)}</td><td>{cell(m.get('dose_err_pct'), '%')}</td>"
            f"<td class='v'>{r['verdict']}</td><td class='why'>{why}</td></tr>")
    passed = sum(r["verdict"] in ("PASS", "CAUGHT") for r in results)
    return f"""<!doctype html><html lang="en"><head><meta charset="utf-8">
<title>Faultline report: {esc(title)}</title>
<style>
body{{font:15px/1.5 "Segoe UI",system-ui,sans-serif;color:#14202b;background:#eef1f3;margin:0;padding:32px}}
main{{max-width:1180px;margin:auto;background:#fff;border:1px solid #d5dbe0;padding:28px 32px}}
h1{{font-size:24px;margin:0 0 4px}} .meta{{color:#4c5a66;margin:0 0 18px}}
.overall{{display:inline-block;font-weight:700;padding:4px 10px;border-radius:4px;color:#fff;background:{'#0e7c66' if ok else '#c2272d'}}}
table{{border-collapse:collapse;width:100%;margin-top:18px;font-variant-numeric:tabular-nums}}
th,td{{text-align:left;padding:8px 10px;border-bottom:1px solid #e3e7ea;vertical-align:top}}
th{{font-size:13px;color:#4c5a66;font-weight:600}}
code{{font:13px Consolas,monospace;background:#f1f4f6;padding:1px 4px}}
td.v{{font-weight:700}} tr.pass td.v,tr.caught td.v{{color:#0e7c66}} tr.fail td.v,tr.missed td.v,tr.error td.v{{color:#c2272d}}
td.why{{color:#4c5a66;font-size:13px}} .note{{color:#4c5a66;font-size:13px;margin-top:22px}}
</style></head><body><main>
<h1>Faultline report: {esc(title)}</h1>
<p class="meta">Run {started.strftime('%Y-%m-%d %H:%M')} on target {esc(host)}. {esc(spec.get('system', ''))}<br>{esc(spec.get('environment', ''))}</p>
<span class="overall">{'PASS' if ok else 'FAIL'}: {passed} of {len(results)} steps as required</span>
<table><tr><th>Step</th><th>Fault</th><th>Detected</th><th>Back in service</th><th>Worst tick late</th><th>Pump</th><th>Dose error</th><th>Verdict</th><th></th></tr>
{''.join(rows)}</table>
<p class="note">Times come from the QNX target's CLOCK_MONOTONIC. Under QEMU they are approximate: the emulator doesn't run in real time, so the same build on real hardware gives tighter numbers. The pump is a simulation used to show recovery behaviour, not a medical device.</p>
</main></body></html>"""
