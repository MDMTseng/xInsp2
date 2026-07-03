"""
qa_pack_fault_path — U1 pack-plane ERROR PATH in the live service
(docs/new_gen/15-pack-fault-semantics.md; parity matrix rows B3/B4; the
io_stress / fixturing_demo error pattern).

mock_camera (PACK MODE) drives the pipeline. Per trigger the script mints a
fault pack (frame_timeout on 'gray'), chains it through TWO blob_analysis
doors — the host funnel short-circuits both hops (the plugin never runs, the
reason + $seq are carried, $src/$prov accumulate "det/det2") — exercises a
REAL door run that mints a contract fault (missing_input, provenance-stamped
by the glue), and a happy control (a valid pack still yields blob_count==1,
no $fault, provenance-stamped). The poisoned frame verdicts NG with a
structured message carrying the propagated reason + chain.

Asserts:
  1. >= 5 NG verdicts (code < 0), each parseable as
     "pfault seq=N built=1 sc1=1 sc2=1 mint=1 happy=1
      reason=frame_timeout prov=det/det2".
  2. ZERO ok verdicts (a poisoned frame must never pass) and zero
     malformed/structurally-red verdicts.

Run:  python examples/qa_pack_fault_path/driver.py   (Windows; backend built)
"""
from __future__ import annotations
import os, queue, re, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
from xinsp2 import Client  # noqa: E402

BACKEND = REPO / "backend" / "build" / "Release" / "xinsp-backend.exe"
PORT = int(os.environ.get("PORT", "7917"))
MSG_RE = re.compile(
    r"pfault seq=(-?\d+) built=(\d) sc1=(\d) sc2=(\d) mint=(\d) happy=(\d) "
    r"reason=(\S+) prov=(\S+)")


def spawn(port):
    iso = Path(os.environ["LOCALAPPDATA"]) / "Temp" / "xi_pack_fault_iso"
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

        c.call("start", {"fps": 0})       # trigger-only; the source drives it
        drain_verdicts(c)                 # zero the event baseline
        c.exchange_instance("cam", {"command": "start"})

        ngs: list[tuple] = []             # parsed pfault tuples (code < 0)
        oks: list[str] = []               # any ok verdict = poison passed = bug
        bad: list[str] = []               # malformed / structurally red
        end = time.time() + 8.0
        while time.time() < end:
            for d in drain_verdicts(c):
                code = d.get("code", 0)
                msg = d.get("msg", "")
                m = MSG_RE.search(msg)
                if code < 0 and m:
                    ngs.append((int(m.group(1)),) +
                               tuple(int(m.group(i)) for i in range(2, 7)) +
                               (m.group(7), m.group(8)))
                elif code > 0:
                    oks.append(f"code={code} msg={msg!r}")
                else:
                    bad.append(f"code={code} msg={msg!r}")
            if len(ngs) >= 12:
                break
            time.sleep(0.1)

        c.exchange_instance("cam", {"command": "stop"})
        c.call("stop")
        print(f"ng_verdicts={len(ngs)} ok={len(oks)} bad={len(bad)} "
              f"first_ng={ngs[0] if ngs else None}")

        # 1. enough NG verdicts, each structurally green.
        if len(ngs) < 5:
            fails.append(f"too few pfault NG verdicts (got {len(ngs)}, want >=5)")
        for seq, built, sc1, sc2, mint, happy, reason, prov in ngs:
            if built != 1:
                fails.append(f"ScriptPackBuilder fault-mint leg broke (built={built})")
                break
            if sc1 != 1 or sc2 != 1:
                fails.append(f"short-circuit legs broke: sc1={sc1} sc2={sc2}")
                break
            if mint != 1:
                fails.append("plugin-minted contract-fault leg broke")
                break
            if happy != 1:
                fails.append("happy control broke (non-fault path affected)")
                break
            if reason != "frame_timeout" or prov != "det/det2":
                fails.append(f"propagated reason/chain wrong on the verdict: "
                             f"reason={reason} prov={prov}")
                break
            if seq < 0:
                fails.append(f"trigger seq missing on the verdict (seq={seq})")
                break

        # 2. a poisoned frame must NEVER pass; nothing malformed.
        if oks:
            fails.append(f"ok verdicts arrived on poisoned frames: {oks[:3]}")
        if bad:
            fails.append(f"malformed verdicts: {bad[:3]}")

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
        print("VERDICT: FAIL: pack-plane error path (fault mint -> 2-hop "
              "short-circuit -> verdict) e2e")
        return 1
    print("VERDICT: PASS: pack fault short-circuited through two doors "
          "(plugins never ran), reason+$seq carried, $prov=det/det2, "
          "plugin-minted fault provenance-stamped, happy path unaffected, "
          "NG verdict carries the chain")
    return 0


if __name__ == "__main__":
    sys.exit(main())
