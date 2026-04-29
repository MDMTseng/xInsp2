"""FL r7 target #3 — host-side IPC frame parser fuzzer.

We swap evil_worker.exe in for xinsp-worker.exe (saving the original),
then open a project that contains an `isolation:"process"` instance.
The backend spawns *us* as the worker; we connect to the named pipe
and send malformed frames. Backend's reader thread in
xi_process_instance.hpp must not take the host down.

PASS criteria
-------------
- Backend process stays alive across N evil-worker spawns.
- Backend stays pingable on the WS port between/after each spawn.
- Reader thread exits cleanly (host should be willing to spawn a
  fresh worker if asked again — though we don't strictly require
  successful respawn here).

How spawning happens
--------------------
We don't directly invoke evil_worker. Instead:
1. Rename xinsp-worker.exe -> xinsp-worker.exe.bak
2. Copy evil_worker.exe -> xinsp-worker.exe
3. Start backend (it picks up the renamed worker via get_exe_dir())
4. Open the cross_proc_trigger project (contains isolation:process inst)
5. Backend's PluginManager spawns "xinsp-worker.exe" — actually our
   evil binary — for the source_iso instance. evil_worker injects bad
   frames, host either tolerates or kills the worker. Then we ping.
6. Restore the original.

We try each strategy index (FUZZ_STRATEGY env var, 0..15) at least
once. Each strategy = one project-open cycle.
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from _common import BackendProc, BACKEND_EXE, WS_URL, port_open  # noqa: E402

from xinsp2 import Client, ProtocolError  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]
RELEASE_DIR = BACKEND_EXE.parent
ORIG_WORKER = RELEASE_DIR / "xinsp-worker.exe"
BACKUP_WORKER = RELEASE_DIR / "xinsp-worker.exe.fuzz-bak"
EVIL_WORKER = RELEASE_DIR / "evil_worker.exe"
PROJECT_DIR = REPO_ROOT / "examples" / "cross_proc_trigger"

ITERS = int(os.environ.get("FUZZ_ITERS", "16"))  # one per strategy


def install_evil():
    if not EVIL_WORKER.exists():
        raise FileNotFoundError(f"evil_worker.exe not at {EVIL_WORKER} — build via "
                                f"`cmake --build backend/build --config Release --target evil_worker`")
    if not ORIG_WORKER.exists():
        raise FileNotFoundError(f"xinsp-worker.exe not at {ORIG_WORKER}")
    if not BACKUP_WORKER.exists():
        shutil.copy2(ORIG_WORKER, BACKUP_WORKER)
        print(f"[swap] backed up {ORIG_WORKER.name} -> {BACKUP_WORKER.name}")
    shutil.copy2(EVIL_WORKER, ORIG_WORKER)
    print(f"[swap] installed evil_worker.exe in place of xinsp-worker.exe")


def restore_orig():
    if BACKUP_WORKER.exists():
        shutil.copy2(BACKUP_WORKER, ORIG_WORKER)
        try:
            BACKUP_WORKER.unlink()
        except Exception:
            pass
        print(f"[swap] restored original xinsp-worker.exe")


def main() -> int:
    findings: list[dict] = []
    print(f"[harness_evil_worker_host] iters={ITERS}", flush=True)

    if not PROJECT_DIR.exists():
        print(f"[FATAL] project dir missing: {PROJECT_DIR}")
        return 2

    install_evil()
    try:
        for i in range(ITERS):
            strategy_idx = i % 16
            print(f"\n--- iter {i}/{ITERS} strategy_idx={strategy_idx} ---", flush=True)
            env = os.environ.copy()
            env["FUZZ_STRATEGY"] = str(strategy_idx)
            env["FUZZ_SEED"] = str(1234 + i)

            # Spawn backend with the env propagated to children (the
            # backend will inherit env when it spawns evil_worker; that
            # FUZZ_STRATEGY is read inside evil_worker.cpp).
            # Spawn backend manually so we can inject FUZZ_STRATEGY into
            # its env (inherited by the worker it spawns). Make sure
            # nothing else holds :7823.
            if port_open():
                print(f"[warn] :7823 in use already — skipping iter {i}")
                continue
            proc = subprocess.Popen(
                [str(BACKEND_EXE)],
                cwd=str(BACKEND_EXE.parent),
                env=env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                stdin=subprocess.DEVNULL,
            )
            try:
                # wait for port
                t0 = time.time()
                while time.time() - t0 < 15.0:
                    if port_open(): break
                    if proc.poll() is not None:
                        findings.append({"iter": i, "strategy_idx": strategy_idx,
                                         "kind": "backend_died_pre_open",
                                         "rc": proc.returncode, "fatal": True})
                        raise RuntimeError("backend died before open")
                    time.sleep(0.1)
                else:
                    findings.append({"iter": i, "kind": "backend_no_listen", "fatal": True})
                    continue

                c = Client(WS_URL)
                c.connect()
                try:
                    # liveness baseline
                    c.ping()

                    # Open the project — backend will spawn evil_worker
                    # for source_iso. open_project may succeed (warning)
                    # or fail (ProtocolError); we don't strictly care
                    # about success — we care that the host doesn't die.
                    open_err = None
                    try:
                        c.open_project(str(PROJECT_DIR), timeout=20.0)
                    except ProtocolError as e:
                        open_err = {"kind": "protocol_error", "msg": str(e)[:300]}
                    except Exception as e:
                        open_err = {"kind": "exception", "msg": repr(e)[:300]}

                    # Give the host a moment to digest whatever frames
                    # the worker sent / its EOF
                    time.sleep(1.0)

                    pingable = True
                    ping_exc = None
                    try:
                        c.ping()
                    except Exception as e:
                        pingable = False
                        ping_exc = repr(e)[:300]

                    proc_alive = (proc.poll() is None)

                    rec = {
                        "iter": i, "strategy_idx": strategy_idx,
                        "open_err": open_err, "pingable": pingable,
                        "ping_exc": ping_exc, "proc_alive": proc_alive,
                        "rc": proc.returncode,
                    }
                    if not proc_alive or not pingable:
                        rec["fatal"] = True
                        print(f"[CRASH] iter={i} strat={strategy_idx} "
                              f"proc_alive={proc_alive} pingable={pingable} rc={proc.returncode}", flush=True)
                    else:
                        oe_str = str(open_err) if open_err else "none"
                        print(f"[ok] iter={i} strat={strategy_idx} survived (open_err={oe_str})")
                    findings.append(rec)
                finally:
                    try: c.close()
                    except Exception: pass
            finally:
                # tear down backend
                if proc.poll() is None:
                    try:
                        # gentle shutdown
                        try:
                            import websocket
                            ws = websocket.create_connection(WS_URL, timeout=2.0)
                            ws.send(json.dumps({"type": "cmd", "id": 99999, "name": "shutdown"}))
                            ws.close()
                        except Exception:
                            pass
                        proc.wait(timeout=4.0)
                    except subprocess.TimeoutExpired:
                        proc.terminate()
                        try: proc.wait(timeout=2.0)
                        except Exception: proc.kill()
                # log progress
                try:
                    prog = REPO_ROOT / ".fl_progress" / "fl_r7_fuzz.txt"
                    with open(prog, "a", encoding="utf-8") as f:
                        f.write(f"{time.strftime('%H:%M:%S')}  evil_worker iter {i+1}/{ITERS} strat={strategy_idx}\n")
                except Exception:
                    pass
    finally:
        restore_orig()

    out = Path(__file__).parent / "_results_evil_worker.json"
    out.write_text(json.dumps({"iters": ITERS, "findings": findings}, indent=2))
    print(f"\n[harness_evil_worker_host] done. findings={len(findings)} fatal={sum(1 for f in findings if f.get('fatal'))}")
    return 0 if not any(f.get("fatal") for f in findings) else 1


if __name__ == "__main__":
    sys.exit(main())
