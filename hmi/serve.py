"""
HMI demo runner: spawn the backend headless on the demo project (continuous via
--autostart-fps), serve this hmi/ folder over HTTP, and print the URL to open.

  python hmi/serve.py

Then open the printed URL in a browser. The HMI is the single WS client of the
backend (the production topology — FE/autostart drives the run, the HMI displays).
Ctrl-C to stop. Windows; backend must be built.
"""
from __future__ import annotations
import functools, http.server, os, socketserver, subprocess, sys, threading, webbrowser
from pathlib import Path

HMI = Path(__file__).resolve().parent
REPO = HMI.parent
BACKEND = REPO / "backend" / "build" / "Release" / "xinsp-backend.exe"
DEMO = HMI / "demo"
WS_PORT = int(os.environ.get("WS_PORT", "7872"))
HTTP_PORT = int(os.environ.get("HTTP_PORT", "8770"))
FPS = int(os.environ.get("FPS", "5"))


def main() -> int:
    if os.name != "nt":
        print("SKIP: backend is Windows-only for now"); return 0
    if not BACKEND.exists():
        print(f"backend not built: {BACKEND}"); return 1

    # Isolated work dir so we don't collide with a dev backend's TEMP/xinsp2.
    iso = Path(os.environ["LOCALAPPDATA"]) / "Temp" / "xi_hmi_demo_tmp"
    iso.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ); env["TEMP"] = env["TMP"] = str(iso)

    log = open(HMI / "demo" / "backend.log", "w", encoding="utf-8")
    be = subprocess.Popen(
        [str(BACKEND), f"--port={WS_PORT}", f"--project={DEMO}", f"--autostart-fps={FPS}"],
        stdout=log, stderr=subprocess.STDOUT, cwd=str(REPO), env=env)

    # Serve hmi/ so the ES modules load (file:// can't).
    Handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(HMI))
    httpd = socketserver.TCPServer(("127.0.0.1", HTTP_PORT), Handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()

    url = f"http://127.0.0.1:{HTTP_PORT}/index.html?ws=ws://127.0.0.1:{WS_PORT}/"
    print("\n  xInsp2 HMI demo running.")
    print(f"  backend  : ws://127.0.0.1:{WS_PORT}/  (project=hmi/demo, {FPS} fps)")
    print(f"  open this: {url}\n  Ctrl-C to stop.\n")
    try:
        webbrowser.open(url)
    except Exception:
        pass
    try:
        be.wait()
    except KeyboardInterrupt:
        pass
    finally:
        httpd.shutdown()
        be.terminate()
        try: be.wait(5)
        except Exception: be.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
