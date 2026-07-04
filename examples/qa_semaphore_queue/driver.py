"""
qa_semaphore_queue — a "custom queue" implemented ENTIRELY in a plugin (doc 24):
N-way admission via a counting semaphore, with ZERO core buffering, while the
ordered output ("envelope") comes for free from the core's per-lane emit gate.
NO CORE CHANGES. The concrete proof of the thesis:

    the PLUGIN owns ADMISSION (custody); the CORE owns the ENVELOPE (mechanism).

Wiring (see project.json): ONE dispatch lane "q" with max_parallel:4,
result_order:"arrival", overflow:block. TWO instances of the sem_queue DLL share a
process-global counting semaphore (permits = N = 4):

  * sem_src  — a source: a dedicated producer thread acquire()s a permit (blocks
    when N are in flight), pops from its own deep std::deque backlog, and emit()s
    one trigger into lane "q". The SEMAPHORE is the sole admission control.
  * sem_ack  — the completion signal: its xi.pack@1 door release()s one permit,
    driven INLINE by the inspect's use("sem_ack").process() as its last step
    (release at compute-completion, on the worker thread).

Plus a generic, UNCHANGED downstream sink (expose) that receives the ordered
results.

PHASE 1 (the proof) — drive the lane hot, then stop the source, drain, and assert:
  * N-way admission bound : observed MAX in-flight == N (reaches N under load, and
    the semaphore guarantees it never exceeds N) → it IS parallel, custom-admitted.
  * depth-0 / core not buffering : the CORE lane's high_watermark stays small
    (<= N) — the deep backlog lives in the PLUGIN's deque, the core holds ~nothing.
  * lossless : processed == emitted, ZERO drops.
  * envelope preserved : the run_result stream (and the expose push) stays in
    strict $seq / run_id order despite N-way parallel compute + the custom
    semaphore admission — 0 inversions. THIS is the punchline.

PHASE 2 (clean teardown) — drive hot so the producer is parked blocked on the
semaphore, then cmd:stop + close_project and assert a bounded return: the parked
acquire() is woken by the plugin's abort() (same stop-wake discipline as the
depth-0 rendezvous), never hanging teardown. On teardown the semaphore is simply
abandoned (a production version would also release on drop/stop).

Run:  python examples/qa_semaphore_queue/driver.py   (Windows; backend + plugins built)
"""
from __future__ import annotations
import json, os, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
sys.path.insert(0, str(REPO / "examples" / "lib"))
from xinsp2 import Client  # noqa: E402
from ports import free_port  # noqa: E402

BACKEND = REPO / "backend" / "build" / "Release" / "xinsp-backend.exe"
N = 4                # semaphore permits == lane max_parallel
WINDOW = 3.0         # drive window (s)


def spawn(port):
    iso = Path(os.environ["LOCALAPPDATA"]) / "Temp" / "xi_semq_iso"
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
    if isinstance(defval, str):
        try: defval = json.loads(defval)
        except Exception: return {}
    return defval if isinstance(defval, dict) else {}


def _load(c):
    c.call("open_project", {"path": str(ROOT)})       # compiles sem_queue too
    c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=300)
    c.call("start", {"fps": 0})   # trigger-only: sem_src drives the line


def phase1(fails: list[str]) -> None:
    port = free_port(); proc = spawn(port)
    try:
        c = connect(port)
        if not c: fails.append("phase1: no connect"); return
        _load(c)
        # Emit only AFTER cmd:start so every frame lands in the continuous lane
        # (emitted==processed is then a clean lossless invariant).
        c.call("exchange_instance", {"name": "sem_src", "cmd": {"command": "start"}})

        # Collect run_results as they STREAM IN (per doc: collecting after cmd:stop
        # would reorder the ~max_parallel tail as stop releases in-flight workers
        # out of turn). Track the run_id order for the envelope check + peak
        # max_inflight seen live from get_def.
        seqs: list[int] = []
        peak_live = 0
        end = time.time() + WINDOW
        while time.time() < end:
            try: ev = c._inbox_events.get(timeout=max(0.05, end - time.time()))
            except Exception: continue
            if ev.get("name") == "run_result" and ev.get("data", {}).get("code") == 1:
                rid = ev["data"].get("run_id")
                if rid is not None: seqs.append(rid)

        # Peek the live shared instrumentation (max_inflight) while still hot.
        d_live = _def(c.call("exchange_instance", {"name": "sem_ack", "cmd": {"command": "stat"}}))
        peak_live = int(d_live.get("max_inflight", -1))

        # Stop the source: joins the producer thread → final emitted count.
        d_src = _def(c.call("exchange_instance", {"name": "sem_src", "cmd": {"command": "stop"}}))
        emitted = int(d_src.get("emitted", -1))
        max_inflight = int(d_src.get("max_inflight", -1))

        # Drain remaining run_results until processed == emitted (lossless), bounded.
        drain_end = time.time() + 20.0
        while len(seqs) < emitted and time.time() < drain_end:
            try: ev = c._inbox_events.get(timeout=0.2)
            except Exception: continue
            if ev.get("name") == "run_result" and ev.get("data", {}).get("code") == 1:
                rid = ev["data"].get("run_id")
                if rid is not None: seqs.append(rid)

        st = {g["name"]: g for g in (c.call("dispatch_stats").get("groups") or [])}
        hw   = int(st.get("q", {}).get("high_watermark", -1))
        drop = int(st.get("q", {}).get("dropped", -1))
        d_fin = _def(c.call("exchange_instance", {"name": "sem_ack", "cmd": {"command": "stat"}}))
        released = int(d_fin.get("released", -1))
        max_inflight = max(max_inflight, int(d_fin.get("max_inflight", -1)), peak_live)
        c.call("stop"); c.call("close_project"); c.close()

        processed = len(seqs)
        inversions = sum(1 for i in range(len(seqs) - 1) if seqs[i] > seqs[i + 1])
        print(f"admission : max_inflight={max_inflight} (N={N})   permits={N}")
        print(f"lossless  : emitted={emitted} processed={processed} released={released} dropped={drop}")
        print(f"core buf  : lane 'q' high_watermark={hw}  (backlog lives in the plugin's deque)")
        print(f"envelope  : run_id inversions={inversions} over {processed} ordered results")

        # --- N-WAY ADMISSION BOUND (the custom semaphore queue) ------------------
        if max_inflight != N:
            fails.append(f"phase1: max_inflight={max_inflight} != N={N} "
                         f"(must REACH N under load — proving N-way parallel — and the "
                         f"semaphore guarantees it never exceeds N)")
        # --- LOSSLESS ------------------------------------------------------------
        if emitted <= N:
            fails.append(f"phase1: emitted={emitted} (nothing/too little emitted?)")
        if drop != 0:
            fails.append(f"phase1: DROPPED {drop} (must be 0 — semaphore(N)+N workers+"
                         f"queue_depth N ⇒ back-pressure via the permit, never a drop)")
        if emitted > N and processed != emitted:
            fails.append(f"phase1: NOT lossless: processed {processed} != emitted {emitted}")
        # --- DEPTH-0 / CORE NOT BUFFERING ---------------------------------------
        # The plugin's OWN deque (backlog) holds the real queue; the core lane's
        # semaphore-gated occupancy never exceeds N (usually far less).
        if hw < 0 or hw > N:
            fails.append(f"phase1: core high_watermark {hw} > N={N} — the core buffered "
                         f"more than the semaphore admits (plugin should own the queue)")
        # --- ENVELOPE PRESERVED (the punchline) ---------------------------------
        # Ordered emit under result_order:"arrival" despite N-way parallel compute
        # + custom semaphore admission: run_ids must arrive strictly monotonic.
        if inversions != 0:
            fails.append(f"phase1: {inversions} run_id inversions — the core envelope did "
                         f"NOT hold under custom N-way admission")
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
        # Drive the lane hot so the producer parks blocked on the semaphore
        # (N permits in flight, waiting for a release to admit the next item).
        c.call("exchange_instance", {"name": "sem_src", "cmd": {"command": "start"}})
        time.sleep(1.0)

        # Teardown WHILE parked. If the parked acquire() isn't woken, close_project
        # hangs forever joining the producer thread; here it must return fast.
        t0 = time.time()
        c.call("stop", timeout=20)
        c.call("close_project", timeout=20)
        dt = time.time() - t0
        c.close()
        print(f"teardown  : stop+close_project while parked on the semaphore returned in {dt:.2f}s")
        if dt > 12.0:
            fails.append(f"phase2: teardown took {dt:.2f}s (> 12s bound) — parked producer not woken?")
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
    print("VERDICT: PASS: a custom N-way semaphore queue lives ENTIRELY in the plugin "
          "(max in-flight == N, never exceeding it; the core lane buffers ~nothing) and is "
          "LOSSLESS (processed == emitted, 0 drops), while the core's emit gate preserves the "
          "ENVELOPE (0 run_id inversions under result_order:arrival) — custom admission and "
          "core ordering coexist with NO core changes; teardown while parked on the semaphore "
          "returns bounded (abort() stop-wake, no hang)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
