"""record_replay example — proof that the recorded run replays, once, exactly.

The project ships five `.xex1` captures in `recorded/` and no camera. This
driver decodes those files itself and then asserts the graph reproduced them:

  positive half  every file arrives as a real trigger — right seq, right
                 checksum, right nested meta, entry-for-entry — and each one
                 arrives EXACTLY once, in file order;
  negative half  with `loop: false` the source then STOPS. The pump keeps
                 ticking for another 3s and nothing more arrives; the cursor
                 reports position == total. A replay that quietly wrapped and
                 re-fed you frame 1 would pass a "did I get 5 frames?" check.

Run:  python toolbox/record_replay/example/driver.py
"""
from __future__ import annotations
import json, os, re, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[2]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
sys.path.insert(0, str(REPO / "qa" / "lib"))
from ports import free_port          # noqa: E402
from backends import backend_built, spawn_backend, connect  # noqa: E402
from xex1 import decode_xex1         # noqa: E402

PORT = int(os.environ.get("PORT", "0")) or free_port()
RECORDED = ROOT / "recorded"         # must match instances/replay/instance.json

REP_RE = re.compile(
    r"replay seq=(-?\d+) src=(\S+) chan=(\d) psum=(\d)\((-?\d+)\) meta=(\d) "
    r"entries=(-?\d+)")


def drain(c, seconds: float) -> list:
    out = []
    end = time.time() + seconds
    while time.time() < end:
        try:
            ev = c._inbox_events.get(timeout=max(0.05, end - time.time()))
        except Exception:
            break
        if ev.get("name") == "run_result":
            d = ev.get("data", {})
            out.append((d.get("code"), d.get("msg", "")))
    return out


def replays(runs) -> list:
    """(seq, src, chan_ok, psum_ok, got, meta_ok, entries) per replayed capture."""
    out = []
    for _, msg in runs:
        m = REP_RE.search(msg)
        if m:
            out.append((int(m.group(1)), m.group(2), int(m.group(3)),
                        int(m.group(4)), int(m.group(5)), int(m.group(6)),
                        int(m.group(7))))
    return out


def main() -> int:
    if not backend_built():
        print("SKIP: backend not built"); return 0
    fails: list[str] = []

    # The ground truth is the shipped files, decoded here independently of the
    # backend: seq -> pixel checksum, in replay (lexicographic) order.
    disk = []
    for f in sorted(RECORDED.glob("*.xex1")):
        fr = decode_xex1(f.read_bytes())
        img = (fr.get("images") or {}).get("frame") or {}
        disk.append((fr["seq"], sum(bytes(img.get("pixels") or b""))))
    print(f"recorded/: {len(disk)} captures, seqs={[s for s, _ in disk]}")
    if len(disk) < 3:
        print(f"VERDICT: FAIL: recorded/ has only {len(disk)} .xex1 fixtures")
        return 1

    proc = spawn_backend(PORT, ROOT / f"backend_{PORT}.log", tag="xi_replay_ex")
    try:
        c = connect(PORT)
        assert c, "no connect"
        c.call("open_project", {"path": str(ROOT)})
        c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=300)
        # fps>0: the synthetic timer tick is what pumps the pull source.
        c.call("start", {"fps": 10})

        runs = drain(c, 4.0)
        got = replays(runs)
        print(f"phase1: run_results={len(runs)} replayed={len(got)}")
        for r in got[:2]:
            print(f"   {r}")

        # ---- positive half: every file, once, intact ----------------------
        if len(got) != len(disk):
            fails.append(f"{len(got)} captures replayed, {len(disk)} on disk")
        for i, (seq, src, chan_ok, psum_ok, gsum, meta_ok, entries) in enumerate(got):
            if i >= len(disk):
                break
            want_seq, want_sum = disk[i]
            if seq != want_seq:
                fails.append(f"replay #{i} out of order: seq={seq} want {want_seq}")
                break
            if gsum != want_sum:
                fails.append(f"seq {seq}: replayed pixels sum to {gsum}, the file's "
                             f"sum to {want_sum} — the frame is not the frame")
                break
            if src != "replay":
                fails.append(f"seq {seq}: primary_source={src!r}, want 'replay'")
                break
            if (chan_ok, psum_ok, meta_ok, entries) != (1, 1, 1, 6):
                fails.append(f"seq {seq}: restore incomplete: chan={chan_ok} "
                             f"psum={psum_ok} meta={meta_ok} entries={entries} "
                             "(want 1/1/1/6)")
                break
        seqs = [r[0] for r in got]
        if len(set(seqs)) != len(seqs):
            fails.append(f"a capture was replayed twice: {seqs}")

        # ---- negative half: past the end, loop:false means STOP ------------
        more = replays(drain(c, 3.0))
        status = c.exchange_instance("replay", {"command": "get_status"})
        if isinstance(status, str):
            status = json.loads(status)
        c.call("stop")
        print(f"phase2: extra replays after the last file={len(more)} "
              f"cursor={status.get('position')}/{status.get('total')}")

        if more:
            fails.append(f"{len(more)} extra captures after the last file — "
                         "loop:false must stop, not wrap")
        if not (status.get("position") == status.get("total") == len(disk)):
            fails.append(f"cursor wrong: position={status.get('position')} "
                         f"total={status.get('total')} files={len(disk)}")

        # No ng verdicts anywhere (the pump ticks are result(0) by design).
        ngs = [m for code, m in runs + [] if code is not None and code < 0]
        if ngs:
            fails.append(f"ng verdicts arrived: {ngs[:2]}")

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
