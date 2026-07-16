"""
qa_pack_pilot — e2e for the wave-2 t.pack() script surface (docs/new_gen/08
Wave 2, step 4).

mock_camera runs in PACK MODE: it emits a v3 Pack on the xi.pack@1 plane
instead of a Record. The pack rides the dual-carry dispatch path to the inspect
script, which reads it via t.pack() and (a) verdicts it with xi::ok/xi::ng and
(b) surfaces seq + image dims on the `expose` channel "qa".

Asserts:
  1. PACK REACHED THE SCRIPT: >=5 XEX1 frames arrive on channel "qa", each
     carrying a seq value read out of t.pack() (before the surface existed the
     script had no way to see a pack-plane emit at all).
  2. SEQ MONOTONICITY: the seq values are strictly increasing in arrival order
     (mock_camera stamps a monotonic frame counter; t.pack().get_i64("seq")
     round-trips it, and the ordered dispatch preserves it).
  3. DIMS: every image reports w==64, h==48, c==3 — the descriptor read back
     through the door matches the mock_camera config.
  4. VERDICT: at least one ok (code>0) run_result arrived — the per-pack verdict
     the script published while holding the pack.

Run:  python examples/qa_pack_pilot/driver.py   (Windows; backend built)
"""
from __future__ import annotations
import os
import tempfile, queue, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
sys.path.insert(0, str(ROOT.parents[0] / "lib"))
from xinsp2 import Client  # noqa: E402
from ports import free_port, backend_exe  # noqa: E402
from xex1 import collect_frames, subscribe  # noqa: E402

BACKEND = backend_exe()
PORT = int(os.environ.get("PORT", "0")) or free_port()


def spawn(port):
    iso = Path(tempfile.gettempdir()) / "xi_pack_pilot_iso"
    iso.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ); env["TEMP"] = env["TMP"] = env["TMPDIR"] = str(iso)
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


def drain_ok_verdicts(c) -> int:
    """Count ok (code>0) run_result events queued on the client's event inbox."""
    n = 0
    while True:
        try:
            ev = c._inbox_events.get_nowait()
        except queue.Empty:
            break
        except Exception:
            break
        if ev.get("name") == "run_result" and (ev.get("data", {}) or {}).get("code", 0) > 0:
            n += 1
    return n


def main() -> int:
    if not BACKEND.exists():
        print(f"SKIP: backend not built ({BACKEND})"); return 0

    fails: list[str] = []
    proc = spawn(PORT)
    try:
        c = connect(PORT)
        assert c, "no connect"
        c.call("open_project", {"path": str(ROOT)})
        c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=300)
        subscribe(c, ["qa"])              # gate the expose channel ON so frames push

        c.call("start", {"fps": 0})       # enable the pipeline; the source drives it
        c.drain_binary()                  # zero the binary baseline
        drain_ok_verdicts(c)              # zero the event baseline

        # Kick mock_camera's capture thread — it emits Packs on the xi.pack@1 plane.
        c.exchange_instance("cam", {"command": "start"})

        seqs: list[int] = []
        dims: list[tuple] = []
        ok_verdicts = 0
        end = time.time() + 8.0
        while time.time() < end:
            for fr in collect_frames(c):
                if fr.get("channel") != "qa":
                    continue
                vals = fr.get("values") or {}
                if "seq" not in vals:
                    continue
                seqs.append(int(vals["seq"]))
                dims.append((vals.get("w"), vals.get("h"), vals.get("c")))
            ok_verdicts += drain_ok_verdicts(c)
            if len(seqs) >= 12:           # plenty — stop early once we have a run
                break
            time.sleep(0.1)

        c.exchange_instance("cam", {"command": "stop"})
        c.call("stop")
        ok_verdicts += drain_ok_verdicts(c)

        print(f"packs={len(seqs)} seqs={seqs[:20]} dims0={dims[0] if dims else None} ok_verdicts={ok_verdicts}")

        # 1. the pack reached the script at all.
        if len(seqs) < 5:
            fails.append(f"too few pack-plane packs reached the script (got {len(seqs)}, want >=5)")

        # 2. seq monotonicity (strictly increasing in arrival order).
        for i in range(1, len(seqs)):
            if seqs[i] <= seqs[i - 1]:
                fails.append(f"seq not strictly increasing at {i}: {seqs[i-1]} -> {seqs[i]} (full={seqs})")
                break

        # 3. dims read back through the door match config (64x48x3).
        bad = [d for d in dims if d != (64, 48, 3)]
        if bad:
            fails.append(f"frame dims mismatch (want (64,48,3)); saw {bad[:5]}")

        # 4. the per-pack verdict surfaced.
        if ok_verdicts < 1:
            fails.append("no ok run_result verdict arrived while the script held the pack")

    except Exception as e:
        fails.append(f"exception: {e!r}")
    finally:
        try:
            proc.terminate(); proc.wait(timeout=10)
        except Exception:
            proc.kill()

    if fails:
        for f in fails:
            print("  -", f)
        print("VERDICT: FAIL: t.pack() pilot e2e")
        return 1
    print("VERDICT: PASS: t.pack() delivered pack-plane packs to the script (seq monotonic, dims ok, verdict surfaced)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
