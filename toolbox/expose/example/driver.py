"""expose example — proof that SUBSCRIPTION, not the push, gates the wire.

The script pushes BOTH channels ("measure", "detail") on every single run. The
driver subscribes to `measure` only and asserts the negative half first: zero
`detail` frames ever reach the socket, while `list_channels` shows `detail`
being written the whole time and `get` pulls its latest frame on demand. Then
it subscribes to `detail` and asserts the frames start; then unsubscribes and
asserts they stop again.

Only checking that a subscribed channel arrives would pass for a backend that
broadcast everything unconditionally. The gate is the claim, so the gate is
what gets asserted — in both directions.

Run:  python toolbox/expose/example/driver.py
"""
from __future__ import annotations
import os, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[2]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
sys.path.insert(0, str(REPO / "qa" / "lib"))
from ports import free_port          # noqa: E402
from backends import backend_built, spawn_backend, connect  # noqa: E402
from xex1 import (collect_frames, subscribe, unsubscribe,   # noqa: E402
                  list_channels, pull_latest)

PORT = int(os.environ.get("PORT", "0")) or free_port()


def gather(c, seconds: float) -> dict:
    """Collect pushed XEX1 frames for `seconds`, bucketed by channel."""
    out: dict[str, list] = {}
    end = time.time() + seconds
    while time.time() < end:
        for fr in collect_frames(c):
            out.setdefault(fr.get("channel"), []).append(fr)
        time.sleep(0.05)
    return out


def main() -> int:
    if not backend_built():
        print("SKIP: backend not built"); return 0
    fails: list[str] = []
    proc = spawn_backend(PORT, ROOT / f"backend_{PORT}.log", tag="xi_expose_ex")
    try:
        c = connect(PORT)
        assert c, "no connect"
        c.call("open_project", {"path": str(ROOT)})
        c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=300)

        # Subscribe to ONE of the two channels the script pushes.
        subscribe(c, ["measure"])

        c.call("start", {"fps": 0})           # trigger-only: the camera drives
        c.drain_binary()                      # zero the binary baseline
        c.call("exchange_instance", {"name": "cam", "cmd": {"command": "start"}})

        # ---- phase A: measure subscribed, detail NOT ------------------------
        a = gather(c, 3.0)
        n_measure_a = len(a.get("measure", []))
        n_detail_a  = len(a.get("detail", []))
        print(f"A  subscribed=[measure]   measure={n_measure_a}  detail={n_detail_a}")

        if n_measure_a < 5:
            fails.append(f"phase A: only {n_measure_a} measure frames — the "
                         "subscribed channel barely streamed")
        if n_detail_a:
            fails.append(f"phase A: {n_detail_a} detail frames arrived while "
                         "UNSUBSCRIBED — the subscription gate does not gate")

        # The unsubscribed channel is still being WRITTEN — gating is on the
        # push, not on the record. list_channels sees it; `get` pulls it.
        chans = list_channels(c) or {}
        seen = (chans.get("channels") or {})
        print(f"A  list_channels -> {seen}")
        det = seen.get("detail") or {}
        if not det:
            fails.append("phase A: expose never saw the detail channel at all — "
                         "the script did not push it")
        elif det.get("seen", 0) < 5:
            fails.append(f"phase A: detail seen={det.get('seen')} — the record "
                         "was not stored while unsubscribed")
        elif det.get("subscribed") is not False:
            fails.append(f"phase A: detail reports subscribed={det.get('subscribed')}")

        pulled = pull_latest(c, "detail")
        imgs = sorted((pulled or {}).get("images", {}).keys())
        print(f"A  get(detail) -> images={imgs} values={sorted((pulled or {}).get('values', {}))}")
        if not pulled:
            fails.append("phase A: get(detail) found nothing — the pull view "
                         "does not work for an unsubscribed channel")
        elif imgs != ["frame", "mask"]:
            fails.append(f"phase A: pulled detail frame has images {imgs}, "
                         "want both 'frame' and 'mask' in the one record")

        # ---- phase B: subscribe detail too — it must start flowing ----------
        subscribe(c, ["measure", "detail"])
        c.drain_binary()                      # ignore anything already in flight
        b = gather(c, 3.0)
        n_measure_b = len(b.get("measure", []))
        n_detail_b  = len(b.get("detail", []))
        print(f"B  subscribed=[measure,detail]  measure={n_measure_b}  detail={n_detail_b}")
        if n_detail_b < 5:
            fails.append(f"phase B: only {n_detail_b} detail frames after "
                         "subscribing — the gate did not open")
        else:
            bad = [sorted(f.get("images", {}).keys()) for f in b["detail"][:5]]
            if any(k != ["frame", "mask"] for k in bad):
                fails.append(f"phase B: pushed detail frames lack both images: {bad}")
            vals = b["detail"][0].get("values") or {}
            if "seq" not in vals or "threshold" not in vals:
                fails.append(f"phase B: detail values missing seq/threshold: {vals}")

        # ---- phase C: unsubscribe detail — it must stop again ----------------
        unsubscribe(c, ["detail"])
        c.drain_binary()
        time.sleep(0.4)                       # let in-flight pushes land
        c.drain_binary()
        d = gather(c, 2.0)
        n_measure_c = len(d.get("measure", []))
        n_detail_c  = len(d.get("detail", []))
        print(f"C  subscribed=[measure]   measure={n_measure_c}  detail={n_detail_c}")
        if n_detail_c:
            fails.append(f"phase C: {n_detail_c} detail frames after "
                         "unsubscribe — the gate does not close")
        if n_measure_c < 3:
            fails.append(f"phase C: measure stopped too ({n_measure_c}) — "
                         "unsubscribing one channel took out the other")

        c.call("exchange_instance", {"name": "cam", "cmd": {"command": "stop"}})
        c.call("stop")
        c.call("close_project"); c.close()
    except Exception as e:
        fails.append(f"{e}")
    finally:
        proc.terminate()
        try: proc.wait(5)
        except Exception: proc.kill()

    print("VERDICT:", "PASS" if not fails else "FAIL: " + "; ".join(fails))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
