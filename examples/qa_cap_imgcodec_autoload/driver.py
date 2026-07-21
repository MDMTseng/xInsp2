"""
qa_cap_imgcodec_autoload — V3 machine-level lib-plugin autoload LIVE (docs/
new_gen/14 + doc 19 V3). The TWIN of qa_cap_imgcodec with the crucial
difference: the project declares NO `imgcodec` (codec) instance. The imgcodec
LIB plugin is instantiated by the SERVICE at boot under a machine owner
(plugin.json `"autoload": true`), so xi.jpeg.encode / xi.image.decode are
available to the consumer's pack door through the host forwarding funnel
WITHOUT any per-project provider instance. This is the deployment shape the app
team's E1 second cause needed (doc 06 §6): a backend with no codec instance
STILL serves the capability.

Asserts (identical headline to qa_cap_imgcodec, minus the provider-instance
book-keeping which no longer has a project instance to read):
  1. >= 5 ok verdicts with a parseable "capqa ..." message, each showing
     built=1 fault=0 rc1=0 rc2=0 magic=1 same=1 hit2=1 enc=1 dec1=1 dec2=1;
     NO ng verdicts. rc1==0 (not -1 EUNKNOWN) and enc==1 PROVE the capability
     was served by the machine-autoloaded provider with no project instance.

Run:  python examples/qa_cap_imgcodec_autoload/driver.py   (Windows; backend built)
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
    r"capqa seq=(-?\d+) built=(\d) fault=(\d) rc1=(-?\d+) rc2=(-?\d+) "
    r"magic=(\d) same=(\d) hit2=(-?\d+) enc=(-?\d+) dec1=(-?\d+) dec2=(-?\d+)")


def spawn(port):
    iso = Path(tempfile.gettempdir()) / "xi_cap_imgcodec_autoload_iso"
    iso.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ); env["TEMP"] = env["TMP"] = env["TMPDIR"] = str(iso)
    log = open(ROOT / f"backend_{port}.log", "w", encoding="utf-8")
    # --autoload-lib: the deployment opt-in that brings up machine lib providers
    # (imgcodec) at boot, so the capability is served with NO project instance.
    return subprocess.Popen([str(BACKEND), f"--port={port}", "--autoload-lib"],
                            stdout=log, stderr=subprocess.STDOUT, cwd=str(REPO), env=env)


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

        c.call("stop")
        print(f"ok_verdicts={len(oks)} ng={len(ngs)} "
              f"first_ok={oks[0] if oks else None}")

        # enough green full-loop verdicts, each structurally right; no ng.
        # rc1==0 + enc==1 is the machine-autoload proof: the capability was
        # served with NO project imgcodec instance declared.
        if len(oks) < 5:
            fails.append(f"too few ok verdicts (got {len(oks)}, want >=5) — "
                         f"autoload provider likely absent (rc1 would be -1)")
        for (seq, built, fault, rc1, rc2, magic, same, hit2, enc,
             dec1, dec2) in oks:
            if built != 1 or fault != 0:
                fails.append(f"build leg broke: built={built} fault={fault}"); break
            if rc1 != 0 or rc2 != 0:
                fails.append(f"funnel rc broke (no autoload provider?): "
                             f"rc1={rc1} rc2={rc2}"); break
            if magic != 1 or same != 1:
                fails.append(f"jpeg bytes broke: magic={magic} same={same}"); break
            if hit2 != 1 or enc != 1:
                fails.append(f"DEDUP broke: hit2={hit2} encodes={enc}"); break
            if dec1 != 1 or dec2 != 1:
                fails.append(f"decode round trip broke: dec1={dec1} dec2={dec2}"); break
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
        print("VERDICT: FAIL: machine-autoload capability plane (no project codec instance)")
        return 1
    print("VERDICT: PASS: consumer pack door -> host-forwarded xi.jpeg.encode/"
          "xi.image.decode served by the MACHINE-AUTOLOADED imgcodec provider "
          "(no project instance declared) — E1 second cause cleared")
    return 0


if __name__ == "__main__":
    sys.exit(main())
