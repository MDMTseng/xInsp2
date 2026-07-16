"""
qa_pack_walk — script-side pack READ surfaces e2e, PACK-ONLY (Gate P2 parity
matrix rows "generic walk", "typed-schema reads", "cross-thread capture";
docs/new_gen/12-pack-parity-matrix.md).

mock_camera runs in PACK MODE; the inspect script proves, per pack, that
(1) the generic for_each walk sees exactly {seq:i64, gain:f64, frame:image}
(gain is the ex-feedback closed-loop echo), (2) typed-
schema (ScriptTypedPack) reads equal the string-keyed reads, and (3) a
ScriptPack captured BY VALUE into a worker thread stays valid (checksums
agree). The script contains no xi::Record and this driver observes ONLY
run_result events — pack-only end to end.

Asserts:
  1. >= 5 ok verdicts, each with a parseable "walk ..." message showing n=3,
     keys covering seq+gain+frame, tags=1, typed==string seq AND gain, xthread=1.
  2. NO ng verdicts.
  3. seq strictly increasing in arrival order.

Run:  python examples/qa_pack_walk/driver.py   (Windows; backend built)
"""
from __future__ import annotations
import os
import tempfile, queue, re, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
sys.path.insert(0, str(REPO / "examples" / "lib"))
from xinsp2 import Client  # noqa: E402
from ports import free_port, backend_exe  # noqa: E402

BACKEND = backend_exe()
PORT = int(os.environ.get("PORT", "0")) or free_port()
MSG_RE = re.compile(
    r"walk n=(\d+) keys=(\S+) tags=(\d) seq=(-?\d+) typed=(-?\d+) "
    r"gain=([\d.]+) gtyped=([\d.]+) xthread=(\d)")


def spawn(port):
    iso = Path(tempfile.gettempdir()) / "xi_pack_walk_iso"
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

        oks: list[tuple] = []             # (n, keys, tags, seq, typed, xthread)
        ngs: list[str] = []
        end = time.time() + 8.0
        while time.time() < end:
            for d in drain_verdicts(c):
                code = d.get("code", 0)
                msg = d.get("msg", "")
                m = MSG_RE.search(msg)
                if code > 0 and m:
                    oks.append((int(m.group(1)), m.group(2), int(m.group(3)),
                                int(m.group(4)), int(m.group(5)),
                                float(m.group(6)), float(m.group(7)),
                                int(m.group(8))))
                elif code < 0 or (code > 0 and not m):
                    ngs.append(f"code={code} msg={msg!r}")
            if len(oks) >= 12:
                break
            time.sleep(0.1)

        c.exchange_instance("cam", {"command": "stop"})
        c.call("stop")
        print(f"ok_verdicts={len(oks)} ng={len(ngs)} "
              f"first={oks[0] if oks else None}")

        # 1. enough pack-only ok verdicts, each structurally right.
        if len(oks) < 5:
            fails.append(f"too few pack-only ok verdicts (got {len(oks)}, want >=5)")
        for n, keys, tags, seq, typed, gain, gtyped, xthread in oks:
            keyset = set(keys.split(","))
            if n != 3 or keyset != {"seq", "gain", "frame"}:
                fails.append(f"generic walk wrong: n={n} keys={keys}")
                break
            if tags != 1:
                fails.append(f"walk tags mismatched ABI tags: {keys}")
                break
            if seq != typed:
                fails.append(f"typed-schema read disagrees with string read: {seq} vs {typed}")
                break
            if gain != gtyped:
                fails.append(f"typed-schema f64 read disagrees: {gain} vs {gtyped}")
                break
            if xthread != 1:
                fails.append("cross-thread ScriptPack capture broke (checksums differ)")
                break
        # 2. no ng verdicts.
        if ngs:
            fails.append(f"ng verdicts arrived: {ngs[:3]}")
        # 3. seq strictly increasing.
        seqs = [o[3] for o in oks]
        for i in range(1, len(seqs)):
            if seqs[i] <= seqs[i - 1]:
                fails.append(f"seq not strictly increasing at {i}: {seqs[i-1]} -> {seqs[i]}")
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
        print("VERDICT: FAIL: pack-only script read surfaces e2e")
        return 1
    print("VERDICT: PASS: generic walk + typed-schema reads + cross-thread "
          "ScriptPack capture all agree, pack-only in the live service")
    return 0


if __name__ == "__main__":
    sys.exit(main())
