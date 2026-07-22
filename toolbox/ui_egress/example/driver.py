"""ui_egress example — proof that the live view costs what it claims to cost.

  NOBODY WATCHING     with no subscriber on ui/cam, the camera pushes hundreds
                      of frames and egress encodes ZERO of them. A live view
                      nobody is looking at must be free, or it is not a live
                      view, it is a tax.

  THE UI RATE WINS    subscribed, a 30fps source arrives at ~5fps (the egress
                      instance's own rate), carrying a full-resolution JPEG.
                      Pushes must far exceed deliveries: that gap is the
                      latest-wins slot collapsing the stream instead of
                      queueing it back into the camera.

  NO EGRESS, NO       the same project without the `ui_egress` instance: the
  PROBLEM             camera's push is a silent no-op, ui/cam delivers nothing
                      at all, and the PRODUCT plane (this script's frames, with
                      strictly increasing seq) is untouched.

The provider-less half runs against a throwaway COPY of this project in the
temp dir, so the driver never edits the example it is testing.

Run:  python toolbox/ui_egress/example/driver.py
"""
from __future__ import annotations
import json, os, shutil, sys, tempfile, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[2]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
sys.path.insert(0, str(REPO / "qa" / "lib"))
from ports import free_port          # noqa: E402
from backends import backend_built, spawn_backend, connect  # noqa: E402
from xex1 import collect_frames, jpeg_dims, subscribe       # noqa: E402

W, H = 320, 240
CAM_FPS = 30
UI_FPS = 5


def gather(c, channels: set[str], seconds: float) -> dict[str, list[dict]]:
    """Collect decoded XEX1 frames for `seconds`, bucketed by channel."""
    out: dict[str, list[dict]] = {ch: [] for ch in channels}
    end = time.time() + seconds
    while time.time() < end:
        for fr in collect_frames(c):
            if fr.get("channel") in out:
                out[fr["channel"]].append(fr)
        time.sleep(0.05)
    return out


def stats(c) -> dict:
    try:
        return c.exchange_instance("egress", {"command": "stats"}) or {}
    except Exception:
        return {}


def project_without_egress(port: int) -> Path:
    """A throwaway copy of this project with the ui_egress instance removed."""
    dst = Path(tempfile.gettempdir()) / f"xi_uiegress_ex_none_{port}"
    shutil.rmtree(dst, ignore_errors=True)
    dst.mkdir(parents=True)
    shutil.copyfile(ROOT / "inspect.cpp", dst / "inspect.cpp")
    for inst in ("cam", "codec", "view"):
        shutil.copytree(ROOT / "instances" / inst, dst / "instances" / inst)
    proj = json.loads((ROOT / "project.json").read_text())
    proj["name"] = "ui_egress_example_none"
    proj["instances"] = [i for i in proj["instances"] if i["plugin"] != "ui_egress"]
    (dst / "project.json").write_text(json.dumps(proj, indent=2))
    return dst


def phase_live(fails: list[str], port: int) -> None:
    proc = spawn_backend(port, ROOT / f"backend_{port}.log", tag="xi_uiegress_ex")
    try:
        c = connect(port)
        assert c, "no connect"
        c.call("open_project", {"path": str(ROOT)})
        c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=300)
        c.call("start", {"fps": 0})            # the camera drives
        c.drain_binary()
        c.exchange_instance("cam", {"command": "start"})

        # -- nobody watching: the camera streams into a channel with no client
        time.sleep(2.0)
        idle = stats(c)
        print(f"unwatched: {idle}")
        if int(idle.get("pushes", 0)) <= 0:
            fails.append(f"camera never pushed a preview: {idle!r}")
        if int(idle.get("dropped_no_sub", 0)) <= 0:
            fails.append("the no-subscriber path was never taken "
                         f"(dropped_no_sub={idle.get('dropped_no_sub')})")
        if int(idle.get("encodes", 0)) != 0:
            fails.append(f"encoded {idle.get('encodes')} frames with nobody "
                         "watching — the live view is not free")

        # -- watching: the UI rate, not the camera rate
        subscribe(c, ["ui/cam"], inst="view")
        c.drain_binary()
        base = stats(c)
        T = 4.0
        got = gather(c, {"ui/cam"}, T)
        now = stats(c)
        c.exchange_instance("cam", {"command": "stop"})
        c.call("stop")

        frames = got["ui/cam"]
        pushed = int(now.get("pushes", 0)) - int(base.get("pushes", 0))
        encoded = int(now.get("encodes", 0)) - int(base.get("encodes", 0))
        print(f"watched:   delivered={len(frames)} in {T}s  pushed={pushed}  "
              f"encoded={encoded}")

        if len(frames) < UI_FPS:
            fails.append(f"only {len(frames)} frames delivered in {T}s — the "
                         "live edge is dead")
        if len(frames) > int(UI_FPS * T * 2) + 4:
            fails.append(f"{len(frames)} frames in {T}s is not capped at the "
                         f"egress rate ({UI_FPS}fps)")
        if pushed < int(CAM_FPS * T * 0.5):
            fails.append(f"source pushed only {pushed} in {T}s — the camera is "
                         "not actually running fast")
        if pushed <= len(frames) * 2:
            fails.append(f"pushed {pushed} vs delivered {len(frames)}: the "
                         "latest-wins slot never collapsed anything")
        if encoded <= 0:
            fails.append("nothing was encoded while watching")

        im = ((frames[-1].get("images") or {}).get("img") or {}) if frames else {}
        pv = im.get("preview") or {}
        if not pv.get("data"):
            fails.append("the delivered frame carries no jpeg preview")
        elif jpeg_dims(pv["data"]) != (W, H):
            fails.append(f"preview decodes to {jpeg_dims(pv['data'])}, not the "
                         f"source {(W, H)}")

        c.call("close_project"); c.close()
    except Exception as e:
        fails.append(f"live: {e!r}")
    finally:
        proc.terminate()
        try: proc.wait(5)
        except Exception: proc.kill()


def phase_no_egress(fails: list[str], port: int) -> None:
    proj = project_without_egress(port)
    proc = spawn_backend(port, ROOT / f"backend_{port}.log", tag="xi_uiegress_ex")
    try:
        c = connect(port)
        assert c, "no connect"
        c.call("open_project", {"path": str(proj)})
        c.compile_and_load(str(proj / "inspect.cpp"), timeout=300)
        subscribe(c, ["cam", "ui/cam"], inst="view")   # product AND live view
        c.call("start", {"fps": 0})
        c.drain_binary()
        c.exchange_instance("cam", {"command": "start"})

        got = gather(c, {"cam", "ui/cam"}, 4.0)
        c.exchange_instance("cam", {"command": "stop"})
        c.call("stop")
        prod, live = got["cam"], got["ui/cam"]
        print(f"no egress: product={len(prod)} live={len(live)}")

        if live:
            fails.append(f"{len(live)} live frames with NO egress provider — "
                         "they cannot have come from anywhere")
        if len(prod) < 5:
            fails.append(f"only {len(prod)} product frames — the pipeline did "
                         "not survive the capability being absent")
        else:
            seqs = [f.get("values", {}).get("seq") for f in prod]
            if any(s is None for s in seqs):
                fails.append("a product frame lost its seq")
            elif seqs != sorted(seqs) or len(set(seqs)) != len(seqs):
                fails.append(f"product seq not strictly increasing: {seqs[:8]}")
            dims = (prod[-1].get("values", {}).get("w"),
                    prod[-1].get("values", {}).get("h"))
            if dims != (W, H):
                fails.append(f"product frame dims {dims} != {(W, H)}")

        c.call("close_project"); c.close()
    except Exception as e:
        fails.append(f"no_egress: {e!r}")
    finally:
        proc.terminate()
        try: proc.wait(5)
        except Exception: proc.kill()


def main() -> int:
    if not backend_built():
        print("SKIP: backend not built"); return 0
    fails: list[str] = []
    phase_live(fails, int(os.environ.get("PORT", "0")) or free_port())
    phase_no_egress(fails, free_port())
    print("VERDICT:", "PASS" if not fails else "FAIL: " + "; ".join(fails))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
