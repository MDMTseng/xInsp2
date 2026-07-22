"""imgcodec example — proof that the optional capability is really optional.

Two halves, one backend, one project:

  WITH the provider   the frames expose puts on the wire carry a JPEG `preview`
                      that decodes to the SOURCE dimensions (full resolution,
                      not a thumbnail) and is a fraction of the raw plane; and
                      the provider's own books say it encoded exactly ONCE for
                      all of them (the pixels never change, and imgcodec keys
                      its memo cache on content).

  WITHOUT it          the same project, minus the `codec` instance, is opened
                      on the same backend. Frames keep arriving, now with the
                      full raw pixel plane and no preview, and expose reports
                      "jpeg preview OFF". No crash, no gap, no verdict change.

(The provider-less half runs against a throwaway COPY of this project in the
temp dir. Deleting the instance for real — `remove_instance`, or the delete
button in the UI — works exactly the same way and is worth trying by hand, but
it rewrites project.json, and a driver that edits the example it is testing is
a driver you can only run once.)

Asserting only the first half would pass on a build where the capability plane
did nothing; asserting only the second would pass on a build with no codec at
all. The claim is the pair.

Run:  python toolbox/imgcodec/example/driver.py
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

PORT = int(os.environ.get("PORT", "0")) or free_port()
W, H = 320, 240
RAW = W * H                      # 1-channel raw plane, bytes


def gather(c, seconds: float, channel: str = "part") -> list[dict]:
    """Collect decoded XEX1 frames on `channel` for `seconds`."""
    out: list[dict] = []
    end = time.time() + seconds
    while time.time() < end:
        for fr in collect_frames(c):
            if fr.get("channel") == channel:
                out.append(fr)
        time.sleep(0.05)
    return out


def img_of(fr: dict) -> dict:
    return (fr.get("images") or {}).get("img") or {}


def project_without_codec() -> Path:
    """A throwaway copy of this project with the imgcodec instance removed."""
    dst = Path(tempfile.gettempdir()) / f"xi_imgcodec_ex_nocodec_{PORT}"
    shutil.rmtree(dst, ignore_errors=True)
    dst.mkdir(parents=True)
    shutil.copyfile(ROOT / "inspect.cpp", dst / "inspect.cpp")
    shutil.copytree(ROOT / "instances" / "view", dst / "instances" / "view")
    proj = json.loads((ROOT / "project.json").read_text())
    proj["name"] = "imgcodec_example_no_codec"
    proj["instances"] = [i for i in proj["instances"] if i["plugin"] != "imgcodec"]
    (dst / "project.json").write_text(json.dumps(proj, indent=2))
    return dst


def main() -> int:
    if not backend_built():
        print("SKIP: backend not built"); return 0
    fails: list[str] = []
    proc = spawn_backend(PORT, ROOT / f"backend_{PORT}.log", tag="xi_imgcodec_ex")
    try:
        c = connect(PORT)
        assert c, "no connect"
        c.call("open_project", {"path": str(ROOT)})
        c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=300)
        subscribe(c, ["part"], inst="view")
        c.drain_binary()
        c.call("start", {"fps": 10})

        # ---- WITH the provider ------------------------------------------
        live = gather(c, 4.0)
        stats = c.exchange_instance("codec", {"command": "stats"}) or {}
        print(f"with codec:    frames={len(live)} stats={stats}")

        if len(live) < 5:
            fails.append(f"only {len(live)} frames with the codec present")
        else:
            im = img_of(live[-1])
            pv = im.get("preview") or {}
            if not pv.get("data"):
                fails.append("no jpeg preview on the wire — the capability "
                             "was never used")
            else:
                dims = jpeg_dims(pv["data"])
                if dims != (W, H):
                    fails.append(f"preview decodes to {dims}, not the source "
                                 f"{(W, H)} — not full resolution")
                if not (0 < len(pv["data"]) < RAW * 0.5):
                    fails.append(f"jpeg is {len(pv['data'])} bytes vs raw "
                                 f"{RAW} — that is not a compressed wire")
                if len(im.get("pixels") or b""):
                    fails.append("raw pixels shipped ALONGSIDE the preview — "
                                 "the encode saved nothing")
            # the dedup headline: one encode served every frame
            if stats.get("registered") is not True:
                fails.append(f"provider not registered: {stats!r}")
            if stats.get("encodes") != 1:
                fails.append(f"encodes={stats.get('encodes')}, want 1 — the "
                             "content memo cache did not dedup")
            if stats.get("hits", 0) < len(live) - 1:
                fails.append(f"hits={stats.get('hits')} for {len(live)} frames "
                             "— frames were not served from the cache")

        # ---- WITHOUT it: the same project, no provider --------------------
        c.call("stop")
        c.call("close_project")
        nocodec = project_without_codec()
        c.call("open_project", {"path": str(nocodec)})
        c.compile_and_load(str(nocodec / "inspect.cpp"), timeout=300)
        subscribe(c, ["part"], inst="view")
        c.drain_binary()
        c.call("start", {"fps": 10})

        after = gather(c, 4.0)
        status = (c.status() or {}).get("view") or {}
        print(f"no codec:      frames={len(after)} "
              f"view_status={status.get('text')!r}")

        if len(after) < 5:
            fails.append(f"only {len(after)} frames with no codec present "
                         "— the pipeline did not survive losing the provider")
        else:
            im = img_of(after[-1])
            if im.get("preview"):
                fails.append("still previewing with NO provider present — the "
                             "fallback never engaged")
            if len(im.get("pixels") or b"") != RAW:
                fails.append(f"raw plane is {len(im.get('pixels') or b'')} "
                             f"bytes, want {RAW} — the product plane changed")
            if "preview OFF" not in (status.get("text") or ""):
                fails.append("expose never said it had degraded: "
                             f"{status.get('text')!r}")

        c.call("stop")
        c.call("close_project"); c.close()
    except Exception as e:
        fails.append(f"{e!r}")
    finally:
        proc.terminate()
        try: proc.wait(5)
        except Exception: proc.kill()

    print("VERDICT:", "PASS" if not fails else "FAIL: " + "; ".join(fails))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
