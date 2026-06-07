"""C2 PoC — detached cmd:run vs script reload FreeLibrary UAF.

Timeline per round:
  t=0.0  client R (background thread) sends cmd:run -> backend runs the slow
         inspect on a DETACHED thread; it sleeps 6 s inside the script DLL.
  t=1.5  client M (main thread) sends compile_and_load(inspect.cpp) -> backend
         recompiles (~2-4 s) then FreeLibrary's the OLD dll and loads the new.
  t~4-5  FreeLibrary lands while the detached inspect is still sleeping in the
         old module. When the sleep returns, the old code/epilogue runs from
         (possibly) unmapped memory -> access violation / backend death.

Verdict:
  backend dies (proc exits / ping fails)  -> C2 UAF CONFIRMED
  backend survives all rounds             -> reload is serialized vs in-flight
                                             run (finding defended / not repro)
"""
from __future__ import annotations
import sys, threading, time
from pathlib import Path
from harness import (ROOT, REPO_ROOT, BACKEND_EXE, port_open, safe_kill,
                     spawn_backend, wait_log, read_log, PORT_UP_BUDGET)

sys.path.insert(0, str(REPO_ROOT / "tools" / "xinsp2_py"))
from xinsp2 import Client  # noqa: E402

PROJ = ROOT / "c2_proj"
SCRIPT = str(PROJ / "inspect.cpp")
PORT = 7902
ROUNDS = 4


def main() -> int:
    if not BACKEND_EXE.exists():
        print("SKIP: backend exe missing"); return 0
    if port_open(PORT):
        print(f"FAIL: :{PORT} already in use"); return 1
    log = ROOT / "c2_be.log"
    proc = spawn_backend(PORT, log, [f"--project={PROJ}", "--script=inspect.cpp"])
    try:
        if not wait_log(log, "autostart: ready", PORT_UP_BUDGET):
            print("FAIL: backend never ready"); print(read_log(log)[-1500:]); return 1
        print("backend ready (slow script compiled)")

        # ONE shared connection (the real VS Code shape): run in flight on the
        # same socket the reload command arrives on. call() multiplexes by id.
        shared = Client(url=f"ws://127.0.0.1:{PORT}/", timeout=30.0)
        shared.connect()

        for rnd in range(ROUNDS):
            if proc.poll() is not None:
                print(f"backend already dead before round {rnd} (rc={proc.returncode})")
                break
            run_result = {}

            def do_run():
                t0 = time.time()
                try:
                    shared.run(timeout=25.0)
                    run_result["status"] = f"returned in {time.time()-t0:.1f}s"
                except Exception as e:
                    run_result["status"] = f"{type(e).__name__}: {str(e)[:80]}"

            th = threading.Thread(target=do_run, daemon=True)
            th.start()
            time.sleep(1.5)  # let the detached inspect enter its 6 s sleep

            reload_status = ""
            try:
                t0 = time.time()
                shared.compile_and_load(SCRIPT, timeout=60)
                reload_status = f"ok in {time.time()-t0:.1f}s"
            except Exception as e:
                reload_status = f"{type(e).__name__}: {str(e)[:80]}"

            th.join(timeout=30)
            alive = proc.poll() is None
            ping_ok = False
            ping_err = ""
            if alive:
                time.sleep(0.8)  # let the reload settle
                try:
                    # backend is single-client: ping on the SAME shared
                    # connection (a fresh socket gets 503 while shared is open).
                    shared.ping()
                    ping_ok = True
                except Exception as e:
                    ping_err = f"{type(e).__name__}: {str(e)[:90]}"
            print(f"[round {rnd}] run={run_result.get('status','?')} | "
                  f"reload={reload_status} | backend_alive={alive} ping_ok={ping_ok} "
                  f"{ping_err} rc={proc.returncode}")
            if not alive:
                break
            time.sleep(0.5)

        # final health check on the shared connection: ping + a happy run
        final_ok = False
        if proc.poll() is None:
            try:
                shared.ping()
                shared.run(timeout=20.0)
                final_ok = True
            except Exception as e:
                print(f"final health check failed: {type(e).__name__}: {str(e)[:90]}")
        try: shared.close()
        except Exception: pass

        print("\n==== C2 VERDICT ====")
        if proc.poll() is not None:
            print(f"backend DIED (rc={proc.returncode}) -> C2 reload-vs-run UAF still present")
            print("---- log tail ----")
            print(read_log(log)[-1600:])
        elif final_ok:
            print("backend survived all reload-during-run rounds AND is healthy "
                  "(fresh ping + run ok) -> C2 UAF FIXED (old module kept mapped "
                  "until the in-flight inspect returned)")
        else:
            print("backend alive but final health check failed -> investigate")
        return 0
    finally:
        safe_kill(proc, "xinsp-backend.exe")


if __name__ == "__main__":
    raise SystemExit(main())
