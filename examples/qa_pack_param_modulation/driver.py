"""
qa_pack_param_modulation — per-frame PARAMETER MODULATION through the pack
plane: parameters that change at frame rate ride IN the pack (data cadence);
defs/commit_group are configuration cadence. See this example's README.md.

The script sweeps `threshold` across frames as a pack entry into
blob_analysis's xi.pack@1 door over a stepped image (four squares at
intensities 60/110/160/210), producing a predictable blob-count STAIRCASE:

    thr  40 -> 4 blobs      190 -> 1 blob
    thr  90 -> 3 blobs      240 -> 0 blobs
    thr 140 -> 2 blobs

Each frame ALSO calls the door with NO threshold entry (leg B): the door falls
through to the def layer set once by instances/det/instance.json
{ "threshold": 200 } -> always 1 blob, threshold_used == 200 (NOT the plugin's
compiled-in 128, which would count 2 — proving the def landed). Leg B runs
after leg A in the same tick, so it doubles as the NO-LEAK proof: a swept
per-pack threshold never contaminates the instance or a later frame.

Asserts:
  1. >= 10 ok verdicts, each with a parseable "pmod ..." message; NO ng.
  2. >= 10 XEX1-v3 frames on channel "qa"; every frame's blob count matches
     the staircase for ITS OWN thr, and threshold_used echoes that thr.
  3. Full staircase coverage: all 5 sweep steps observed on the wire.
  4. Baseline every frame: base_blobs == 1 and base_thr_used == 200 — the def
     layer is constant while leg A modulates, and no swept value leaked.
  5. Cross-frame no-leak: for every consecutive wire pair with DIFFERENT
     thresholds, each frame's count is the staircase of its own thr (a leak
     would surface the neighbor's count).
  6. Wire seq strictly increasing.

Run:  python examples/qa_pack_param_modulation/driver.py   (Windows; backend built)
"""
from __future__ import annotations
import os
import tempfile, queue, re, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
sys.path.insert(0, str(ROOT.parents[0] / "lib"))
from xinsp2 import Client  # noqa: E402
from ports import free_port, backend_exe  # noqa: E402
from xex1 import collect_frames, subscribe  # noqa: E402

BACKEND = backend_exe()
PORT = int(os.environ.get("PORT", "0")) or free_port()
STAIRCASE = {40: 4, 90: 3, 140: 2, 190: 1, 240: 0}
DEF_THR, DEF_COUNT = 200, 1
MSG_RE = re.compile(
    r"pmod seq=(-?\d+) thr=(-?\d+) blobs=(-?\d+) want=(-?\d+) thru=(-?\d+) "
    r"base=(-?\d+) base_thru=(-?\d+) faults=(\d)(\d) pushed=(\d)")


def spawn(port):
    iso = Path(tempfile.gettempdir()) / "xi_pack_param_mod_iso"
    iso.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ); env["TEMP"] = env["TMP"] = env["TMPDIR"] = str(iso)
    log = open(ROOT / f"backend_{port}.log", "w", encoding="utf-8")
    return subprocess.Popen([str(BACKEND), f"--port={port}"], stdout=log,
                            stderr=subprocess.STDOUT, cwd=str(REPO), env=env)


def connect(port):
    for _ in range(80):
        try:
            c = Client(url=f"ws://127.0.0.1:{port}/", timeout=60); c.connect(); c.ping(); return c
        except Exception:
            time.sleep(0.5)
    return None


def drain_verdicts(c) -> list[dict]:
    out = []
    while True:
        try:
            ev = c._inbox_events.get_nowait()
        except queue.Empty:
            break
        except Exception:
            break
        if ev.get("name") == "run_result":
            out.append(ev.get("data", {}) or {})
    return out


def main() -> int:
    if not BACKEND.exists():
        print(f"SKIP: backend not built ({BACKEND})"); return 0

    fails: list[str] = []
    proc = spawn(PORT)
    try:
        c = connect(PORT)
        assert c, "no connect"
        c.call("open_project", {"path": str(ROOT)})
        c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=300)
        subscribe(c, ["qa"])

        c.call("start", {"fps": 0})       # enable the pipeline; the source drives it
        c.drain_binary()                  # zero the binary baseline
        drain_verdicts(c)                 # zero the event baseline

        c.exchange_instance("cam", {"command": "start"})

        oks: list[tuple] = []
        ngs: list[str] = []
        frames: list[dict] = []
        end = time.time() + 8.0
        while time.time() < end:
            for d in drain_verdicts(c):
                code = d.get("code", 0)
                msg = d.get("msg", "")
                m = MSG_RE.search(msg)
                if code > 0 and m:
                    oks.append(tuple(int(m.group(i)) for i in range(1, 11)))
                elif code < 0 or (code > 0 and not m):
                    ngs.append(f"code={code} msg={msg!r}")
            for fr in collect_frames(c):
                if fr.get("channel") == "qa":
                    frames.append(fr)
            if len(oks) >= 15 and len(frames) >= 15:
                break
            time.sleep(0.1)

        c.exchange_instance("cam", {"command": "stop"})
        c.call("stop")

        # ---- the staircase evidence, reconstructed from the wire ------------
        observed: dict[int, set[int]] = {}
        for fr in frames:
            v = fr.get("values") or {}
            observed.setdefault(v.get("thr"), set()).add(v.get("blobs"))
        stair = ", ".join(f"thr={t}->blobs={sorted(cs)}"
                          for t, cs in sorted(observed.items(), key=lambda kv: (kv[0] is None, kv[0])))
        print(f"ok_verdicts={len(oks)} ng={len(ngs)} frames_qa={len(frames)}")
        print(f"staircase: {stair}")

        # 1. enough ok verdicts, no ng.
        if len(oks) < 10:
            fails.append(f"too few ok verdicts (got {len(oks)}, want >=10)")
        if ngs:
            fails.append(f"ng verdicts arrived: {ngs[:3]}")

        # 2. every wire frame matches the staircase for ITS OWN thr.
        if len(frames) < 10:
            fails.append(f"too few frames on channel qa (got {len(frames)}, want >=10)")
        for fr in frames:
            if fr.get("v") != 3:
                fails.append(f"frame is not XEX1-v3: v={fr.get('v')}")
                break
            v = fr.get("values") or {}
            thr, blobs, thru = v.get("thr"), v.get("blobs"), v.get("thr_used")
            if thr not in STAIRCASE:
                fails.append(f"unexpected swept thr on the wire: {thr}")
                break
            if blobs != STAIRCASE[thr]:
                fails.append(f"staircase broke: thr={thr} gave blobs={blobs}, "
                             f"want {STAIRCASE[thr]} (seq={v.get('seq')})")
                break
            if thru != thr:
                fails.append(f"door did not echo the per-pack thr: thr={thr} "
                             f"threshold_used={thru}")
                break

        # 3. full staircase coverage: all 5 sweep steps seen.
        seen = {v for v in observed if v in STAIRCASE}
        if len(frames) >= 10 and seen != set(STAIRCASE):
            fails.append(f"staircase not fully covered: saw thr={sorted(seen)}, "
                         f"want {sorted(STAIRCASE)}")

        # 4. def-layer baseline constant on EVERY frame (and no leak into it).
        for fr in frames:
            v = fr.get("values") or {}
            if v.get("base_thr_used") != DEF_THR:
                fails.append(f"def layer wrong/leaked: base_thr_used="
                             f"{v.get('base_thr_used')}, want {DEF_THR} "
                             f"(frame thr={v.get('thr')}, seq={v.get('seq')})")
                break
            if v.get("base_blobs") != DEF_COUNT:
                fails.append(f"def-baseline count wrong: base_blobs="
                             f"{v.get('base_blobs')}, want {DEF_COUNT}")
                break

        # 5. cross-frame no-leak: consecutive frames with different thr each
        #    match their OWN staircase step (frame N never inherits N-1's).
        pairs_checked = 0
        for a, b in zip(frames, frames[1:]):
            va, vb = a.get("values") or {}, b.get("values") or {}
            if va.get("thr") == vb.get("thr"):
                continue
            pairs_checked += 1
            if vb.get("blobs") != STAIRCASE.get(vb.get("thr")):
                fails.append(f"cross-frame leak: seq={vb.get('seq')} thr={vb.get('thr')} "
                             f"gave blobs={vb.get('blobs')} after neighbor thr={va.get('thr')}")
                break
        if len(frames) >= 10 and pairs_checked < 5:
            fails.append(f"too few differing-thr consecutive pairs ({pairs_checked}) "
                         f"to prove no-leak")

        # 6. wire seq strictly increasing.
        wseqs = [fr.get("seq") for fr in frames]
        for i in range(1, len(wseqs)):
            if wseqs[i] <= wseqs[i - 1]:
                fails.append(f"wire seq not strictly increasing at {i}: "
                             f"{wseqs[i-1]} -> {wseqs[i]}")
                break

    except Exception as e:
        fails.append(f"exception: {e!r}")
    finally:
        try:
            proc.terminate(); proc.wait(timeout=10)
        except Exception:
            proc.kill()

    if fails:
        for f in fails:
            print("  -", f)
        print("VERDICT: FAIL: per-frame parameter modulation through the pack plane")
        return 1
    print("VERDICT: PASS: per-frame params ride IN the pack (staircase matches "
          "each frame's OWN thr; def layer constant at 200; no cross-frame leak)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
