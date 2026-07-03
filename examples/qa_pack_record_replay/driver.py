"""
qa_pack_record_replay — the graph-level RECORD -> REPLAY composing example
(docs/new_gen/12-pack-parity-matrix.md rows E1/E2/E3; the example owed by the
doc 10 Gate P2 verdict). The full persistence loop as a real project, pack-only:

  phase 1 (RECORD): mock_camera (PACK MODE) triggers the script; per trigger it
    rebuilds the frame as a routed capture pack ($channel/$seq + psum checksum
    + nested mp "meta" + the image), routes it into record_save's xi.pack@1
    door (use("rec").process — one canonical XEX1-v3 file per trigger in the
    rec instance's captures folder) AND pushes the same sealed pack on wire
    channel "rec".
  phase 2 (REPLAY): the driver flips the record_replay instance's `enabled`
    config; the script's synthetic-tick pump (use("replay").process, one file
    per tick) re-emits every file as a fresh sealed-pack trigger. The script
    verifies each replayed pack (restored $channel/$seq, checksum, nested mp,
    entry count) and re-surfaces it on wire channel "rep".

Asserts:
  1. >= 5 "rec ..." ok verdicts (saved=1 fault=0 pushed=1); NO ng verdicts.
  2. The .xex1 files on disk decode as XEX1-v3 with channel "rec"; per file the
     decoded image's pixel sum equals its psum entry and values.seq == header
     seq; file count == rec verdict count == "rec" wire frame count.
  3. Every file is replayed EXACTLY once: "rep" wire frames match the disk
     files 1:1 by seq — values (seq, psum), decoded nested "meta", image dims
     AND pixel bytes identical (disk == recorded wire == replayed wire).
  4. >= file-count "rep ..." ok verdicts (chan=1 seqok=1 psum=1 meta=1
     entries=6 img=1 pushed=1) — verdicts ride the run_result plane.
  5. The replay cursor ends at position == total == file count.
  6. Wire seq strictly increasing per channel.

Run:  python examples/qa_pack_record_replay/driver.py   (Windows; backend built)
"""
from __future__ import annotations
import json, os, queue, re, shutil, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
sys.path.insert(0, str(ROOT.parents[0] / "lib"))
from xinsp2 import Client  # noqa: E402
from ports import free_port  # noqa: E402
from xex1 import collect_frames, decode_xex1, subscribe  # noqa: E402

BACKEND = REPO / "backend" / "build" / "Release" / "xinsp-backend.exe"
PORT = int(os.environ.get("PORT", "0")) or free_port()
CAPTURES = ROOT / "instances" / "rec" / "captures"

REC_RE = re.compile(
    r"rec seq=(-?\d+) psum=(-?\d+) saved=(\d) fault=(\d) bytes=(-?\d+) pushed=(\d)")
REP_RE = re.compile(
    r"rep seq=(-?\d+) chan=(\d) seqok=(\d) psum=(\d) meta=(\d) entries=(-?\d+) "
    r"img=(\d) pushed=(\d)")
# "ran, set no result" (class no_verdict, backend/src/runner_main.cpp) — the
# EXPECTED outcome of the synthetic pump ticks, where the script deliberately
# sets no verdict. Every ACTIVE tick must still verdict: the count equalities
# below (rec verdicts == files == "rec" wire frames; rep verdicts == files)
# catch any swallowed real inspection.
XI_SYS_NO_VERDICT = -999005


def spawn(port):
    iso = Path(os.environ["LOCALAPPDATA"]) / "Temp" / "xi_pack_record_replay_iso"
    iso.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ); env["TEMP"] = env["TMP"] = str(iso)
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


def frame_essence(fr: dict) -> dict | str:
    """The comparable payload of a decoded XEX1-v3 frame: scalar values + the
    nested meta + the frame image (dims + raw pixel bytes)."""
    vals = fr.get("values") or {}
    img = (fr.get("images") or {}).get("frame") or {}
    return {
        "seq": vals.get("seq"),
        "psum": vals.get("psum"),
        "meta": vals.get("meta"),
        "w": img.get("w"), "h": img.get("h"), "c": img.get("c"),
        "pixels": bytes(img.get("pixels") or b""),
    }


def main() -> int:
    if os.name != "nt":
        print("SKIP: Windows-only"); return 0
    if not BACKEND.exists():
        print(f"SKIP: backend not built ({BACKEND})"); return 0

    shutil.rmtree(CAPTURES, ignore_errors=True)   # a stale capture set would
    fails: list[str] = []                         # poison the replay phase
    proc = spawn(PORT)
    try:
        c = connect(PORT)
        assert c, "no connect"
        c.call("open_project", {"path": str(ROOT)})
        c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=300)
        subscribe(c, ["rec", "rep"])

        # fps>0: the synthetic timer tick clocks the script's replay pump.
        c.call("start", {"fps": 20})
        c.drain_binary()
        drain_verdicts(c)

        recs: list[tuple] = []; reps: list[tuple] = []; ngs: list[str] = []
        frames: dict[str, list[dict]] = {"rec": [], "rep": []}

        def collect():
            for d in drain_verdicts(c):
                code = d.get("code", 0); msg = d.get("msg", "")
                if code == XI_SYS_NO_VERDICT:
                    continue                      # pump tick: no verdict by design
                m = REC_RE.search(msg); p = REP_RE.search(msg)
                if code > 0 and m:
                    recs.append(tuple(int(m.group(i)) for i in range(1, 7)))
                elif code > 0 and p:
                    reps.append(tuple(int(p.group(i)) for i in range(1, 9)))
                elif code != 0:
                    ngs.append(f"code={code} msg={msg!r}")
            for fr in collect_frames(c):
                if fr.get("channel") in frames:
                    frames[fr["channel"]].append(fr)

        # ---- phase 1: RECORD ------------------------------------------------
        c.exchange_instance("cam", {"command": "start"})
        end = time.time() + 12.0
        while time.time() < end:
            collect()
            if len(recs) >= 6 and len(frames["rec"]) >= 6:
                break
            time.sleep(0.1)
        c.exchange_instance("cam", {"command": "stop"})

        # settle: drain until file count / verdicts / wire frames stop growing.
        stable_since = time.time(); snap = (0, 0, 0)
        while time.time() - stable_since < 1.0:
            collect()
            files_now = sorted(CAPTURES.glob("*.xex1")) if CAPTURES.exists() else []
            cur = (len(files_now), len(recs), len(frames["rec"]))
            if cur != snap:
                snap = cur; stable_since = time.time()
            time.sleep(0.1)
        files = sorted(CAPTURES.glob("*.xex1"))
        nfiles = len(files)
        print(f"phase1: rec_verdicts={len(recs)} rec_frames={len(frames['rec'])} "
              f"files={nfiles} ng={len(ngs)}")

        # ---- phase 2: REPLAY (rewind BEFORE enabling — no double-replay) ----
        c.exchange_instance("replay", {"command": "rewind"})
        c.exchange_instance("replay", {"command": "set_enabled", "value": True})
        end = time.time() + 15.0
        while time.time() < end:
            collect()
            if len(reps) >= nfiles and len(frames["rep"]) >= nfiles:
                break
            time.sleep(0.1)
        time.sleep(0.5); collect()                # one last drain past the end
        status = c.exchange_instance("replay", {"command": "get_status"})
        if isinstance(status, str):
            status = json.loads(status)
        c.call("stop")
        print(f"phase2: rep_verdicts={len(reps)} rep_frames={len(frames['rep'])} "
              f"ng={len(ngs)} cursor={status.get('position')}/{status.get('total')} "
              f"first_rec={recs[0] if recs else None} "
              f"first_rep={reps[0] if reps else None}")

        # 1. record-phase verdicts: enough, structurally right, no ng at all.
        if len(recs) < 5:
            fails.append(f"too few rec ok verdicts (got {len(recs)}, want >=5)")
        for seq, psum, saved, fault, nbytes, pushed in recs:
            if saved != 1 or fault != 0 or nbytes <= 0 or pushed != 1:
                fails.append(f"record leg broke: seq={seq} saved={saved} "
                             f"fault={fault} bytes={nbytes} pushed={pushed}")
                break
        if ngs:
            fails.append(f"ng verdicts arrived: {ngs[:3]}")

        # 2. the disk files ARE canonical XEX1-v3 dumps of the captures.
        disk: dict[int, dict] = {}
        if nfiles < 5:
            fails.append(f"too few .xex1 files on disk (got {nfiles}, want >=5)")
        for f in files:
            try:
                fr = decode_xex1(f.read_bytes())
            except Exception as e:
                fails.append(f"{f.name}: does not decode as XEX1: {e!r}"); break
            if fr.get("v") != 3 or fr.get("channel") != "rec":
                fails.append(f"{f.name}: header wrong: v={fr.get('v')} "
                             f"channel={fr.get('channel')!r}")
                break
            e = frame_essence(fr)
            if e["seq"] != fr.get("seq"):
                fails.append(f"{f.name}: values.seq {e['seq']} != header seq {fr.get('seq')}")
                break
            if e["meta"] != {"origin": "cam", "trigger_seq": e["seq"]}:
                fails.append(f"{f.name}: nested meta wrong: {e['meta']!r}"); break
            if (e["w"], e["h"], e["c"]) != (32, 24, 3) or \
                    len(e["pixels"]) != 32 * 24 * 3:
                fails.append(f"{f.name}: image shape wrong: "
                             f"{e['w']}x{e['h']}x{e['c']} len={len(e['pixels'])}")
                break
            if sum(e["pixels"]) != e["psum"]:
                fails.append(f"{f.name}: pixel sum {sum(e['pixels'])} != psum {e['psum']}")
                break
            disk[fr["seq"]] = e
        if len(disk) not in (0, nfiles):
            fails.append(f"duplicate seq across disk files: {nfiles} files, "
                         f"{len(disk)} distinct seqs")
        if not (len(recs) == nfiles == len(frames["rec"])):
            fails.append(f"count mismatch: rec_verdicts={len(recs)} files={nfiles} "
                         f"rec_wire_frames={len(frames['rec'])}")

        # 3. disk == recorded wire == replayed wire, entry- and byte-level.
        for ch in ("rec", "rep"):
            seen: set[int] = set()
            for fr in frames[ch]:
                if fr.get("v") != 3:
                    fails.append(f"[{ch}] frame is not XEX1-v3: v={fr.get('v')}"); break
                e = frame_essence(fr)
                if e["seq"] in seen:
                    fails.append(f"[{ch}] seq {e['seq']} surfaced twice"); break
                seen.add(e["seq"])
                d = disk.get(e["seq"])
                if d is None:
                    fails.append(f"[{ch}] wire seq {e['seq']} has no disk file"); break
                if e != d:
                    diff = [k for k in d if e.get(k) != d.get(k)]
                    fails.append(f"[{ch}] seq {e['seq']} differs from disk in {diff}")
                    break
        if len(frames["rep"]) != nfiles:
            fails.append(f"not every file replayed exactly once: files={nfiles} "
                         f"rep_frames={len(frames['rep'])}")

        # 4. replay-phase verdicts: the script's own checks all held.
        if len(reps) < nfiles:
            fails.append(f"too few rep ok verdicts (got {len(reps)}, want {nfiles})")
        for seq, chan, seqok, psum_ok, meta_ok, entries, img, pushed in reps:
            if (chan, seqok, psum_ok, meta_ok, entries, img, pushed) != (1, 1, 1, 1, 6, 1, 1):
                fails.append(f"replay leg broke: seq={seq} chan={chan} seqok={seqok} "
                             f"psum={psum_ok} meta={meta_ok} entries={entries} "
                             f"img={img} pushed={pushed}")
                break

        # 5. the replay cursor consumed exactly the capture set.
        if not (status.get("position") == status.get("total") == nfiles):
            fails.append(f"replay cursor wrong: position={status.get('position')} "
                         f"total={status.get('total')} files={nfiles}")

        # 6. wire seq strictly increasing per channel.
        for ch, frs in frames.items():
            wseqs = [fr.get("seq") for fr in frs]
            for i in range(1, len(wseqs)):
                if wseqs[i] <= wseqs[i - 1]:
                    fails.append(f"[{ch}] wire seq not strictly increasing at {i}: "
                                 f"{wseqs[i-1]} -> {wseqs[i]}")
                    break

    except Exception as e:
        fails.append(f"exception: {e!r}")
    finally:
        try:
            proc.terminate(); proc.wait(timeout=10)
        except Exception:
            proc.kill()
        shutil.rmtree(CAPTURES, ignore_errors=True)

    if fails:
        for f in fails:
            print("  -", f)
        print("VERDICT: FAIL: graph-level record -> replay loop (E1/E2/E3 composition)")
        return 1
    print("VERDICT: PASS: record -> save(.xex1) -> replay -> verify as ONE live "
          "graph; disk == recorded wire == replayed wire, pixel-byte identical")
    return 0


if __name__ == "__main__":
    sys.exit(main())
