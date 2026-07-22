"""backends.py — spawn/connect helpers for driver scripts.

WHERE the executables are is `ports.py`'s job (`ports.backend_exe()` and
friends probe build-linux / build/Release / build/Debug / build, cross-platform)
— the 39 ported qa drivers already resolve through it, so there is exactly one
answer to "which exe" in this tree. This module adds the two steps every driver
then repeats: spawning the backend with an isolated temp dir, and polling until
it accepts a WS client.
"""
from __future__ import annotations
import os
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from ports import backend_exe, runner_exe, fe_exe  # noqa: E402,F401 (re-exported)


def backend_built() -> bool:
    """True when this checkout has a built backend (drivers SKIP when it doesn't)."""
    override = os.environ.get("XINSP2_BACKEND_EXE")
    if override:
        return Path(override).exists()
    return backend_exe().exists()


def spawn_backend(port: int, log_path: Path, tag: str = "xi") -> subprocess.Popen:
    """Spawn a backend on `port`, logging to `log_path`.

    Runs with a PRIVATE temp dir so a driver's JIT script build cannot collide
    with a parallel driver's (run_qa runs a pool). TMPDIR is the POSIX knob,
    TEMP/TMP the Windows one — set all three and let each platform read its own.
    """
    exe = Path(os.environ.get("XINSP2_BACKEND_EXE") or backend_exe())
    if not exe.exists():
        raise FileNotFoundError(f"no backend built at {exe} "
                                "(set XINSP2_BACKEND_EXE to override)")
    iso = Path(tempfile.gettempdir()) / f"{tag}_iso_{port}"
    iso.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env["TMPDIR"] = env["TEMP"] = env["TMP"] = str(iso)
    log = open(log_path, "w", encoding="utf-8")
    return subprocess.Popen([str(exe), f"--port={port}"], stdout=log,
                            stderr=subprocess.STDOUT, cwd=str(REPO), env=env)


def connect(port: int, attempts: int = 80, timeout: int = 60):
    """Poll until the backend accepts a WS client. Returns a Client or None."""
    sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
    from xinsp2 import Client  # noqa: E402  (path set above)
    import time
    for _ in range(attempts):
        try:
            c = Client(url=f"ws://127.0.0.1:{port}/", timeout=timeout)
            c.connect()
            c.ping()
            return c
        except Exception:
            time.sleep(0.5)
    return None
