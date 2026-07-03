"""
qa_cap_imgcodec — the capability plane pilot LIVE (docs/new_gen/14): the
imgcodec LIB plugin (no data plane) registers xi.jpeg.encode + xi.image.decode
on create; a consumer plugin's pack door calls them BY NAME through the host
forwarding funnel; the script drives two consumer calls per tick with a fixed
image and asserts THE DEDUP HEADLINE — the provider's lifetime encode counter
stays at 1 while every call gets byte-identical JPEG (magic-checked).

Asserts:
  1. >= 5 ok verdicts with a parseable "capqa ..." message, each showing
     built=1 fault=0 rc1=0 rc2=0 magic=1 same=1 hit2=1 enc=1 dec1=1 dec2=1;
     NO ng verdicts.
  2. The provider's own books agree: exchange_instance("codec", stats) reports
     encodes == 1 and hits >= 2*oks - 1 (every call after the very first was a
     cache hit), registered == true.

Run:  python examples/qa_cap_imgcodec/driver.py   (Windows; backend built)
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
    r"capqa seq=(-?\d+) built=(\d) fault=(\d) rc1=(-?\d+) rc2=(-?\d+) "
    r"magic=(\d) same=(\d) hit2=(-?\d+) enc=(-?\d+) dec1=(-?\d+) dec2=(-?\d+)")


def spawn(port):
    iso = Path(os.environ["LOCALAPPDATA"]) / "Temp" / "xi_cap_imgcodec_iso"
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
                    oks.append(tuple(int(m.group(i)) for i in range(1, 12)))
                elif code < 0 or (code > 0 and not m):
                    ngs.append(f"code={code} msg={msg!r}")
            if len(oks) >= 10:
                break
            time.sleep(0.1)

        stats = {}
        try:
            stats = c.exchange_instance("codec", {"command": "stats"}) or {}
        except Exception as e:
            fails.append(f"stats exchange failed: {e!r}")
        c.call("stop")
        print(f"ok_verdicts={len(oks)} ng={len(ngs)} "
              f"first_ok={oks[0] if oks else None} stats={stats}")

        # 1. enough green full-loop verdicts, each structurally right; no ng.
        if len(oks) < 5:
            fails.append(f"too few ok verdicts (got {len(oks)}, want >=5)")
        for (seq, built, fault, rc1, rc2, magic, same, hit2, enc,
             dec1, dec2) in oks:
            if built != 1 or fault != 0:
                fails.append(f"build leg broke: built={built} fault={fault}"); break
            if rc1 != 0 or rc2 != 0:
                fails.append(f"funnel rc broke: rc1={rc1} rc2={rc2}"); break
            if magic != 1 or same != 1:
                fails.append(f"jpeg bytes broke: magic={magic} same={same}"); break
            if hit2 != 1 or enc != 1:
                fails.append(f"DEDUP broke: hit2={hit2} encodes={enc}"); break
            if dec1 != 1 or dec2 != 1:
                fails.append(f"decode round trip broke: dec1={dec1} dec2={dec2}"); break
        if ngs:
            fails.append(f"ng verdicts arrived: {ngs[:3]}")

        # 2. the provider's own books: ONE encode ever; everything else hits.
        if stats:
            if stats.get("encodes") != 1:
                fails.append(f"provider encodes != 1: {stats.get('encodes')}")
            if oks and stats.get("hits", 0) < 2 * len(oks) - 1:
                fails.append(f"provider hits too low: {stats.get('hits')} "
                             f"for {len(oks)} runs")
            if stats.get("registered") is not True:
                fails.append(f"provider not registered: {stats.get('registered')}")
        else:
            fails.append("no stats from the codec instance")

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
        print("VERDICT: FAIL: capability plane e2e (imgcodec lib + dedup)")
        return 1
    print("VERDICT: PASS: script -> consumer pack door -> host-forwarded "
          "xi.jpeg.encode/xi.image.decode (imgcodec lib plugin), ONE encode "
          "serving every consumer (byte-identical JPEG, magic-checked)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
