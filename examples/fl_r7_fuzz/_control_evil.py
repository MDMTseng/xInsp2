"""Control: does the host crash if evil_worker just connects and
closes immediately, sending zero frames? If yes — the bug is in the
host's pipe-EOF handling, not in any specific malformed frame."""
import os, sys, time, json, shutil, subprocess
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from _common import BACKEND_EXE, WS_URL, port_open
from xinsp2 import Client, ProtocolError

REPO_ROOT = Path(__file__).resolve().parents[2]
RELEASE_DIR = BACKEND_EXE.parent
ORIG = RELEASE_DIR / "xinsp-worker.exe"
BAK  = RELEASE_DIR / "xinsp-worker.exe.fuzz-bak"
EVIL = RELEASE_DIR / "evil_worker.exe"
PROJECT = REPO_ROOT / "examples" / "cross_proc_trigger"

if not BAK.exists():
    shutil.copy2(ORIG, BAK)
shutil.copy2(EVIL, ORIG)

env = os.environ.copy()
env["FUZZ_STRATEGY"] = "-2"  # immediate close
proc = subprocess.Popen([str(BACKEND_EXE)], cwd=str(BACKEND_EXE.parent), env=env,
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    t0 = time.time()
    while time.time() - t0 < 10 and not port_open():
        time.sleep(0.1)
    c = Client(WS_URL); c.connect()
    open_err = None
    try:
        c.open_project(str(PROJECT), timeout=15.0)
    except Exception as e:
        open_err = repr(e)[:300]
    time.sleep(0.5)
    pingable, ping_exc = True, None
    try: c.ping()
    except Exception as e:
        pingable = False; ping_exc = repr(e)[:200]
    proc_alive = (proc.poll() is None)
    print(json.dumps({"open_err": open_err, "pingable": pingable,
                      "ping_exc": ping_exc, "proc_alive": proc_alive,
                      "rc": proc.returncode}, indent=2))
finally:
    if proc.poll() is None:
        try:
            import websocket
            ws = websocket.create_connection(WS_URL, timeout=2.0)
            ws.send(json.dumps({"type":"cmd","id":1,"name":"shutdown"}))
            ws.close()
        except Exception: pass
        try: proc.wait(timeout=3)
        except Exception: proc.kill()
    shutil.copy2(BAK, ORIG)
    BAK.unlink(missing_ok=True)
