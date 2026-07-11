"""qa_jpeg_preview — E2 end-to-end: expose's full-resolution compressed preview
via the xi.jpeg.encode capability.

Two phases, two isolated backends (unique ports via examples/lib/ports.free_port):

  PHASE 1 (project has the `codec` imgcodec instance):
    * expose's v3 WS frames for channels "a"/"b" carry a `preview` entry
      {w,h,c,enc:"jpeg",q,data:<bin>} INSTEAD of raw pixels.
    * the preview JPEG decodes to the SOURCE dims (128x96) — FULL resolution,
      not a thumbnail — proven by reading the JPEG SOF marker (no Pillow dep).
    * the preview is much smaller than the raw plane (compression).
    * DEDUP: "a" and "b" push byte-identical content, so the provider encodes
      exactly ONCE (codec stats encodes == 1) across both channels + every tick.
    * PER-IMAGE FAIL-OPEN: channel "bad" pushes a 2-channel image imgcodec
      refuses ($fault "wrong_type"); that entry falls back to RAW px while "a"/"b"
      keep previewing — never fails to nothing.

  PHASE 2 (no_codec project — expose, NO imgcodec):
    * the capability is absent -> expose runs its persistent degraded/raw path;
      every image entry rides RAW, nothing breaks, and expose publishes a
      "jpeg preview OFF" status line.

Run:  python examples/qa_jpeg_preview/driver.py   (Windows; backend + plugins built)
"""
from __future__ import annotations

import os
import queue
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
sys.path.insert(0, str(REPO / "examples" / "lib"))
from xinsp2 import Client       # noqa: E402
from ports import free_port     # noqa: E402
from xex1 import collect_frames, jpeg_dims  # noqa: E402

BACKEND = REPO / "backend" / "build" / "Release" / "xinsp-backend.exe"
INSPECT = ROOT / "inspect.cpp"
W, H = 128, 96
RAW = W * H  # 1-channel raw plane bytes


def spawn(port: int) -> subprocess.Popen:
    log = open(ROOT / f"backend_{port}.log", "w", encoding="utf-8")
    return subprocess.Popen([str(BACKEND), f"--port={port}"], stdout=log,
                            stderr=subprocess.STDOUT, cwd=str(REPO))


def connect(port: int) -> Client | None:
    for _ in range(80):
        try:
            c = Client(url=f"ws://127.0.0.1:{port}/", timeout=60)
            c.connect(); c.ping(); return c
        except Exception:
            time.sleep(0.5)
    return None


def latest_per_channel(client, channels: set[str], seconds: float) -> dict[str, dict]:
    """Collect binary XEX1 frames for `seconds`, returning the latest decoded
    frame seen per channel of interest."""
    out: dict[str, dict] = {}
    end = time.time() + seconds
    while time.time() < end:
        for fr in collect_frames(client):
            ch = fr.get("channel")
            if ch in channels:
                out[ch] = fr
        time.sleep(0.1)
    return out


def img_entry(frame: dict, key: str = "img") -> dict | None:
    return (frame.get("images") or {}).get(key)


def phase1(fails: list[str]) -> None:
    port = free_port()
    proc = spawn(port)
    try:
        c = connect(port)
        assert c, "phase1: no connect"
        c.call("open_project", {"path": str(ROOT)})
        c.compile_and_load(str(INSPECT), timeout=300)
        for ch in ("a", "b", "bad"):
            c.exchange_instance("expose", {"command": "subscribe", "channels": [ch]})
        c.drain_binary()
        c.call("start", {"fps": 10})
        frames = latest_per_channel(c, {"a", "b", "bad"}, 6.0)
        stats = {}
        try:
            stats = c.exchange_instance("codec", {"command": "stats"}) or {}
        except Exception as e:
            fails.append(f"phase1: codec stats exchange failed: {e!r}")
        c.call("stop")

        # good channels carry a full-resolution preview
        for ch in ("a", "b"):
            fr = frames.get(ch)
            if not fr:
                fails.append(f"phase1: no frame for channel {ch!r}"); continue
            im = img_entry(fr)
            if not im:
                fails.append(f"phase1: channel {ch!r} frame has no 'img' entry"); continue
            pv = im.get("preview")
            if not pv or not pv.get("data"):
                fails.append(f"phase1: channel {ch!r} 'img' has NO preview entry"); continue
            if pv.get("enc") != "jpeg":
                fails.append(f"phase1: channel {ch!r} preview enc != jpeg: {pv.get('enc')!r}")
            # source dims are carried in the preview map (encoder reply has none)
            if (pv.get("w"), pv.get("h"), pv.get("c")) != (W, H, 1):
                fails.append(f"phase1: channel {ch!r} preview dims "
                             f"{(pv.get('w'), pv.get('h'), pv.get('c'))} != {(W, H, 1)}")
            dims = jpeg_dims(pv["data"])
            if dims != (W, H):
                fails.append(f"phase1: channel {ch!r} DECODED jpeg dims {dims} != "
                             f"source {(W, H)} (not full resolution)")
            size = len(pv["data"])
            if not (0 < size < RAW):
                fails.append(f"phase1: channel {ch!r} jpeg size {size} not < raw {RAW}")
            elif size >= int(RAW * 0.75):
                fails.append(f"phase1: channel {ch!r} jpeg size {size} not << raw {RAW} "
                             f"(weak compression)")
            # the raw px must have LEFT the wire (preview replaces it)
            if len(im.get("pixels") or b"") != 0:
                fails.append(f"phase1: channel {ch!r} still ships raw px "
                             f"({len(im['pixels'])} bytes) alongside the preview")

        # bad channel: per-image fail-open to RAW (no preview, raw px present)
        frb = frames.get("bad")
        if not frb:
            fails.append("phase1: no frame for channel 'bad'")
        else:
            imb = img_entry(frb)
            if not imb:
                fails.append("phase1: 'bad' frame has no 'img' entry")
            elif imb.get("preview"):
                fails.append("phase1: 'bad' (2-channel) image was previewed, "
                             "expected RAW fail-open")
            elif len(imb.get("pixels") or b"") != 8 * 8 * 2:
                fails.append(f"phase1: 'bad' raw px len {len(imb.get('pixels') or b'')} "
                             f"!= {8 * 8 * 2}")

        # DEDUP: two channels, identical content -> ONE encode ever
        if stats:
            if stats.get("encodes") != 1:
                fails.append(f"phase1: DEDUP broke — codec encodes={stats.get('encodes')} "
                             f"(want 1 across both channels)")
            if stats.get("registered") is not True:
                fails.append(f"phase1: codec not registered: {stats.get('registered')}")
        else:
            fails.append("phase1: no codec stats")

        print(f"phase1: channels={sorted(frames)} "
              f"a_preview={bool((img_entry(frames.get('a') or {}) or {}).get('preview'))} "
              f"stats={stats}")
    except Exception as e:
        fails.append(f"phase1: exception: {e!r}")
    finally:
        try:
            proc.terminate(); proc.wait(timeout=10)
        except Exception:
            proc.kill()


def phase2(fails: list[str]) -> None:
    # The path-containment guard (red-team fix: open_project refuses a
    # project.json `script` that escapes the project folder) forbids the old
    # `"script": "../inspect.cpp"` layout. Keep ONE source of truth for the
    # script by copying it into no_codec/ at run time instead of committing a
    # duplicate that would drift.
    shutil.copyfile(INSPECT, ROOT / "no_codec" / "inspect.cpp")
    port = free_port()
    proc = spawn(port)
    try:
        c = connect(port)
        assert c, "phase2: no connect"
        c.call("open_project", {"path": str(ROOT / "no_codec")})
        c.compile_and_load(str(INSPECT), timeout=300)
        for ch in ("a", "b"):
            c.exchange_instance("expose", {"command": "subscribe", "channels": [ch]})
        c.drain_binary()
        c.call("start", {"fps": 10})
        frames = latest_per_channel(c, {"a", "b"}, 5.0)
        status = {}
        try:
            status = c.status() or {}
        except Exception as e:
            fails.append(f"phase2: status() failed: {e!r}")
        c.call("stop")

        for ch in ("a", "b"):
            fr = frames.get(ch)
            if not fr:
                fails.append(f"phase2: no frame for channel {ch!r}"); continue
            im = img_entry(fr)
            if not im:
                fails.append(f"phase2: channel {ch!r} frame has no 'img' entry"); continue
            if im.get("preview"):
                fails.append(f"phase2: channel {ch!r} previewed with NO codec present "
                             f"(fallback leg broken)")
            if len(im.get("pixels") or b"") != RAW:
                fails.append(f"phase2: channel {ch!r} raw px len "
                             f"{len(im.get('pixels') or b'')} != {RAW}")

        # persistent degraded mode should have published a status line
        exp = (status.get("expose") or {}).get("text", "")
        if "preview OFF" not in exp:
            fails.append(f"phase2: expose status did not report degraded/raw fallback: {exp!r}")
        print(f"phase2: channels={sorted(frames)} expose_status={exp!r}")
    except Exception as e:
        fails.append(f"phase2: exception: {e!r}")
    finally:
        try:
            proc.terminate(); proc.wait(timeout=10)
        except Exception:
            proc.kill()


def main() -> int:
    if os.name != "nt":
        print("SKIP: Windows-only"); return 0
    if not BACKEND.exists():
        print(f"SKIP: backend not built ({BACKEND})"); return 0

    fails: list[str] = []
    phase1(fails)
    phase2(fails)

    if fails:
        for f in fails:
            print("  -", f)
        print("VERDICT: FAIL: E2 jpeg preview (full-res compressed preview via xi.jpeg.encode)")
        return 1
    print("VERDICT: PASS: expose ships a FULL-RES compressed 'preview' via xi.jpeg.encode "
          "(dims==source, size<<raw, ONE encode dedups both channels), fails OPEN to raw "
          "per-image on a codec $fault, and runs a persistent raw/degraded path with no "
          "imgcodec provider")
    return 0


if __name__ == "__main__":
    sys.exit(main())
