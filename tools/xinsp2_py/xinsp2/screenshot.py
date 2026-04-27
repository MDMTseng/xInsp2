"""Capture a PNG of the primary display via PowerShell + System.Drawing.

Standalone — does not go through the backend WS. Mirrors the path the
e2e suites use (`vscode-extension/test/e2e/index.cjs:takeScreenshot`)
so AI agents can see what the VS Code window looks like alongside the
VAR / preview data they get from the SDK.

Captures the full primary screen, not just the VS Code window — that's
the same trade the e2e tests make. Assumes the user has VS Code open
and visible. If not, you'll get a desktop screenshot, which is rarely
what you want, so the caller's prompt should establish that context.
"""
from __future__ import annotations

import os
import subprocess
import tempfile
import time
from pathlib import Path

_PS_TEMPLATE = r"""
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
$bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$pt = New-Object System.Drawing.Point(0, 0)
$bmp = New-Object System.Drawing.Bitmap($bounds.Width, $bounds.Height)
$gfx = [System.Drawing.Graphics]::FromImage($bmp)
$gfx.CopyFromScreen($bounds.Location, $pt, $bounds.Size)
$bmp.Save("__OUT__")
$gfx.Dispose()
$bmp.Dispose()
"""


def screenshot(path: str | Path | None = None, *, timeout: float = 15.0) -> Path:
    """Save a PNG of the primary display and return its path.

    `path=None` writes to `%TEMP%/xinsp2_screenshot_<unix_ms>.png`.

    Raises `RuntimeError` if the PowerShell capture fails (non-Windows
    host, locked workstation, or display not available).
    """
    if path is None:
        ms = int(time.time() * 1000)
        path = Path(tempfile.gettempdir()) / f"xinsp2_screenshot_{ms}.png"
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)

    ps_text = _PS_TEMPLATE.replace("__OUT__", str(path).replace("\\", "\\\\"))
    ps_file = Path(tempfile.gettempdir()) / "xinsp2_screenshot.ps1"
    ps_file.write_text(ps_text, encoding="utf-8")

    try:
        subprocess.run(
            ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(ps_file)],
            check=True, timeout=timeout, capture_output=True,
        )
    except FileNotFoundError as e:
        raise RuntimeError("screenshot requires PowerShell (Windows host)") from e
    except subprocess.CalledProcessError as e:
        raise RuntimeError(
            f"screenshot failed: {e.stderr.decode('utf-8', errors='replace')[:300]}"
        ) from e
    except subprocess.TimeoutExpired as e:
        raise RuntimeError(f"screenshot timed out after {timeout}s") from e

    if not path.exists() or path.stat().st_size == 0:
        raise RuntimeError(f"screenshot produced no output at {path}")
    return path
