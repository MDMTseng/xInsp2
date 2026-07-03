"""
qa_resource_handle — the resource-handle convention LIVE (doc 14 appendix):
the lut_owner TYPE-OWNER lib plugin (ring_slots=2) constructs heavy demo.lut
objects that never ride packs; packs carry only the handle entry
{type:"demo.lut", id, gen, $v} as a nested canonical-mp map. The script builds
the fixed LUT A through consumer u1, hops the handle entry through a door hop
to consumer u2 which queries + dumps it, then proves the lease mechanics.

Asserts (script-side, one verdict per tick):
  1. >= 5 ok verdicts with a parseable "rhqa ..." message; NO ng verdicts.
  2. ZERO REBUILD: built=1 only at seq 0 (content dedup after), the owner's
     build counter b == seq+1 exactly (LUT A built once, ever; one unique
     throwaway per tick), and u2 sees the SAME counter u1 saw (zr=1).
  3. The door hop works (hop=1, q1=q2=1) and the dump materialization is
     BYTE-DETERMINISTIC across consumers (dq=1).
  4. Ring-pressure recycle: last tick's throwaway handle answers a clean
     sealed $fault "stale_handle" (stale=1 for seq>=1; -1 skip at seq 0).
  5. Wrong-type resolve answers $fault "wrong_type" (wt=1).
  6. The owner's own books (exchange stats): registered, dedup_hits >= 1,
     recycles >= 1, stale_faults >= 1, wrong_type_faults >= 1, live <= 2.

Run:  python examples/qa_resource_handle/driver.py   (Windows; backend +
      plugins built — lut_owner ships from plugins/lut_owner)
"""
from __future__ import annotations
import os, queue, re, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
from xinsp2 import Client  # noqa: E402

BACKEND = REPO / "backend" / "build" / "Release" / "xinsp-backend.exe"
PORT = int(os.environ.get("PORT", "7919"))
MSG_RE = re.compile(
    r"rhqa seq=(-?\d+) ok=(\d) flt=(\d) built=(-?\d+) b=(-?\d+) hop=(\d) "
    r"q1=(\d) q2=(\d) zr=(\d) dq=(\d) stale=(-?\d+) wt=(\d)")


def spawn(port):
    iso = Path(os.environ["LOCALAPPDATA"]) / "Temp" / "xi_resource_handle_iso"
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
                    oks.append(tuple(int(m.group(i)) for i in range(1, 13)))
                elif code < 0 or (code > 0 and not m):
                    ngs.append(f"code={code} msg={msg!r}")
            if len(oks) >= 10:
                break
            time.sleep(0.1)

        stats = {}
        try:
            stats = c.exchange_instance("lut", {"command": "stats"}) or {}
        except Exception as e:
            fails.append(f"stats exchange failed: {e!r}")
        c.call("stop")
        print(f"ok_verdicts={len(oks)} ng={len(ngs)} "
              f"first_ok={oks[0] if oks else None} stats={stats}")

        # 1. enough green full-loop verdicts, each structurally right; no ng.
        if len(oks) < 5:
            fails.append(f"too few ok verdicts (got {len(oks)}, want >=5)")
        for (seq, ok, flt, built, b, hop, q1, q2, zr, dq, stale, wt) in oks:
            if ok != 1 or flt != 0:
                fails.append(f"seq={seq}: happy legs broke: ok={ok} flt={flt}"); break
            if hop != 1 or q1 != 1 or q2 != 1:
                fails.append(f"seq={seq}: handle hop broke: hop={hop} q1={q1} q2={q2}"); break
            # ZERO REBUILD: A built exactly once ever; the owner's build
            # counter is exactly 1 (A) + seq (one unique throwaway per past
            # tick); both consumers observed the same counter.
            if built != (1 if seq == 0 else 0) or b != seq + 1 or zr != 1:
                fails.append(f"seq={seq}: ZERO-REBUILD broke: built={built} b={b} zr={zr}"); break
            if dq != 1:
                fails.append(f"seq={seq}: dump determinism broke: dq={dq}"); break
            if stale != (-1 if seq == 0 else 1):
                fails.append(f"seq={seq}: stale-lease leg broke: stale={stale}"); break
            if wt != 1:
                fails.append(f"seq={seq}: wrong-type leg broke: wt={wt}"); break
        if ngs:
            fails.append(f"ng verdicts arrived: {ngs[:3]}")

        # 2. the owner's own books.
        if stats:
            if stats.get("registered") is not True:
                fails.append(f"owner not registered: {stats.get('registered')}")
            if stats.get("dedup_hits", 0) < 1:
                fails.append(f"no dedup hits: {stats.get('dedup_hits')}")
            if stats.get("recycles", 0) < 1:
                fails.append(f"no ring recycles: {stats.get('recycles')}")
            if stats.get("stale_faults", 0) < 1:
                fails.append(f"no stale faults: {stats.get('stale_faults')}")
            if stats.get("wrong_type_faults", 0) < 1:
                fails.append(f"no wrong-type faults: {stats.get('wrong_type_faults')}")
            if stats.get("live", 99) > 2:
                fails.append(f"ring overgrew: live={stats.get('live')}")
            if stats.get("ring_slots") != 2:
                fails.append(f"ring_slots config not applied: {stats.get('ring_slots')}")
        else:
            fails.append("no stats from the lut instance")

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
        print("VERDICT: FAIL: resource-handle convention e2e (demo.lut type owner)")
        return 1
    print("VERDICT: PASS: handle entry {type,id,gen,$v} rode packs through a "
          "door hop to a second consumer with ZERO rebuild (build counter "
          "pinned), byte-deterministic dump materialization, and clean "
          "stale_handle / wrong_type $faults from the type owner")
    return 0


if __name__ == "__main__":
    sys.exit(main())
