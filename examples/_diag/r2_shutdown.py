"""R2-#1/#2 PoC — cmd:shutdown does a clean controlled teardown.

Spawn a backend running continuous mode (script loaded, bus sink installed,
g_srv_for_bp set), then cmd:shutdown. The controlled teardown must drop the bus
sink + unload the script while the pool/srv are alive, so the process exits
cleanly (rc 0, "shutdown complete") with no static-destruction crash.
"""
from __future__ import annotations
import sys, time
from pathlib import Path
from harness import (ROOT, REPO_ROOT, BACKEND_EXE, port_open, safe_kill,
                     spawn_backend, wait_log, read_log, PORT_UP_BUDGET)

sys.path.insert(0, str(REPO_ROOT / "tools" / "xinsp2_py"))
from xinsp2 import Client  # noqa: E402

PROJ = REPO_ROOT / "examples" / "qa_run_result"
PORT = 7912


def main() -> int:
    if not BACKEND_EXE.exists():
        print("SKIP"); return 0
    if port_open(PORT):
        print(f"FAIL: :{PORT} in use"); return 1
    log = ROOT / "r2_shutdown_be.log"
    proc = spawn_backend(PORT, log, [f"--project={PROJ}", "--script=inspect.cpp",
                                     "--autostart-fps=20"])
    try:
        if not wait_log(log, "autostart: ready", PORT_UP_BUDGET):
            print("FAIL: not ready"); return 1
        time.sleep(1.0)   # let continuous mode run a bit (bus sink active)
        with Client(url=f"ws://127.0.0.1:{PORT}/", timeout=15.0) as c:
            c.call("shutdown", {}, timeout=10)
        # wait for the process to exit on its own
        deadline = time.time() + 15
        while time.time() < deadline and proc.poll() is None:
            time.sleep(0.2)
        rc = proc.poll()
        clean = "shutdown complete" in read_log(log)
        print(f"\n==== R2-#1/#2 VERDICT ====")
        print(f"process exit rc={rc}  'shutdown complete' logged={clean}")
        if rc == 0 and clean:
            print("=> FIXED: clean controlled teardown, no shutdown-time crash")
        elif rc is None:
            print("=> backend did not exit after shutdown — investigate")
        else:
            print(f"=> non-clean exit rc={rc} (0x{rc & 0xffffffff:08x}) — possible teardown crash")
            print(read_log(log)[-600:])
        return 0
    finally:
        safe_kill(proc, "xinsp-backend.exe")


if __name__ == "__main__":
    raise SystemExit(main())
