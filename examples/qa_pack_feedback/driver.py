"""
qa_pack_feedback — closed-loop control at FRAME latency, pack-only.

mock_camera (pack mode, initial gain 0.2 — deliberately dim) drives the
pipeline. Per frame the script measures mean intensity, computes a
proportional multiplicative gain correction toward the target band
(110 +/- 14), and pushes a CONTROL pack {command:"set_gain", value} into the
camera's OWN xi.pack@1 door (xi::use("cam").process(ctrl)); the commanded
gain takes effect on the NEXT emitted frame. The loop is a SOURCE plugin
consuming packs through its own door — the bilingual source, both directions.

Verdicts ride the run_result plane (xi::ok / xi::ng); zero xi::Record.

Asserts:
  1. >= 15 ok verdicts with a parseable "fb ..." message; NO ng.
  2. The loop STARTS unconverged: first frame's mean is far below the band
     (the dim plant) and out of band.
  3. CONVERGENCE within K frames: the mean enters the band within the first
     8 verdicts.
  4. MONOTONE approach: until the band is reached, |mean - target| never
     increases by more than a small drift slack (the plant's own gradient
     shifts ~1-3 gray levels per frame).
  5. STABILITY: once in band, EVERY later frame stays in band (the loop keeps
     regulating against the source's drift).
  6. The control ACTUALLY acted: the echoed gain at convergence grew by more
     than 1.5x from the initial gain, and at least one frame's echoed gain
     equals the PREVIOUS frame's commanded value (the frame-latency contract
     observed on the wire), and every ack echoes exactly the commanded value.

Run:  python examples/qa_pack_feedback/driver.py   (Windows; backend built)
"""
from __future__ import annotations
import os
import tempfile, queue, re, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
sys.path.insert(0, str(ROOT.parents[0] / "lib"))
from xinsp2 import Client  # noqa: E402
from ports import free_port, backend_exe  # noqa: E402

BACKEND = backend_exe()
# A probed-free port, not a fixed one: several qa drivers share 7917 and a
# stale/lingering backend there serves STALE plugin DLLs to whoever connects.
PORT = int(os.environ.get("PORT", "0")) or free_port()
TARGET, BAND = 110.0, 14.0
MSG_RE = re.compile(
    r"fb seq=(-?\d+) mean=([\d.]+) gain=([\d.]+) cmd=([\d.]+) "
    r"ackg=(-?[\d.]+) band=(\d)")


def spawn(port):
    iso = Path(tempfile.gettempdir()) / "xi_pack_feedback_iso"
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


def drain_verdicts(c) -> list[dict]:
    out = []
    while True:
        try:
            ev = c._inbox_events.get_nowait()
        except queue.Empty:
            break
        except Exception:
            break
        if ev.get("name") == "run_result":
            out.append(ev.get("data", {}) or {})
    return out


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

        c.call("start", {"fps": 0})       # enable the pipeline; the source drives it
        drain_verdicts(c)                 # zero the event baseline

        c.exchange_instance("cam", {"command": "start"})

        oks: list[tuple] = []             # (seq, mean, gain, cmd, ackg, band)
        ngs: list[str] = []
        end = time.time() + 10.0
        while time.time() < end:
            for d in drain_verdicts(c):
                code = d.get("code", 0)
                msg = d.get("msg", "")
                m = MSG_RE.search(msg)
                if code > 0 and m:
                    oks.append((int(m.group(1)), float(m.group(2)),
                                float(m.group(3)), float(m.group(4)),
                                float(m.group(5)), int(m.group(6))))
                elif code < 0 or (code > 0 and not m):
                    ngs.append(f"code={code} msg={msg!r}")
            if len(oks) >= 22:
                break
            time.sleep(0.1)

        c.exchange_instance("cam", {"command": "stop"})
        c.call("stop")

        oks.sort(key=lambda t: t[0])
        means = [t[1] for t in oks]
        gains = [t[2] for t in oks]
        cmds  = [t[3] for t in oks]
        ackgs = [t[4] for t in oks]
        bands = [t[5] for t in oks]
        errs  = [abs(m - TARGET) for m in means]
        print(f"ok={len(oks)} ng={len(ngs)} "
              f"means={[round(m, 1) for m in means[:12]]} "
              f"gains={[round(g, 3) for g in gains[:12]]}")

        # 1. enough closed-loop ticks; none structurally broken.
        if len(oks) < 15:
            fails.append(f"too few ok verdicts (got {len(oks)}, want >=15)")
        if ngs:
            fails.append(f"ng verdicts arrived: {ngs[:3]}")

        if oks:
            # 2. starts unconverged: the dim plant, out of band, well below it.
            if bands[0] != 0 or means[0] >= TARGET - BAND:
                fails.append(f"loop did not start unconverged: first mean={means[0]:.2f} "
                             f"band={bands[0]} (initial gain 0.2 should be dim)")

            # 3. convergence within K frames.
            K = next((i for i, b in enumerate(bands) if b == 1), None)
            if K is None:
                fails.append(f"never reached the band: means={[round(m,1) for m in means]}")
            elif K > 8:
                fails.append(f"too slow: first in-band verdict at index {K} (want <=8)")

            if K is not None:
                # 4. monotone approach (small slack for the plant's own drift).
                for i in range(K):
                    if errs[i + 1] > errs[i] + 3.0:
                        fails.append(f"non-monotone approach at {i}: "
                                     f"err {errs[i]:.2f} -> {errs[i+1]:.2f}")
                        break

                # 5. stability: once in band, stays in band.
                for i in range(K, len(oks)):
                    if bands[i] != 1:
                        fails.append(f"left the band at index {i}: mean={means[i]:.2f} "
                                     f"(seq={oks[i][0]})")
                        break

                # 6. the control actually acted, through the door.
                if gains[K] < gains[0] * 1.5:
                    fails.append(f"gain barely moved: {gains[0]:.3f} -> {gains[K]:.3f}")
                if not any(abs(gains[i + 1] - cmds[i]) <= 1e-3
                           for i in range(len(oks) - 1)):
                    fails.append("no frame's echoed gain matches the previous frame's "
                                 "command -- the door never steered the plant")
                for i, (cmd, ackg) in enumerate(zip(cmds, ackgs)):
                    if abs(cmd - ackg) > 1e-3:
                        fails.append(f"ack mismatch at {i}: cmd={cmd} ackg={ackg}")
                        break

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
        print("VERDICT: FAIL: closed-loop frame-latency control (analyze -> door -> next frame)")
        return 1
    print("VERDICT: PASS: closed-loop control at frame latency, pack-only: "
          "the script steers mock_camera through its OWN pack door into the "
          "target band and holds it there")
    return 0


if __name__ == "__main__":
    sys.exit(main())
