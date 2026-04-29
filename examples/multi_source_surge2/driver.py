"""multi_source_surge2 driver - FL r6 regression sub-round.

Goal: verify the three PR #22 fixes hold via a different driver path.

  Fix 1 (P1-1): dispatch_stats counters reset on cmd:start. Drive 5
                start/stop sweeps with different N/queue settings,
                follow the doc's prescription (sample AFTER stop and
                treat as per-run), confirm numbers stay sane and
                positive across sweeps.

  Fix 2 (P1-3): cmd:start emits a WARN log when N>1 and watchdog>0.
                Set the watchdog via cmd:set_watchdog_ms, then start
                with N=4 and capture the log inbox via on_log().

  Fix 3 (P2-1): VAR(foo, foo) collision. Compile inspect_collision.cpp
                on purpose, capture the cl.exe diagnostic via the
                SDK's enriched ProtocolError, and check whether the
                xi_var.hpp footgun comment + writing-a-script gotcha
                are reachable from the resulting error.

Topology is deliberately different from multi_source_surge/:
  - 2 sources (steady source_a, bursty source_b), 1 sink
  - 5 sweeps with different N/queue/overflow combos
  - source_b under slow_mode (heavy sleep_ms via xi::Param) for the
    watchdog test
"""
from __future__ import annotations

import json
import statistics
import sys
import threading
import time
from collections import Counter, defaultdict
from pathlib import Path
from queue import Empty

from xinsp2 import Client, ProtocolError

ROOT = Path(__file__).parent
PROJECT_JSON   = ROOT / "project.json"
INSPECT_CPP    = ROOT / "inspect.cpp"
COLLISION_CPP  = ROOT / "inspect_collision.cpp"

DRIVER_FPS = 200
SWEEP_DURATION_S = 2.5

# 5 sweeps. Mix N=1 and N>1, two queue caps, two overflow policies.
# Surge plans differ across sweeps so the counter values per-sweep
# should differ visibly, exposing any reset-misbehaviour.
SWEEPS = [
    # (label,            N, q,   overflow,       slow_ms, surges)
    ("S1-N1-q32-clean",  1, 32,  "drop_oldest",  0,  []),
    ("S2-N4-q64-light",  4, 64,  "drop_oldest",  0,  [(0.5, "source_b", 30)]),
    ("S3-N1-q16-tight",  1, 16,  "drop_oldest",  0,  [(0.5, "source_b", 80)]),
    ("S4-N4-q128-big",   4, 128, "drop_oldest",  0,  [(0.5, "source_b", 250)]),
    ("S5-N2-q32-newest", 2, 32,  "drop_newest",  0,  [(0.5, "source_b", 250)]),
]


def write_parallelism(dispatch_threads: int, queue_depth: int,
                      overflow: str = "drop_oldest") -> None:
    cfg = json.loads(PROJECT_JSON.read_text())
    cfg["parallelism"] = {
        "dispatch_threads": dispatch_threads,
        "queue_depth":      queue_depth,
        "overflow":         overflow,
    }
    PROJECT_JSON.write_text(json.dumps(cfg, indent=2) + "\n")


def drain_q(q):
    try:
        while True:
            q.get_nowait()
    except Empty:
        pass


def collect_vars(c: Client, duration_s: float) -> list[dict]:
    events = []
    deadline = time.time() + duration_s
    while time.time() < deadline:
        rem = deadline - time.time()
        try:
            ev = c._inbox_vars.get(timeout=min(0.2, max(0.02, rem)))
        except Empty:
            continue
        flat = {}
        for it in ev.get("items") or []:
            flat[it["name"]] = it.get("value")
        flat["_run_id"] = ev.get("run_id")
        events.append(flat)
    return events


def schedule_surges(c: Client, t0: float, plan):
    def worker():
        for (t_off, inst, count) in plan:
            wait = (t0 + t_off) - time.time()
            if wait > 0:
                time.sleep(wait)
            try:
                c.exchange_instance(inst, {"command": "burst", "count": count})
            except Exception as e:
                print(f"  [surge] {inst} burst={count} failed: {e}")
    th = threading.Thread(target=worker, daemon=True)
    th.start()
    return th


def per_run_stats_after(c: Client) -> dict:
    """The doc-prescribed pattern: snapshot dispatch_stats AFTER stop,
    treat as per-run total. Do NOT subtract any pre-start snapshot."""
    try:
        return c.call("dispatch_stats")
    except ProtocolError as e:
        return {"_err": str(e)}


def run_sweep(c: Client, label: str, n: int, q: int, overflow: str,
              slow_ms: int, surges, log_inbox: list) -> dict:
    print(f"\n=== {label}  N={n}  q={q}  overflow={overflow}  slow_ms={slow_ms} ===")
    write_parallelism(n, q, overflow)
    info = c.open_project(str(ROOT))
    inst_names = sorted(i.get('name') for i in (info.get('instances') or []))
    print(f"  instances: {inst_names}")

    c.compile_and_load(str(INSPECT_CPP))

    if slow_ms > 0:
        c.set_param("slow_mode_ms", slow_ms)
        print(f"  set slow_mode_ms={slow_ms}")

    time.sleep(0.3)
    drain_q(c._inbox_vars)
    drain_q(c._inbox_previews)
    log_pre_len = len(log_inbox)

    c.call("start", {"fps": DRIVER_FPS})
    t0 = time.time()
    schedule_surges(c, t0, surges)
    events = collect_vars(c, SWEEP_DURATION_S)
    elapsed = time.time() - t0
    c.call("stop")
    time.sleep(0.2)

    # Doc-prescribed: snapshot after stop, treat as per-run totals.
    stats_after = per_run_stats_after(c)

    # Catch any log lines that landed during start/run.
    new_logs = log_inbox[log_pre_len:]

    active = [e for e in events if e.get("active") is True and "seq" in e]
    by_src = Counter(e.get("src") or "?" for e in active)
    lats = [float(e["latency_us"]) for e in active
            if isinstance(e.get("latency_us"), (int, float))]

    drops = (stats_after.get("dropped_oldest", 0) or 0) + \
            (stats_after.get("dropped_newest", 0) or 0)
    qmax = stats_after.get("queue_depth_high_watermark", 0) or 0

    # Reset semantics check: every counter field MUST be a non-negative
    # integer; the high-watermark MUST be in [0, queue_depth_cap]. If
    # any sweep returns a negative or > cap value we have a reset bug.
    cap = stats_after.get("queue_depth_cap", q) or q
    sane = (
        isinstance(stats_after.get("dropped_oldest"), int) and stats_after["dropped_oldest"] >= 0 and
        isinstance(stats_after.get("dropped_newest"), int) and stats_after["dropped_newest"] >= 0 and
        isinstance(qmax, int) and 0 <= qmax <= cap
    )

    print(f"  active inspects: {len(active)} ({len(active)/elapsed:.1f}/s)  by_src: {dict(by_src)}")
    print(f"  dispatch_stats AFTER: {stats_after}")
    print(f"  per-run drops={drops}  qmax={qmax}/{cap}  sane_counters={sane}")
    if lats:
        p95 = sorted(lats)[max(0, int(len(lats)*0.95)-1)]
        print(f"  latency_us mean/median/p95: {statistics.mean(lats):.0f} / "
              f"{statistics.median(lats):.0f} / {p95:.0f}")
    if new_logs:
        warn_lines = [m for m in new_logs if m.get("level") == "warn"]
        print(f"  log lines during sweep: {len(new_logs)} total, {len(warn_lines)} warn")
        for m in warn_lines[:3]:
            print(f"    [warn] {m.get('msg', '')[:160]}")

    return {
        "label": label, "n": n, "q": q, "overflow": overflow, "slow_ms": slow_ms,
        "active": len(active), "throughput": len(active) / elapsed if elapsed else 0.0,
        "by_src": dict(by_src),
        "drops": drops, "qmax": qmax, "qcap": cap,
        "sane_counters": sane,
        "stats_after": stats_after,
        "lat_mean": statistics.mean(lats) if lats else None,
        "lat_p95": (sorted(lats)[max(0, int(len(lats)*0.95)-1)] if lats else None),
        "new_log_warns": [m for m in new_logs if m.get("level") == "warn"],
    }


# -- Fix-2 specific test: watchdog warn-log under N>1 ------------------

def test_watchdog_warn(c: Client, log_inbox: list) -> dict:
    """Set watchdog>0, start with N=4. The PR #22 fix should emit a
    WARN log line. Without watchdog, also confirm long inspect under
    N=4 actually completes (so the warn was useful)."""
    print("\n=== FIX 2: watchdog warn-log under N>1 ===")

    # Reload project as N=4.
    write_parallelism(4, 64, "drop_oldest")
    c.open_project(str(ROOT))
    c.compile_and_load(str(INSPECT_CPP))

    # Arm the watchdog. Anything > 0 should trigger the warn path.
    try:
        wd = c.call("set_watchdog_ms", {"ms": 500})
        print(f"  set_watchdog_ms -> {wd}")
    except ProtocolError as e:
        print(f"  set_watchdog_ms FAILED: {e}")
        return {"warn_received": False, "error": str(e)}

    log_pre_len = len(log_inbox)

    # cmd:start. The PR #22 path emits the WARN here.
    c.call("start", {"fps": DRIVER_FPS})
    time.sleep(0.5)
    c.call("stop")
    time.sleep(0.2)

    # Disarm the watchdog so it doesn't bleed into later sweeps.
    c.call("set_watchdog_ms", {"ms": 0})

    new_logs = log_inbox[log_pre_len:]
    warns = [m for m in new_logs if m.get("level") == "warn"]
    wd_warns = [m for m in warns
                if "watchdog" in (m.get("msg") or "").lower()]

    print(f"  log lines during start: {len(new_logs)} total, "
          f"{len(warns)} warn, {len(wd_warns)} watchdog-related")
    for m in wd_warns[:3]:
        print(f"    [warn] {m.get('msg', '')}")

    # Negative control: start without watchdog -> NO warn.
    print("  -- negative control: start with watchdog=0 --")
    log_pre_len2 = len(log_inbox)
    c.call("start", {"fps": DRIVER_FPS})
    time.sleep(0.4)
    c.call("stop")
    time.sleep(0.2)
    new_logs2 = log_inbox[log_pre_len2:]
    wd_warns2 = [m for m in new_logs2
                 if m.get("level") == "warn"
                 and "watchdog" in (m.get("msg") or "").lower()]
    print(f"    -> {len(wd_warns2)} watchdog warns (expect 0)")

    return {
        "warn_received":      len(wd_warns) >= 1,
        "warn_text":          (wd_warns[0].get("msg") if wd_warns else None),
        "negative_clean":     len(wd_warns2) == 0,
    }


# -- Fix-3 specific test: VAR(foo, foo) collision diagnostic ----------

def test_var_shadow(c: Client) -> dict:
    """Compile inspect_collision.cpp; expect cl.exe C2374. Check that
    the SDK's ProtocolError surfaces a diagnostic the developer can
    use to find the xi_var.hpp footgun comment and the gotcha doc."""
    print("\n=== FIX 3: VAR(name, name) shadow diagnostic ===")
    err_msg = ""
    diags = []
    try:
        c.compile_and_load(str(COLLISION_CPP))
        return {"compiled": True, "expected_failure": False}
    except ProtocolError as e:
        err_msg = str(e)
        diags = (e.data or {}).get("diagnostics", []) if isinstance(e.data, dict) else []
        print(f"  ProtocolError caught (good).")
        print(f"  message preview: {err_msg.splitlines()[0][:160]}")
        for d in diags[:6]:
            if isinstance(d, dict):
                msg = d.get("message", "")
                code = d.get("code", "")
                line = d.get("line", "?")
                print(f"    {code} L{line}: {msg[:140]}")

    # Reload the working inspect.cpp so subsequent steps don't run on
    # a half-loaded broken script.
    c.compile_and_load(str(INSPECT_CPP))

    has_c2374 = ("C2374" in err_msg) or any(
        isinstance(d, dict) and d.get("code") == "C2374" for d in diags
    )
    redef_lang = ("redef" in err_msg.lower()) or any(
        isinstance(d, dict) and "redef" in (d.get("message") or "").lower()
        for d in diags
    )

    return {
        "compiled":         False,
        "has_C2374":        has_c2374,
        "redef_in_msg":     redef_lang,
        "n_diagnostics":    len(diags),
        "first_diag":       diags[0] if diags else None,
    }


def main() -> int:
    print("multi_source_surge2 - FL r6 regression sub-round")
    print("Verifying PR #22 fixes via a different driver path.\n")

    # Capture every log message globally; the watchdog warn must
    # actually arrive at the SDK, not just stderr.
    log_inbox: list = []
    def remember(msg: dict):
        log_inbox.append(msg)

    with Client() as c:
        c.on_log(remember)

        # ---- multi-sweep counter regression ----
        sweep_results = []
        for (label, n, q, ovr, slow, surges) in SWEEPS:
            r = run_sweep(c, label, n, q, ovr, slow, surges, log_inbox)
            sweep_results.append(r)

        # ---- watchdog fix verification ----
        wd_result = test_watchdog_warn(c, log_inbox)

        # ---- VAR shadow fix verification ----
        var_result = test_var_shadow(c)

        # ---- summary ----
        print("\n\n=========== SWEEP COMPARISON ===========")
        hdr = ("Sweep", "N", "q", "overflow", "active",
               "thr/s", "drops", "qmax/qcap", "sane")
        print("  ".join(f"{h:>16}" for h in hdr))
        for r in sweep_results:
            print("  ".join(f"{x:>16}" for x in (
                r["label"], r["n"], r["q"], r["overflow"], r["active"],
                f"{r['throughput']:.1f}", r["drops"],
                f"{r['qmax']}/{r['qcap']}", r["sane_counters"],
            )))

        all_sane = all(r["sane_counters"] for r in sweep_results)
        # Exactly one sweep (S5 drop_newest under sustained surge) is
        # expected to log dropped_newest; others should be 0 there.
        # We don't assert exact numbers - just consistency.

        # Verify per-sweep counter resets actually reset:
        # any sweep with no surge should have qmax LOW relative to
        # sweeps with the 250-frame surge. If reset is broken, qmax
        # would carry over.
        clean_qmax = sweep_results[0]["qmax"]   # S1, no surge
        big_qmax   = sweep_results[3]["qmax"]   # S4, 250-frame surge
        reset_ok = (clean_qmax < big_qmax) or (clean_qmax <= 4 and big_qmax > clean_qmax)

        print("\n=========== VERIFICATION ===========")
        print(f"FIX 1 (counter reset / doc-driver pattern):")
        print(f"  all sweeps returned sane non-negative counters: {all_sane}")
        print(f"  clean-sweep qmax ({clean_qmax}) < big-surge qmax ({big_qmax}): "
              f"{clean_qmax < big_qmax}")
        print(f"  -> reset_ok: {reset_ok}")

        print(f"\nFIX 2 (watchdog warn under N>1):")
        print(f"  warn arrived at SDK log inbox: {wd_result.get('warn_received')}")
        if wd_result.get("warn_text"):
            print(f"  warn text: {wd_result['warn_text']}")
        print(f"  no warn when watchdog=0: {wd_result.get('negative_clean')}")

        print(f"\nFIX 3 (VAR shadow diagnostic):")
        print(f"  cl.exe failed as expected: {not var_result['compiled']}")
        print(f"  C2374 in error chain: {var_result.get('has_C2374')}")
        print(f"  'redefinition' wording in error: {var_result.get('redef_in_msg')}")
        print(f"  diagnostics count: {var_result.get('n_diagnostics')}")

        # Pass = all three fixes hold AND no sweep returned insane counters.
        pass_all = (
            all_sane
            and reset_ok
            and wd_result.get("warn_received") is True
            and wd_result.get("negative_clean") is True
            and var_result.get("compiled") is False
            and (var_result.get("has_C2374") or var_result.get("redef_in_msg"))
        )
        print("\nVERDICT (mechanical):", "PASS" if pass_all else "FAIL")

        # Persist a JSON dump for RESULTS.md/FRICTION.md to reference.
        out = {
            "sweeps":   sweep_results,
            "watchdog": wd_result,
            "var":      var_result,
            "verdict":  "PASS" if pass_all else "FAIL",
        }
        (ROOT / "driver_summary.json").write_text(
            json.dumps(out, indent=2, default=str))
        return 0 if pass_all else 2


if __name__ == "__main__":
    sys.exit(main())
