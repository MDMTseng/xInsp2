"""
qa_pack_poly_door — the POLYMORPHIC DOOR pattern (blessed convention:
contract/canonical-profile-notes.md "Producer-domain type dispatch"): ONE
plugin door (poly_door's xi.pack@1 door) dispatches different behaviors on the
pack's bare `type` entry — the producer-domain twin of the capability plane's
`$v` dispatch. The script routes a mixed sequence (measure / annotate / one
unknown) through the SAME door via use().process() every tick and keeps a
running per-type dispatch count in xi::kv().

Asserts:
  1. >= 5 ok verdicts with a parseable "polydoor ..." message, each showing
     built=1 m=1 a=1 u=1 (measure stats right, annotate overlay byte-checked,
     unknown type faulted unsupported_type + $fault_key=type + "types" naming
     the supported set); NO ng verdicts.
  2. The xi::kv() aggregation: km == ka == ku == seq+1 in every verdict (one
     dispatch of each kind per tick, counted across ticks).

Run:  python examples/qa_pack_poly_door/driver.py   (Windows; backend built)
"""
from __future__ import annotations
import os, queue, re, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
sys.path.insert(0, str(REPO / "examples" / "lib"))
from xinsp2 import Client  # noqa: E402
from ports import free_port  # noqa: E402

BACKEND = REPO / "backend" / "build" / "Release" / "xinsp-backend.exe"
PORT = int(os.environ.get("PORT", "0")) or free_port()
MSG_RE = re.compile(
    r"polydoor seq=(-?\d+) built=(\d) m=(\d) a=(\d) u=(\d) "
    r"km=(-?\d+) ka=(-?\d+) ku=(-?\d+)")


def spawn(port):
    iso = Path(os.environ["LOCALAPPDATA"]) / "Temp" / "xi_pack_poly_door_iso"
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
    if os.name != "nt":
        print("SKIP: Windows-only"); return 0
    if not BACKEND.exists():
        print(f"SKIP: backend not built ({BACKEND})"); return 0

    fails: list[str] = []
    proc = spawn(PORT)
    try:
        c = connect(PORT)
        assert c, "no connect"
        c.call("open_project", {"path": str(ROOT)})
        c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=300)

        drain_verdicts(c)                 # zero the event baseline
        c.call("start", {"fps": 10})      # synthetic timer ticks drive the script

        oks: list[tuple] = []
        ngs: list[str] = []
        end = time.time() + 10.0
        while time.time() < end:
            for d in drain_verdicts(c):
                code = d.get("code", 0)
                msg = d.get("msg", "")
                m = MSG_RE.search(msg)
                if code > 0 and m:
                    oks.append(tuple(int(m.group(i)) for i in range(1, 9)))
                elif code < 0 or (code > 0 and not m):
                    ngs.append(f"code={code} msg={msg!r}")
            if len(oks) >= 8:
                break
            time.sleep(0.1)

        c.call("stop")
        print(f"ok_verdicts={len(oks)} ng={len(ngs)} "
              f"first_ok={oks[0] if oks else None} last_ok={oks[-1] if oks else None}")

        # 1. enough green mixed-dispatch verdicts, each structurally right; no ng.
        if len(oks) < 5:
            fails.append(f"too few ok verdicts (got {len(oks)}, want >=5)")
        for (seq, built, m_ok, a_ok, u_ok, km, ka, ku) in oks:
            if built != 1:
                fails.append(f"pack build broke at seq={seq}"); break
            if m_ok != 1:
                fails.append(f"measure dispatch broke at seq={seq}"); break
            if a_ok != 1:
                fails.append(f"annotate dispatch broke at seq={seq}"); break
            if u_ok != 1:
                fails.append(f"unknown-type fault leg broke at seq={seq}"); break
            # 2. the xi::kv() per-type aggregation tracks the tick count exactly.
            if not (km == ka == ku == seq + 1):
                fails.append(f"kv per-type counts skewed at seq={seq}: "
                             f"km={km} ka={ka} ku={ku}"); break
        seqs = [o[0] for o in oks]
        if seqs != sorted(seqs) or len(set(seqs)) != len(seqs):
            fails.append(f"verdict seqs not strictly increasing: {seqs}")
        if ngs:
            fails.append(f"ng verdicts arrived: {ngs[:3]}")

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
        print("VERDICT: FAIL: polymorphic door (type dispatch through one pack door)")
        return 1
    print("VERDICT: PASS: ONE pack door dispatched measure + annotate + faulted "
          "the unknown type (unsupported_type, supported set in 'types'), with "
          "xi::kv() per-type counts tracking every tick")
    return 0


if __name__ == "__main__":
    sys.exit(main())
