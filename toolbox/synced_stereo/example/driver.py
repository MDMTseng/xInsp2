"""synced_stereo example — proof that left+right are ONE record, not two streams.

Fires the source N times, deterministically, one tick at a time. Three things
have to hold together, and each one is useless without the others:

  ONE RECORD PER TRIGGER   exactly N runs and N exposed frames — not 2N. A pair
                           of independent streams would produce two of each.
  BOTH IMAGES IN IT        every exposed frame carries `left` AND `right`.
  ACTUALLY CORRELATED      the script recovers the tick's seq from each image's
                           stamp *and* from the stripe phase painted into the
                           pixels, and both sides agree.

...plus the negative half: the two images must be DIFFERENT (left vertically
striped, right horizontally). "Perfectly correlated" is trivially true if you
are handed the same buffer twice, so a correlation check alone would pass for
the most broken possible source.

Run:  python toolbox/synced_stereo/example/driver.py
"""
from __future__ import annotations
import os, re, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[2]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
sys.path.insert(0, str(REPO / "qa" / "lib"))
from ports import free_port          # noqa: E402
from backends import backend_built, spawn_backend, connect  # noqa: E402
from xex1 import collect_frames, subscribe  # noqa: E402

PORT = int(os.environ.get("PORT", "0")) or free_port()
FIRES = 12

MSG_RE = re.compile(
    r"stereo seq=(-?\d+) stamps=(-?\d+)/(-?\d+) phase=(-?\d+)/(-?\d+) want=(-?\d+) "
    r"dims=(\d) vert=(\d) horz=(\d) differ=(\d)")


def main() -> int:
    if not backend_built():
        print("SKIP: backend not built"); return 0
    fails: list[str] = []
    verdicts: list[dict] = []
    frames: list[dict] = []
    proc = spawn_backend(PORT, ROOT / f"backend_{PORT}.log", tag="xi_stereo_ex")

    def pump(c):
        for ev in _drain_events(c):
            if ev.get("name") == "run_result":
                verdicts.append(ev.get("data", {}) or {})
        for fr in collect_frames(c):
            if fr.get("channel") == "stereo":
                frames.append(fr)

    try:
        c = connect(PORT)
        assert c, "no connect"
        c.call("open_project", {"path": str(ROOT)})
        c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=300)
        subscribe(c, ["stereo"])              # gate the channel on so images push

        c.call("start", {"fps": 0})           # trigger-only: `fire` drives it
        c.drain_binary(); _drain_events(c)    # zero both baselines

        # One tick at a time — a gathered pair per fire, nothing overlapping.
        for _ in range(FIRES):
            c.exchange_instance("stereo", {"command": "fire", "n": 1})
            time.sleep(0.15)
            pump(c)
        end = time.time() + 5.0
        while time.time() < end and (len(verdicts) < FIRES or len(frames) < FIRES):
            pump(c); time.sleep(0.1)

        c.call("stop")
        pump(c)

        oks = [MSG_RE.search(v.get("msg", "")) for v in verdicts if v.get("code", 0) > 0]
        oks = [tuple(int(g) for g in m.groups()) for m in oks if m]
        ngs = [v.get("msg") for v in verdicts if v.get("code", 0) < 0]
        print(f"fires={FIRES} verdicts={len(verdicts)} ok={len(oks)} ng={len(ngs)} "
              f"exposed_frames={len(frames)}")
        if oks:
            print(f"  first: seq={oks[0][0]} stamps={oks[0][1]}/{oks[0][2]} "
                  f"phase={oks[0][3]}/{oks[0][4]} want={oks[0][5]}")

        # ---- ONE record per trigger ----------------------------------------
        if len(verdicts) != FIRES:
            fails.append(f"{FIRES} fires produced {len(verdicts)} runs — a "
                         "gathered pair must be exactly ONE trigger")
        if len(frames) != FIRES:
            fails.append(f"{FIRES} fires produced {len(frames)} exposed stereo "
                         "records — left+right must ride one record, not two")
        if ngs:
            fails.append(f"ng verdicts: {ngs[:2]}")
        if len(oks) != FIRES:
            fails.append(f"only {len(oks)} parseable ok verdicts of {FIRES}")

        # ---- BOTH images, in the SAME record ---------------------------------
        for i, fr in enumerate(frames):
            keys = sorted((fr.get("images") or {}).keys())
            if keys != ["left", "right"]:
                fails.append(f"exposed record {i} carries images {keys} — "
                             "want both 'left' and 'right' in the one record")
                break

        # ---- ACTUALLY CORRELATED, and actually two different views ----------
        seqs = []
        for (seq, sl, sr, pl, pr, want, dims, vert, horz, differ) in oks:
            seqs.append(seq)
            if not (sl == sr == seq):
                fails.append(f"seq={seq}: image stamps {sl}/{sr} disagree with "
                             "the pack seq — the pair is not from one tick")
                break
            if not (pl == pr == want):
                fails.append(f"seq={seq}: stripe phase {pl}/{pr} != {want} — the "
                             "pixels were not painted for this tick")
                break
            if not dims:
                fails.append(f"seq={seq}: dims mismatch between left and right")
                break
            if not (vert and horz and differ):
                fails.append(f"seq={seq}: left/right are not two DISTINCT views "
                             f"(vert={vert} horz={horz} differ={differ}) — "
                             "correlation is meaningless if it is one buffer twice")
                break
        # Distinct ticks, in order: 12 copies of one event would satisfy
        # everything above.
        if seqs and seqs != sorted(set(seqs)):
            fails.append(f"seqs are not strictly increasing: {seqs}")
        if len(set(seqs)) != len(oks):
            fails.append(f"duplicate seqs — the same tick was seen twice: {seqs}")

        c.call("close_project"); c.close()
    except Exception as e:
        fails.append(f"{e}")
    finally:
        proc.terminate()
        try: proc.wait(5)
        except Exception: proc.kill()

    print("VERDICT:", "PASS" if not fails else "FAIL: " + "; ".join(fails))
    return 0 if not fails else 1


def _drain_events(c) -> list:
    out = []
    while True:
        try:
            out.append(c._inbox_events.get_nowait())
        except Exception:
            return out


if __name__ == "__main__":
    sys.exit(main())
