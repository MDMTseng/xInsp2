"""
qa_pack_stereo — PACK-ONLY stereo gathering e2e (Gate P2 parity matrix row
"multi-image gathered trigger"; docs/new_gen/12-pack-parity-matrix.md).

synced_stereo runs in PACK MODE under the real backend: each `fire` gathers the
correlated left+right pair + shared `seq` into ONE sealed xi.pack@1 Pack under a
single trigger. The inspect script reads it via t.pack() and verdicts it. The
whole data plane is Pack — the script contains no xi::Record at all, and this
driver observes ONLY run_result events (the verdict plane), so the example is
pack-only end to end.

Asserts:
  1. GATHERED PACKS REACHED THE SCRIPT: >= 8 ok verdicts, each carrying the
     "stereo seq=..." message the script formats from the pack's entries.
  2. NO NG: every verdict is ok — dims are the plugin's fixed 320x240x1 and the
     seq stamped in both images' first 4 bytes equals the pack's seq entry
     (the same-event correlation proof, in script hands, in the live service).
  3. SEQ MONOTONIC: seq values strictly increase in arrival order.

Run:  python qa/qa_pack_stereo/driver.py   (Windows; backend built)
"""
from __future__ import annotations
import os
import tempfile, queue, re, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
sys.path.insert(0, str(REPO / "qa" / "lib"))
from xinsp2 import Client  # noqa: E402
from ports import free_port, backend_exe  # noqa: E402

BACKEND = backend_exe()
PORT = int(os.environ.get("PORT", "0")) or free_port()
MSG_RE = re.compile(r"stereo seq=(-?\d+) lseq=(-?\d+) rseq=(-?\d+) dims=(\d)")


def spawn(port):
    iso = Path(tempfile.gettempdir()) / "xi_pack_stereo_iso"
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
    """Pop every queued run_result event's data dict off the client inbox."""
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

        c.call("start", {"fps": 0})       # enable the pipeline; fires drive it
        drain_verdicts(c)                 # zero the event baseline

        # Deterministic drive: small bursts so the dispatch queue never drops.
        for _ in range(6):
            c.exchange_instance("stereo", {"command": "fire", "n": 2})
            time.sleep(0.35)

        oks: list[tuple[int, int, int, int]] = []   # (seq, lseq, rseq, dims)
        ngs: list[str] = []
        end = time.time() + 8.0
        while time.time() < end:
            for d in drain_verdicts(c):
                code = d.get("code", 0)
                msg = d.get("msg", "")
                m = MSG_RE.search(msg)
                if code > 0 and m:
                    oks.append(tuple(int(g) for g in m.groups()))
                elif code < 0 or (code > 0 and not m):
                    ngs.append(f"code={code} msg={msg!r}")
            if len(oks) + len(ngs) >= 12:
                break
            time.sleep(0.1)

        c.call("stop")
        print(f"ok_verdicts={len(oks)} ng={len(ngs)} seqs={[o[0] for o in oks][:16]}")

        # 1. gathered packs reached the script.
        if len(oks) < 8:
            fails.append(f"too few pack-only ok verdicts (got {len(oks)}, want >=8)")
        # 2. every verdict passed dims + same-event correlation in the script;
        #    re-check here so a script bug can't silently pass.
        for seq, lseq, rseq, dims in oks:
            if not (seq == lseq == rseq) or dims != 1:
                fails.append(f"correlation/dims broke: seq={seq} lseq={lseq} rseq={rseq} dims={dims}")
                break
        if ngs:
            fails.append(f"ng verdicts arrived: {ngs[:3]}")
        # 3. seq strictly increasing in arrival order.
        seqs = [o[0] for o in oks]
        for i in range(1, len(seqs)):
            if seqs[i] <= seqs[i - 1]:
                fails.append(f"seq not strictly increasing at {i}: {seqs[i-1]} -> {seqs[i]} (full={seqs})")
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
        print("VERDICT: FAIL: pack-only stereo gathering e2e")
        return 1
    print("VERDICT: PASS: synced_stereo pack-mode gathered left+right+seq into one "
          "pack per trigger; script verified same-event correlation pack-only")
    return 0


if __name__ == "__main__":
    sys.exit(main())
