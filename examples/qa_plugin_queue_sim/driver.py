"""
qa_plugin_queue_sim — "plugin owns the queue" over the depth ladder (doc 24).

A single source plugin (queue_source) OWNS its own deep std::deque of pending work
(backlog=500) and paces its emits ENTIRELY off the core lane's back-pressure. Two
instances feed two max_parallel:1 lanes with the SAME slow ~40ms inspect:

  * group "rv"   — queue_depth:0, overflow:block  → RENDEZVOUS (synchronous handoff:
    each emit blocks until a worker TAKES the frame; strict lock-step, no run-ahead).
  * group "pipe" — queue_depth:1, overflow:block  → 1-deep PIPELINE (the producer
    runs one ahead: it deposits frame N+1 as soon as the worker takes frame N, while
    frame N is still being computed).

Both are LOSSLESS (the plugin's own deque holds the backlog; the core lane holds at
most the single in-flight handoff), so this proves the core buffer stays ~0/1 while
the plugin owns 100% of the real queue.

PHASE 1 (lossless + depth ladder): drive both, stop the sources (join their emit
threads → final emitted counts + the run-ahead tells from get_def), drain, then:
  - rv   : emitted==processed, 0 drops, core high_watermark <= 1, first_gap_ms LARGE
           (~inspect time) → strict rendezvous, no run-ahead.
  - pipe : emitted==processed, 0 drops, core high_watermark <= 1, first_gap_ms SMALL
           (~0) → the producer prepared/emitted frame 1 while the worker still
           computed frame 0 (the pipelining).

PHASE 2 (clean teardown while parked in the rendezvous): a fresh backend, drive the
rv lane hot so the source is parked in the depth-0 handoff wait, then cmd:stop +
close_project and assert a bounded return — the rendezvous stop-wake (predicate keys
off continuous, same discipline as overflow:block) must degrade the parked producer
to a drop, never hang teardown.

Run:  python examples/qa_plugin_queue_sim/driver.py   (Windows; backend + plugins built)
"""
from __future__ import annotations
import json, os, subprocess, sys, time
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
sys.path.insert(0, str(REPO / "examples" / "lib"))
from xinsp2 import Client  # noqa: E402
from ports import free_port  # noqa: E402

BACKEND = REPO / "backend" / "build" / "Release" / "xinsp-backend.exe"
WINDOW = 3.0
INSPECT_MS = 40          # matches inspect.cpp's sleep
FAST_MS = 15             # "fast" emit-return threshold (matches instance config)


def spawn(port):
    iso = Path(os.environ["LOCALAPPDATA"]) / "Temp" / "xi_qs_iso"
    iso.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ); env["TEMP"] = env["TMP"] = str(iso)
    log = open(ROOT / f"backend_{port}.log", "w", encoding="utf-8")
    return subprocess.Popen([str(BACKEND), f"--port={port}"], stdout=log,
                            stderr=subprocess.STDOUT, cwd=str(REPO), env=env)


def connect(port):
    for _ in range(80):
        try:
            c = Client(url=f"ws://127.0.0.1:{port}/", timeout=60); c.connect(); c.ping(); return c
        except Exception:
            time.sleep(0.5)
    return None


def _def(defval) -> dict:
    """exchange_instance returns the plugin's get_def() JSON (dict or string)."""
    if isinstance(defval, str):
        try: defval = json.loads(defval)
        except Exception: return {}
    return defval if isinstance(defval, dict) else {}


def _load(c):
    c.call("open_project", {"path": str(ROOT)})
    c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=300)
    c.call("start", {"fps": 0})   # trigger-only: no synthetic timer ticks


def phase1(fails: list[str]) -> None:
    port = free_port(); proc = spawn(port)
    try:
        c = connect(port)
        if not c: fails.append("phase1: no connect"); return
        _load(c)
        # Emit only AFTER cmd:start so every frame lands in the continuous lane
        # (emitted==processed is then a clean lossless invariant).
        c.call("exchange_instance", {"name": "src_rv",   "cmd": {"command": "start"}})
        c.call("exchange_instance", {"name": "src_pipe", "cmd": {"command": "start"}})

        n = defaultdict(int)
        end = time.time() + WINDOW
        while time.time() < end:
            try: ev = c._inbox_events.get(timeout=max(0.05, end - time.time()))
            except Exception: continue
            if ev.get("name") == "run_result" and ev.get("data", {}).get("code") == 1:
                n[ev["data"].get("group")] += 1

        # Stop the sources: joins each emit thread (a parked rendezvous producer
        # completes its in-flight handoff as the worker takes it, then exits) → the
        # returned get_def carries the FINAL emitted count + run-ahead tells.
        d_rv   = _def(c.call("exchange_instance", {"name": "src_rv",   "cmd": {"command": "stop"}}))
        d_pipe = _def(c.call("exchange_instance", {"name": "src_pipe", "cmd": {"command": "stop"}}))
        emit_rv,   emit_pipe   = int(d_rv.get("emitted", -1)),   int(d_pipe.get("emitted", -1))

        # Drain: count run_results until both lanes have processed every emitted frame
        # (lossless => must reach emitted), bounded by a deadline.
        drain_end = time.time() + 20.0
        while (n["rv"] < emit_rv or n["pipe"] < emit_pipe) and time.time() < drain_end:
            try: ev = c._inbox_events.get(timeout=0.2)
            except Exception: continue
            if ev.get("name") == "run_result" and ev.get("data", {}).get("code") == 1:
                n[ev["data"].get("group")] += 1

        st = {g["name"]: g for g in (c.call("dispatch_stats").get("groups") or [])}
        hw_rv   = int(st.get("rv",   {}).get("high_watermark", -1))
        hw_pipe = int(st.get("pipe", {}).get("high_watermark", -1))
        drop_rv   = int(st.get("rv",   {}).get("dropped", -1))
        drop_pipe = int(st.get("pipe", {}).get("dropped", -1))
        c.call("stop"); c.call("close_project"); c.close()

        fg_rv,   fg_pipe   = int(d_rv.get("first_gap_ms", -1)),   int(d_pipe.get("first_gap_ms", -1))
        fr_rv,   fr_pipe   = int(d_rv.get("fast_returns", -1)),   int(d_pipe.get("fast_returns", -1))
        print(f"rv   (depth0/rendezvous): emitted={emit_rv} processed={n['rv']} dropped={drop_rv} "
              f"core_hw={hw_rv} first_gap_ms={fg_rv} fast_returns={fr_rv}")
        print(f"pipe (depth1/pipeline):   emitted={emit_pipe} processed={n['pipe']} dropped={drop_pipe} "
              f"core_hw={hw_pipe} first_gap_ms={fg_pipe} fast_returns={fr_pipe}")

        # --- LOSSLESS + plugin-owns-the-queue (core buffer ~0/1) for BOTH rungs ---
        for g, emit, proc_n, drop, hw in (("rv", emit_rv, n["rv"], drop_rv, hw_rv),
                                          ("pipe", emit_pipe, n["pipe"], drop_pipe, hw_pipe)):
            if emit <= 1:
                fails.append(f"phase1: {g} emitted={emit} (nothing/too little emitted?)")
            if drop != 0:
                fails.append(f"phase1: {g} DROPPED {drop} (must be 0 — back-pressure, not drop)")
            if emit > 1 and proc_n != emit:
                fails.append(f"phase1: {g} NOT lossless: processed {proc_n} != emitted {emit}")
            # The whole point: the plugin's OWN deque (backlog 500) held the real
            # queue; the core lane never buffered more than the single handoff.
            if hw > 1:
                fails.append(f"phase1: {g} core high_watermark {hw} > 1 — core buffered more than the "
                             f"in-flight handoff (plugin should own the queue)")

        # --- DEPTH LADDER: pipe runs one ahead; rv is strict lock-step -----------
        # depth=1: frame 1's emit returns almost immediately (worker took frame 0,
        # freed the slot) while frame 0 is still being computed → first_gap ~0.
        if fg_pipe < 0 or fg_pipe >= INSPECT_MS // 2:
            fails.append(f"phase1: pipe first_gap_ms={fg_pipe} — expected << {INSPECT_MS} (no run-ahead "
                         f"observed; depth=1 should let the producer prepare frame N+1 while the worker "
                         f"computes N)")
        if fr_pipe < 1:
            fails.append(f"phase1: pipe fast_returns={fr_pipe} — expected >=1 (the one-slot run-ahead)")
        # depth=0: frame 1 can't be TAKEN until the worker finishes computing frame 0
        # → first_gap ~inspect time; strict rendezvous, no run-ahead.
        if fg_rv < INSPECT_MS // 2:
            fails.append(f"phase1: rv first_gap_ms={fg_rv} — expected ~{INSPECT_MS} (rendezvous is strict "
                         f"lock-step: the 2nd emit must wait for the 1st frame to be TAKEN, i.e. for the "
                         f"worker to finish the current inspect)")
        if fr_rv > 1:
            fails.append(f"phase1: rv fast_returns={fr_rv} — expected 0 (no run-ahead in a rendezvous)")
    except Exception as e:
        fails.append(f"phase1: exception: {e!r}")
    finally:
        try: proc.terminate(); proc.wait(10)
        except Exception: proc.kill()


def phase2(fails: list[str]) -> None:
    port = free_port(); proc = spawn(port)
    try:
        c = connect(port)
        if not c: fails.append("phase2: no connect"); return
        _load(c)
        # Drive the rendezvous lane hot so the source is parked in the depth-0 handoff
        # wait (deposited a frame, blocked until a worker takes the NEXT one).
        c.call("exchange_instance", {"name": "src_rv", "cmd": {"command": "start"}})
        time.sleep(1.0)   # by now the source is parked in the rendezvous

        # Teardown WHILE parked. If the rendezvous stop-wake is missing, close_project
        # hangs forever joining the still-parked emit thread; here it must return fast.
        t0 = time.time()
        c.call("stop", timeout=20)
        c.call("close_project", timeout=20)
        dt = time.time() - t0
        c.close()
        print(f"phase2: stop+close_project while parked in rendezvous returned in {dt:.2f}s")
        if dt > 12.0:
            fails.append(f"phase2: teardown took {dt:.2f}s (> 12s bound) — parked rendezvous producer not woken?")
    except Exception as e:
        fails.append(f"phase2: exception (teardown likely hung): {e!r}")
    finally:
        try: proc.terminate(); proc.wait(10)
        except Exception: proc.kill()


def main() -> int:
    if os.name != "nt":
        print("SKIP: Windows-only"); return 0
    if not BACKEND.exists():
        print(f"SKIP: backend not built ({BACKEND})"); return 0
    fails: list[str] = []
    phase1(fails)
    phase2(fails)
    if fails:
        for f in fails: print("  -", f)
        print("VERDICT: FAIL: " + "; ".join(fails))
        return 1
    print("VERDICT: PASS: the plugin owns the queue (backlog held in its own deque; core lane "
          "high_watermark <= 1) and both rungs are lossless — depth=0 is a strict rendezvous "
          "(no run-ahead) while depth=1 pipelines one frame ahead, and teardown while parked in "
          "the rendezvous returns within a bounded time (stop-wake degrades to drop, no hang)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
