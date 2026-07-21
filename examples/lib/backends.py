"""backends.py — locate + spawn the built backend, whatever the build layout.

The exe name and the directory both differ per generator: the Windows/MSVC
multi-config build writes backend/build/<Config>/xinsp-backend.exe, the
Linux/macOS Ninja build is single-config and writes backend/build/xinsp-backend.
Drivers should not each re-derive that. Mirrors the same convention as
tests/fuzz/_common.py, ui-components/test/backend-exe.mjs and the extension's
findBackendExe: XINSP2_BACKEND_EXE wins, then Release, then Debug, then the
single-config dir.
"""
from __future__ import annotations
import os
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
_EXE = "xinsp-backend.exe" if os.name == "nt" else "xinsp-backend"


def backend_exe() -> Path | None:
    """The built backend, or None if this checkout has no build."""
    override = os.environ.get("XINSP2_BACKEND_EXE")
    if override:
        p = Path(override)
        return p if p.exists() else None
    base = REPO / "backend" / "build"
    for cfg in ("Release", "Debug", ""):
        cand = base / cfg / _EXE if cfg else base / _EXE
        if cand.exists():
            return cand
    return None


def runner_exe() -> Path | None:
    """The headless runner, same layout rules as backend_exe()."""
    name = "xinsp-runner.exe" if os.name == "nt" else "xinsp-runner"
    base = REPO / "backend" / "build"
    for cfg in ("Release", "Debug", ""):
        cand = base / cfg / name if cfg else base / name
        if cand.exists():
            return cand
    return None


def spawn_backend(port: int, log_path: Path, tag: str = "xi") -> subprocess.Popen:
    """Spawn a backend on `port`, logging to `log_path`.

    Runs with a PRIVATE temp dir so a driver's JIT script build cannot collide
    with a parallel driver's (run_qa runs a pool). TMPDIR is the POSIX knob,
    TEMP/TMP the Windows one — set all three and let each platform read its own.
    """
    exe = backend_exe()
    if exe is None:
        raise FileNotFoundError(
            f"no backend built (looked for backend/build/[Release|Debug/]{_EXE}; "
            "set XINSP2_BACKEND_EXE to override)")
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
