"""Driver for the image_sources demo (local_image_source plugin).

Proves the loop: open project (compiles the local_image_source plugin) -> point
it at a folder of images -> the webui-equivalent exchange returns thumbnails ->
"issue" an image -> a pipeline pass runs on it and the script reads it back.

No GUI: drives the same exchange commands the webui would, over WS.
"""
from __future__ import annotations
import json, os, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
SDK = REPO / "tools" / "xinsp2_py"
sys.path.insert(0, str(SDK))
from xinsp2 import Client  # noqa: E402

BACKEND = REPO / "backend" / "build" / "Release" / "xinsp-backend.exe"
FRAMES = REPO / "examples" / "object_count_solve" / "frames"   # a folder of PNGs
PORT = int(os.environ.get("PORT", "7891"))


def main() -> int:
    if os.name != "nt":
        print("SKIP: backend is Windows-only"); return 0
    if not FRAMES.is_dir():
        print(f"SKIP: no frames folder at {FRAMES} (run object_count_solve/generate_puzzle.py)"); return 0
    log = open(ROOT / "backend.log", "w", encoding="utf-8")
    proc = subprocess.Popen([str(BACKEND), f"--port={PORT}"], stdout=log,
                            stderr=subprocess.STDOUT, cwd=str(REPO))
    failures: list[str] = []
    try:
        c = None
        for _ in range(60):
            try:
                c = Client(url=f"ws://127.0.0.1:{PORT}/", timeout=120); c.connect(); c.ping(); break
            except Exception:
                time.sleep(0.5); c = None
        if c is None:
            print("FAIL: backend never came up"); return 1

        print("open_project (compiles local_image_source plugin)")
        c.open_project(str(ROOT), timeout=300)

        def exch(cmd: dict) -> dict:
            r = c.call("exchange_instance", {"name": "local", "cmd": cmd})
            data = r if isinstance(r, dict) else {}
            try:
                return json.loads(data) if isinstance(data, str) else (data or {})
            except Exception:
                return {}

        # Point the source at the frames folder (what "Set folder" does in the UI).
        st = exch({"command": "set_dir", "value": str(FRAMES)})
        files = st.get("files", [])
        print(f"  dir={st.get('dir')}  files={st.get('count')}")
        if not files:
            failures.append(f"no files listed for {FRAMES}")
        else:
            with_thumb = sum(1 for f in files if (f.get("thumb") or "").startswith("data:image/jpeg;base64,"))
            print(f"  thumbnails: {with_thumb}/{len(files)}  (first: {files[0].get('name')} {files[0].get('w')}x{files[0].get('h')})")
            if with_thumb == 0:
                failures.append("no thumbnails encoded")

        print("compile_and_load inspect.cpp")
        c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=300)
        c.call("start", {"fps": 4})   # continuous: workers run; emitted triggers dispatch
        time.sleep(0.5)
        # Drain any synthetic ticks already queued so we start clean.
        while c.next_vars(timeout=0.2) is not None:
            pass

        print("issue index 0 (== clicking the first thumbnail)")
        exch({"command": "issue", "index": 0})

        # Drain vars until we see the issued pass (loaded=true from source 'local').
        got = None
        deadline = time.time() + 10
        seen = 0
        while time.time() < deadline:
            v = c.next_vars(timeout=2)
            if v is None:
                continue
            seen += 1
            raw = v.get("items", v.get("vars", []))   # continuous vars msg uses "items"
            items = {it["name"]: it for it in raw}
            src = items.get("source", {}).get("value")
            ld  = items.get("loaded", {}).get("value")
            if ld is True:
                got = items
                break
        if got is None:
            failures.append("issued image never produced a pass with loaded=true")
        else:
            src = got.get("source", {}).get("value")
            w = got.get("width", {}).get("value")
            h = got.get("height", {}).get("value")
            print(f"  pass ran: source={src} {w}x{h} trigger_id={got.get('trigger_id',{}).get('value','')[:12]}…")
            if "local" not in str(src):
                failures.append(f"pass source not 'local': {src!r}")
            if not (isinstance(w, (int, float)) and w > 0):
                failures.append(f"bad width: {w!r}")
        try: c.close()
        except Exception: pass
    finally:
        proc.terminate()
        try: proc.wait(timeout=8)
        except subprocess.TimeoutExpired: proc.kill()
        log.close()

    print("\n" + "=" * 48)
    if failures:
        print("VERDICT: FAIL")
        for f in failures: print("  -", f)
    else:
        print("VERDICT: PASS")
        print("  local images listed with thumbnails; issuing one ran a pass on it.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
