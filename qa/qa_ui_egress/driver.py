"""qa_ui_egress — the LIVE-UI egress pipeline e2e (spec 31 /
docs/internals/ui-egress.md).

Full path exercised through the REAL backend + a WS client:

    mock_camera (ui_preview=on, fast) --push--> xi.ui.egress (own timer @ fps)
       --dedup/encode xi.jpeg.encode--> xi.ui.sink (expose) --emit_binary--> WS

Two phases, two isolated backends (unique free_port()):

  PHASE 1 (cam + ui_egress + imgcodec + expose):
    (e) NO-SUBSCRIBER -> ZERO ENCODE: the camera streams to `ui/cam` with NOBODY
        subscribed; egress probes the sink, sees 0 subscribers, and DROPS at the
        probe — stats show pushes>0, dropped_no_sub>0, encodes==0 (no encode work
        for an unwatched channel — the property the whole "probe-then-push" design
        exists to guarantee).
    (a) RATE CAP: once subscribed, a 30fps source is delivered at the egress rate
        (fps=6) — the client receives FAR fewer frames than the source pushed
        (<= ~fps*T, and pushes >> delivered: the latest-wins slot collapsed the
        stream). The delivered frame carries a full-resolution JPEG preview
        (encode happened; dims == source), proving the encode+fan-out edge.

  PHASE 2 (cam + expose; NO ui_egress, NO imgcodec) — (d) CAP-ABSENT NO-OP:
    mock_camera runs with ui_preview=on but the xi.ui.egress cap is ABSENT, so the
    push is a fail-open no-op: the PRODUCT plane is byte-intact (frames on channel
    "cam" carry correct dims + a full raw payload, seq strictly monotonic) and
    NOTHING is delivered on `ui/cam` (no egress => no live UI at all). No crash.

  PHASE 3 (cam + ui_egress + expose; NO imgcodec) — (d, second leg) FAIL-OPEN RAW:
    egress has a subscriber but no xi.jpeg.encode provider, so every frame falls
    open to RAW pixels on the wire (stats raw_fallbacks>0, encodes==0; the frame
    carries raw pixels, no jpeg preview) — nothing fails to nothing.

The deterministic LRU/slot semantics — content dedup, exact latest-wins, LRU
eviction — are pinned by the ctest unit toolbox/cap_ui_egress_test.cpp (a timer-
driven pipeline cannot make those wall-clock-deterministic from out here).

Run:  python qa/qa_ui_egress/driver.py   (Windows; backend + plugins built)
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
sys.path.insert(0, str(REPO / "qa" / "lib"))
from xinsp2 import Client       # noqa: E402
from ports import free_port     # noqa: E402
from xex1 import collect_frames, jpeg_dims, subscribe  # noqa: E402

BACKEND = REPO / "backend" / "build" / "Release" / "xinsp-backend.exe"
INSPECT = ROOT / "inspect.cpp"
W, H = 64, 48
EGRESS_FPS = 6
CAM_FPS = 30


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


def egress_stats(c) -> dict:
    try:
        return c.exchange_instance("egress", {"command": "stats"}) or {}
    except Exception:
        return {}


def collect_on(c, channels: set[str], seconds: float) -> dict[str, list[dict]]:
    """Collect XEX1 frames for `seconds`, bucketed by channel of interest."""
    out: dict[str, list[dict]] = {ch: [] for ch in channels}
    end = time.time() + seconds
    while time.time() < end:
        for fr in collect_frames(c):
            ch = fr.get("channel")
            if ch in out:
                out[ch].append(fr)
        time.sleep(0.05)
    return out


def phase1(fails: list[str]) -> None:
    port = free_port()
    proc = spawn(port)
    try:
        c = connect(port)
        assert c, "phase1: no connect"
        c.call("open_project", {"path": str(ROOT)})
        c.compile_and_load(str(INSPECT), timeout=300)
        c.call("start", {"fps": 0})           # enable the pipeline; the source drives it
        c.drain_binary()
        c.exchange_instance("cam", {"command": "start"})   # kick the capture thread

        # (e) NO SUBSCRIBER -> ZERO ENCODE: nobody is subscribed to ui/cam yet.
        time.sleep(1.4)
        s0 = egress_stats(c)
        if not s0:
            fails.append("phase1: no egress stats")
        else:
            if int(s0.get("pushes", 0)) <= 0:
                fails.append(f"phase1: camera did not push (pushes={s0.get('pushes')})")
            if int(s0.get("dropped_no_sub", 0)) <= 0:
                fails.append("phase1: no-subscriber path never exercised "
                             f"(dropped_no_sub={s0.get('dropped_no_sub')})")
            if int(s0.get("encodes", 0)) != 0:
                fails.append(f"phase1: ENCODED with no subscriber — zero-encode broken "
                             f"(encodes={s0.get('encodes')})")

        # (a) RATE CAP: subscribe and measure the delivered rate over a window.
        subscribe(c, ["ui/cam"])
        c.drain_binary()
        base = egress_stats(c)
        T = 4.0
        got = collect_on(c, {"ui/cam"}, T)
        s1 = egress_stats(c)
        c.exchange_instance("cam", {"command": "stop"})
        c.call("stop")

        frames = got["ui/cam"]
        n = len(frames)
        pushes_delta = int(s1.get("pushes", 0)) - int(base.get("pushes", 0))
        encodes_delta = int(s1.get("encodes", 0)) - int(base.get("encodes", 0))
        cap = int(EGRESS_FPS * T * 2) + 4      # generous jitter margin, still << source
        source_est = int(CAM_FPS * T * 0.5)    # a floor on what the source pushed

        if n < EGRESS_FPS:                     # delivery actually happened
            fails.append(f"phase1: too few delivered frames ({n}); the live edge is dead")
        if n > cap:                            # the rate WAS capped near fps
            fails.append(f"phase1: delivered {n} frames in {T}s > cap {cap} "
                         f"— rate cap broken (egress fps={EGRESS_FPS})")
        if pushes_delta < source_est:
            fails.append(f"phase1: source pushed only {pushes_delta} in {T}s "
                         f"(expected >= {source_est}) — producer not fast?")
        if pushes_delta <= n * 2:
            fails.append(f"phase1: pushes {pushes_delta} not >> delivered {n} "
                         "— latest-wins collapse not observed")
        if encodes_delta <= 0:
            fails.append("phase1: nothing encoded while subscribed "
                         f"(encodes_delta={encodes_delta})")

        # the delivered frame carries a full-resolution JPEG preview
        sample = frames[-1] if frames else None
        im = (sample.get("images") or {}).get("img") if sample else None
        pv = (im or {}).get("preview")
        if not pv or not pv.get("data"):
            fails.append("phase1: delivered frame has no jpeg preview entry")
        else:
            if (pv.get("w"), pv.get("h")) != (W, H):
                fails.append(f"phase1: preview dims {(pv.get('w'), pv.get('h'))} != {(W, H)}")
            dims = jpeg_dims(pv["data"])
            if dims != (W, H):
                fails.append(f"phase1: DECODED jpeg dims {dims} != source {(W, H)}")

        print(f"phase1: delivered={n} in {T}s (cap {cap}); pushes_delta={pushes_delta} "
              f"encodes_delta={encodes_delta}; zero-encode-stats={s0}")
    except Exception as e:
        fails.append(f"phase1: exception: {e!r}")
    finally:
        try:
            proc.terminate(); proc.wait(timeout=10)
        except Exception:
            proc.kill()


def phase2(fails: list[str]) -> None:
    # One source of truth for the script (path-containment guard forbids a
    # ../inspect.cpp): copy it into no_egress/ at run time, like qa_jpeg_preview.
    shutil.copyfile(INSPECT, ROOT / "no_egress" / "inspect.cpp")
    proj = ROOT / "no_egress"
    port = free_port()
    proc = spawn(port)
    try:
        c = connect(port)
        assert c, "phase2: no connect"
        c.call("open_project", {"path": str(proj)})
        c.compile_and_load(str(INSPECT), timeout=300)
        subscribe(c, ["cam", "ui/cam"])       # watch BOTH the product + the (absent) live plane
        c.call("start", {"fps": 0})
        c.drain_binary()
        c.exchange_instance("cam", {"command": "start"})

        got = collect_on(c, {"cam", "ui/cam"}, 4.0)
        c.exchange_instance("cam", {"command": "stop"})
        c.call("stop")

        prod = got["cam"]
        live = got["ui/cam"]

        # PRODUCT plane byte-intact: frames flowed, dims correct, raw payload full.
        if len(prod) < 5:
            fails.append(f"phase2: too few product frames ({len(prod)}) — "
                         "mock_camera did not run with the cap absent")
        seqs = [int((fr.get("values") or {}).get("seq", -1)) for fr in prod]
        for i in range(1, len(seqs)):
            if seqs[i] <= seqs[i - 1]:
                fails.append(f"phase2: product seq not monotonic at {i}: "
                             f"{seqs[i-1]} -> {seqs[i]}")
                break
        for fr in prod[:3]:
            im = (fr.get("images") or {}).get("img") or {}
            if (im.get("w"), im.get("h"), im.get("c")) != (W, H, 3):
                fails.append(f"phase2: product dims {(im.get('w'), im.get('h'), im.get('c'))} "
                             f"!= {(W, H, 3)}")
                break
            if len(im.get("pixels") or b"") != W * H * 3:
                fails.append(f"phase2: product raw payload len {len(im.get('pixels') or b'')} "
                             f"!= {W * H * 3} — product plane not intact")
                break

        # LIVE plane absent: with no ui_egress, the push is a no-op — nothing arrives.
        if live:
            fails.append(f"phase2: {len(live)} frames on ui/cam with NO ui_egress "
                         "— the cap-absent push was not a no-op")

        # No crash / hard fault in the backend log.
        log_txt = (ROOT / f"backend_{port}.log").read_text(encoding="utf-8", errors="replace")
        for tok in ("crashed", "terminate called", "unhandled exception", "assert failed"):
            if tok in log_txt.lower():
                fails.append(f"phase2: backend log contains {tok!r}")

        print(f"phase2: product_frames={len(prod)} seq0={seqs[0] if seqs else None} "
              f"live_frames={len(live)} (want 0)")
    except Exception as e:
        fails.append(f"phase2: exception: {e!r}")
    finally:
        try:
            proc.terminate(); proc.wait(timeout=10)
        except Exception:
            proc.kill()


def phase3(fails: list[str]) -> None:
    # (d, second leg) NO imgcodec: egress has a subscriber but no xi.jpeg.encode
    # provider, so every frame FAILS OPEN to raw pixels on the wire (raw_fallbacks
    # climbs, encodes stays 0) — nothing fails to nothing.
    shutil.copyfile(INSPECT, ROOT / "no_codec" / "inspect.cpp")
    proj = ROOT / "no_codec"
    port = free_port()
    proc = spawn(port)
    try:
        c = connect(port)
        assert c, "phase3: no connect"
        c.call("open_project", {"path": str(proj)})
        c.compile_and_load(str(INSPECT), timeout=300)
        subscribe(c, ["ui/cam"])
        c.call("start", {"fps": 0})
        c.drain_binary()
        c.exchange_instance("cam", {"command": "start"})

        got = collect_on(c, {"ui/cam"}, 3.0)
        s = egress_stats(c)
        c.exchange_instance("cam", {"command": "stop"})
        c.call("stop")

        frames = got["ui/cam"]
        if len(frames) < 3:
            fails.append(f"phase3: too few delivered frames ({len(frames)}) with no codec")
        if int(s.get("raw_fallbacks", 0)) <= 0:
            fails.append(f"phase3: no raw fallback with imgcodec absent "
                         f"(raw_fallbacks={s.get('raw_fallbacks')})")
        if int(s.get("encodes", 0)) != 0:
            fails.append(f"phase3: ENCODED with no codec present (encodes={s.get('encodes')})")
        # the delivered frame rides RAW pixels (no jpeg preview).
        im = (frames[-1].get("images") or {}).get("img") if frames else None
        if im is not None:
            if (im or {}).get("preview"):
                fails.append("phase3: frame previewed with NO codec (raw fallback broken)")
            if len(im.get("pixels") or b"") != W * H * 3:
                fails.append(f"phase3: raw payload len {len(im.get('pixels') or b'')} != {W*H*3}")
        print(f"phase3: delivered={len(frames)} raw_fallbacks={s.get('raw_fallbacks')} "
              f"encodes={s.get('encodes')}")
    except Exception as e:
        fails.append(f"phase3: exception: {e!r}")
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
    phase3(fails)

    if fails:
        for f in fails:
            print("  -", f)
        print("VERDICT: FAIL: qa_ui_egress (live-UI egress pipeline)")
        return 1
    print("VERDICT: PASS: xi.ui.egress rate-caps a fast source to the UI rate with a "
          "full-res jpeg preview, does ZERO encode work with no subscriber, fails OPEN "
          "to raw with no imgcodec, and is a clean no-op (product plane byte-intact) "
          "when the egress cap itself is absent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
