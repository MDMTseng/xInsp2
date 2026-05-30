"""Driver for object_count_puzzle.

Orchestration ONLY: launches a private backend, opens the project, compiles
inspect.cpp, runs all 8 frames, reads the count/centroids VARs the C++ script
emits, and writes predictions.json. No detection logic lives here.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).parent.resolve()
REPO = ROOT.parents[1]
SDK = REPO / "tools" / "xinsp2_py"
BACKEND = REPO / "backend" / "build" / "Release" / "xinsp-backend.exe"
FRAMES = ROOT / "frames"
INSPECT = ROOT / "inspect.cpp"
PORT = int(os.environ.get("PUZZLE_PORT", "7867"))

sys.path.insert(0, str(SDK))
from xinsp2 import Client, ProtocolError  # noqa: E402


def main():
    log = open(ROOT / "backend.scratch.log", "w", encoding="utf-8")
    proc = subprocess.Popen(
        [str(BACKEND), f"--port={PORT}"],
        stdout=log, stderr=subprocess.STDOUT, cwd=str(REPO),
    )
    print(f"spawned backend pid={proc.pid} on port {PORT}")
    try:
        # wait for the port
        url = f"ws://127.0.0.1:{PORT}/"
        c = None
        last_err = None
        for _ in range(60):
            try:
                c = Client(url=url, timeout=120.0)
                c.connect()
                c.ping()
                break
            except Exception as e:  # noqa: BLE001
                last_err = e
                c = None
                time.sleep(0.5)
        if c is None:
            raise RuntimeError(f"backend never came up: {last_err}")

        logs = []
        c.on_log(lambda m: logs.append(m))

        print("open_project")
        c.open_project(str(ROOT), timeout=300)
        print("compile_and_load")
        try:
            c.compile_and_load(str(INSPECT))
        except ProtocolError as e:
            print("COMPILE FAILED:", e)
            for m in logs[-40:]:
                if m.get("level") in ("error", "warn"):
                    print("  log:", m.get("msg"))
            raise

        frames_out = []
        for i in range(8):
            fname = f"frame_{i:02d}.png"
            fp = str((FRAMES / fname).resolve())
            r = c.run(frame_path=fp, timeout=120.0)
            count = r.value("count", -1)
            cents_raw = r.value("centroids", "[]")
            otsu = r.value("otsu", None)
            try:
                cents = json.loads(cents_raw)
            except Exception:
                cents = []
            frames_out.append({"file": fname, "count": int(count),
                               "centroids": cents})
            print(f"  {fname}: count={count} otsu={otsu} ncent={len(cents)}")

        out = {"frames": frames_out}
        (ROOT / "predictions.json").write_text(
            json.dumps(out, indent=2), encoding="utf-8")
        print(f"wrote {ROOT / 'predictions.json'}")

        try:
            c.close()
        except Exception:
            pass
    finally:
        # only kill the backend WE spawned, by its known PID
        try:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        except Exception as e:  # noqa: BLE001
            print("backend cleanup warning:", e)
        log.close()
        print(f"backend pid={proc.pid} cleaned up")


if __name__ == "__main__":
    main()
