"""process_overhead — measure the cost of `isolation:"process"` vs
`isolation:"in_process"` for a single source instance.

Reuses the `burst_source` plugin from `examples/cross_proc_trigger/`.
At runtime we copy that project tree into two temp dirs and rewrite
`instances/source_iso/instance.json` to flip between isolation modes.

Each mode gets a fresh backend process so RSS deltas are clean.

Measured per mode
-----------------
- open_project + compile wall time
- first-trigger latency (cmd:start -> first VAR)
- sustained throughput over WINDOW_S (events / sec)
- inter-arrival jitter (mean, p95, stdev of gaps)
- backend RSS, sum of xinsp-worker.exe child RSS

Run from this dir:

    python driver.py
"""
from __future__ import annotations

import json
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from queue import Empty

import psutil

from xinsp2 import Client

ROOT       = Path(__file__).parent
SOURCE_TPL = ROOT.parent / "cross_proc_trigger"
WINDOW_S   = 3.0
EXPECTED_FPS = 60
BACKEND_EXE = (ROOT.parent.parent / "backend" / "build" /
               "Release" / "xinsp-backend.exe")


def make_project(tmp: Path, isolation: str) -> Path:
    dst = tmp / f"proj_{isolation}"
    shutil.copytree(SOURCE_TPL, dst)
    inst = dst / "instances" / "source_iso" / "instance.json"
    cfg = json.loads(inst.read_text())
    cfg["isolation"] = isolation
    inst.write_text(json.dumps(cfg, indent=2))
    return dst


def drain(c: Client) -> None:
    for q in (c._inbox_vars, c._inbox_previews):
        try:
            while True:
                q.get_nowait()
        except Empty:
            pass


def collect_arrivals(c: Client, duration_s: float) -> list[float]:
    arr: list[float] = []
    deadline = time.monotonic() + duration_s
    while time.monotonic() < deadline:
        rem = deadline - time.monotonic()
        try:
            ev = c._inbox_vars.get(timeout=min(0.05, max(0.005, rem)))
        except Empty:
            continue
        items = {it["name"]: it.get("value") for it in (ev.get("items") or [])}
        if items.get("active") is True:
            arr.append(time.monotonic())
    return arr


def total_rss_of_children(parent_pid: int, name_substr: str) -> int:
    try:
        p = psutil.Process(parent_pid)
    except psutil.NoSuchProcess:
        return 0
    total = 0
    for ch in p.children(recursive=True):
        try:
            if name_substr.lower() in (ch.name() or "").lower():
                total += ch.memory_info().rss
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            pass
    return total


def measure(label: str, isolation: str, project: Path) -> dict:
    print(f"\n=== {label} (isolation={isolation}) ===")
    out: dict = {"isolation": isolation}

    proc = subprocess.Popen([str(BACKEND_EXE)],
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    out["backend_pid"] = proc.pid
    print(f"  backend pid            : {proc.pid}")
    time.sleep(0.4)

    try:
        with Client() as c:
            t0 = time.monotonic()
            c.open_project(str(project))
            out["open_s"] = time.monotonic() - t0
            print(f"  open_project           : {out['open_s']*1000:.1f} ms")

            c.compile_and_load(str(SOURCE_TPL / "inspect.cpp"))
            time.sleep(0.2)
            drain(c)

            # First-trigger latency.
            t_start = time.monotonic()
            c.call("start", {"fps": 1})
            first = None
            deadline = t_start + 2.0
            while time.monotonic() < deadline and first is None:
                try:
                    ev = c._inbox_vars.get(timeout=0.1)
                except Empty:
                    continue
                items = {it["name"]: it.get("value")
                         for it in (ev.get("items") or [])}
                if items.get("active") is True:
                    first = time.monotonic()
            out["first_trigger_ms"] = (
                (first - t_start) * 1000 if first else None)
            if first:
                print(f"  first-trigger latency  : "
                      f"{out['first_trigger_ms']:.1f} ms")
            else:
                print("  first-trigger latency  : (timeout)")

            # RSS sample.
            try:
                out["backend_rss_mb"] = (
                    psutil.Process(proc.pid).memory_info().rss / 1024 / 1024)
            except psutil.NoSuchProcess:
                out["backend_rss_mb"] = None
            out["worker_rss_mb"] = (
                total_rss_of_children(proc.pid, "xinsp-worker") / 1024 / 1024)
            print(f"  backend RSS            : "
                  f"{out['backend_rss_mb']:.1f} MB")
            print(f"  worker RSS (sum)       : "
                  f"{out['worker_rss_mb']:.2f} MB")

            # Throughput window.
            drain(c)
            arr = collect_arrivals(c, WINDOW_S)

            out["events"]  = len(arr)
            out["rate_hz"] = len(arr) / WINDOW_S if arr else 0.0
            if len(arr) >= 2:
                gaps = [(arr[i+1]-arr[i])*1000 for i in range(len(arr)-1)]
                out["gap_mean_ms"]  = statistics.mean(gaps)
                out["gap_stdev_ms"] = (statistics.stdev(gaps)
                                       if len(gaps) > 1 else 0.0)
                out["gap_p95_ms"]   = sorted(gaps)[int(len(gaps)*0.95)]
            else:
                out["gap_mean_ms"] = out["gap_stdev_ms"] = out["gap_p95_ms"] = None
            print(f"  events / {WINDOW_S}s window : {out['events']}")
            print(f"  rate                   : {out['rate_hz']:.1f} Hz "
                  f"(expected ~{EXPECTED_FPS})")
            if out["gap_mean_ms"] is not None:
                print(f"  inter-arrival mean/p95 : "
                      f"{out['gap_mean_ms']:.2f} / {out['gap_p95_ms']:.2f} ms "
                      f"(stdev {out['gap_stdev_ms']:.2f})")

    finally:
        proc.terminate()
        try: proc.wait(timeout=3)
        except subprocess.TimeoutExpired: proc.kill()
    return out


def main() -> int:
    print("process_overhead — isolation:in_process vs isolation:process")
    print(f"  template      : {SOURCE_TPL}")
    print(f"  window        : {WINDOW_S}s @ ~{EXPECTED_FPS} Hz expected")
    if not BACKEND_EXE.exists():
        print(f"FAIL: backend exe not found: {BACKEND_EXE}")
        return 1
    if not SOURCE_TPL.exists():
        print(f"FAIL: template missing: {SOURCE_TPL}")
        return 1

    with tempfile.TemporaryDirectory(prefix="xinsp_overhead_") as td:
        tmp = Path(td)
        proj_thread  = make_project(tmp, "in_process")
        proj_process = make_project(tmp, "process")

        t = measure("THREAD ", "in_process", proj_thread)
        p = measure("PROCESS", "process",    proj_process)

        # Comparison summary.
        def fmt_ms(v): return "—" if v is None else f"{v:.1f} ms"
        def fmt_mb(v): return "—" if v is None else f"{v:.2f} MB"
        def fmt_hz(v): return "—" if v is None else f"{v:.1f} Hz"
        def diff(a, b): return "—" if a is None or b is None else f"{b-a:+.2f}"

        print()
        print("=" * 78)
        print(f"{'metric':<28}{'thread':>15}{'process':>15}{'Δ':>14}")
        print("-" * 78)
        rows = [
            ("open_project",         "open_s",         lambda v: f"{v*1000:.1f} ms" if v is not None else "—"),
            ("first-trigger latency","first_trigger_ms", fmt_ms),
            ("backend RSS",          "backend_rss_mb", fmt_mb),
            ("worker RSS (sum)",     "worker_rss_mb",  fmt_mb),
            ("events / window",      "events",         lambda v: f"{v}"),
            ("rate",                 "rate_hz",        fmt_hz),
            ("gap mean",             "gap_mean_ms",    fmt_ms),
            ("gap p95",              "gap_p95_ms",     fmt_ms),
            ("gap stdev",            "gap_stdev_ms",   fmt_ms),
        ]
        for label, key, fmt in rows:
            tv, pv = t.get(key), p.get(key)
            d = "—"
            if tv is not None and pv is not None:
                if isinstance(tv, (int, float)) and isinstance(pv, (int, float)):
                    if key == "open_s":
                        d = f"{(pv-tv)*1000:+.1f} ms"
                    elif key in ("backend_rss_mb", "worker_rss_mb"):
                        d = f"{pv-tv:+.2f} MB"
                    elif key == "events":
                        d = f"{pv-tv:+d}"
                    elif key == "rate_hz":
                        d = f"{pv-tv:+.1f} Hz"
                    else:
                        d = f"{pv-tv:+.2f} ms"
            print(f"{label:<28}{fmt(tv):>15}{fmt(pv):>15}{d:>14}")
        print("=" * 78)

        # Headline.
        if (t.get("first_trigger_ms") is not None
                and p.get("first_trigger_ms") is not None):
            ftd = p["first_trigger_ms"] - t["first_trigger_ms"]
            print(f"\nfirst-trigger overhead of process vs thread: "
                  f"{ftd:+.1f} ms")
        odiff = (p["open_s"] - t["open_s"]) * 1000
        print(f"open_project overhead of process vs thread:  "
              f"{odiff:+.1f} ms")
        rss_diff = (p["backend_rss_mb"] + p["worker_rss_mb"]
                    - t["backend_rss_mb"] - t["worker_rss_mb"])
        print(f"total RSS overhead of process vs thread:     "
              f"{rss_diff:+.2f} MB")

        (ROOT / "results.json").write_text(
            json.dumps({"thread": t, "process": p,
                        "window_s": WINDOW_S,
                        "expected_fps": EXPECTED_FPS}, indent=2) + "\n")
        print(f"\nwrote {ROOT / 'results.json'}")
        return 0


if __name__ == "__main__":
    sys.exit(main())
