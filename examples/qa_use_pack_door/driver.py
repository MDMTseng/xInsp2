"""
qa_use_pack_door — the Gate P2 FLAGSHIP: the full pack write-half loop in one
script, pack-only in the live service (docs/new_gen/12-pack-parity-matrix.md
rows B1/B2 use-door chaining, C1 expose-from-script, D1 script-built payloads,
U4 nested canonical-mp).

mock_camera (PACK MODE) drives the pipeline. Per trigger the script (1) builds
a white-square gray pack with xi::ScriptPackBuilder (image + scalars + a nested
xi::mp::Writer entry, round-tripped byte-identical), (2) chains it into
blob_analysis's xi.pack@1 door via xi::use("det").process(pack) and asserts on
the door's result entries, (3) builds a RESULT pack carrying the derived binary
image + the nested mp entry + $channel/$seq and xi::use("expose").push()es it,
plus a SECOND script-built pack (the synthetic gray) on channel "qa2"
(multi-channel). expose runs the XEX1-v3 canonical wire, so this driver
byte-checks the pushed pixels and decodes the nested entry.

Asserts:
  1. >= 5 ok verdicts, each with a parseable "pdoor ..." message showing
     built=1 mp=1 blobs=1 thr=128 bin=1 fault=0 pushed=1 pushed2=1; NO ng.
  2. >= 5 XEX1-v3 frames on channel "qa": values blob_count==1, seq matches the
     frame's top-level seq (the $seq routing entry), the nested "meta" entry
     decodes to {origin:"script", trigger_seq:seq}, and the pushed binary
     image is 16x16x1 with EXACTLY the 25 thresholded white pixels (byte check
     of the script-built, door-derived, script-pushed pixels).
  3. >= 5 frames on channel "qa2" carrying the script-built 16x16x1 gray with
     the 5x5 white square, byte-identical (multi-channel + synthetic preview).
  4. Wire seq strictly increasing per channel (sink-staged flush order).

Run:  python examples/qa_use_pack_door/driver.py   (Windows; backend built)
"""
from __future__ import annotations
import os, queue, re, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
sys.path.insert(0, str(ROOT.parents[0] / "lib"))
from xinsp2 import Client  # noqa: E402
from xex1 import collect_frames, subscribe  # noqa: E402

BACKEND = REPO / "backend" / "build" / "Release" / "xinsp-backend.exe"
PORT = int(os.environ.get("PORT", "7913"))
MSG_RE = re.compile(
    r"pdoor seq=(-?\d+) built=(\d) mp=(\d) blobs=(-?\d+) thr=(-?\d+) "
    r"bin=(\d) fault=(\d) pushed=(\d) pushed2=(\d)")


def spawn(port):
    iso = Path(os.environ["LOCALAPPDATA"]) / "Temp" / "xi_use_pack_door_iso"
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


def check_binary_image(img: dict) -> str | None:
    """The pushed door output: 16x16x1, exactly the 5x5 white square at (5,5)."""
    if not img:
        return "no 'binary' image on the frame"
    if (img.get("w"), img.get("h"), img.get("c")) != (16, 16, 1):
        return f"binary dims wrong: {img.get('w')}x{img.get('h')}x{img.get('c')}"
    px = img.get("pixels") or b""
    if len(px) != 16 * 16:
        return f"binary pixel length wrong: {len(px)}"
    for y in range(16):
        for x in range(16):
            want = 255 if (5 <= x < 10 and 5 <= y < 10) else 0
            if px[y * 16 + x] != want:
                return f"binary pixel ({x},{y}) = {px[y*16+x]}, want {want}"
    return None


def main() -> int:
    if os.name != "nt":
        print("SKIP: Windows-only"); return 0
    if not BACKEND.exists():
        print(f"SKIP: backend not built ({BACKEND})"); return 0

    fails: list[str] = []
    proc = spawn(PORT)
    try:
        c = connect(PORT)
        assert c, "no connect"
        c.call("open_project", {"path": str(ROOT)})
        c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=300)
        subscribe(c, ["qa", "qa2"])       # gate BOTH expose channels ON

        c.call("start", {"fps": 0})       # enable the pipeline; the source drives it
        c.drain_binary()                  # zero the binary baseline
        drain_verdicts(c)                 # zero the event baseline

        c.exchange_instance("cam", {"command": "start"})

        oks: list[tuple] = []             # parsed pdoor tuples
        ngs: list[str] = []
        frames: dict[str, list[dict]] = {"qa": [], "qa2": []}
        end = time.time() + 8.0
        while time.time() < end:
            for d in drain_verdicts(c):
                code = d.get("code", 0)
                msg = d.get("msg", "")
                m = MSG_RE.search(msg)
                if code > 0 and m:
                    oks.append(tuple(int(m.group(i)) for i in range(1, 10)))
                elif code < 0 or (code > 0 and not m):
                    ngs.append(f"code={code} msg={msg!r}")
            for fr in collect_frames(c):
                if fr.get("channel") in frames:
                    frames[fr["channel"]].append(fr)
            if len(oks) >= 12 and all(len(v) >= 12 for v in frames.values()):
                break
            time.sleep(0.1)

        c.exchange_instance("cam", {"command": "stop"})
        c.call("stop")
        print(f"ok_verdicts={len(oks)} ng={len(ngs)} "
              f"frames_qa={len(frames['qa'])} frames_qa2={len(frames['qa2'])} "
              f"first_ok={oks[0] if oks else None} "
              f"first_qa_vals={frames['qa'][0].get('values') if frames['qa'] else None}")

        # 1. enough full-loop ok verdicts, each structurally right; no ng.
        if len(oks) < 5:
            fails.append(f"too few full-loop ok verdicts (got {len(oks)}, want >=5)")
        for seq, built, mp, blobs, thr, has_bin, fault, pushed, pushed2 in oks:
            if built != 1 or mp != 1:
                fails.append(f"ScriptPackBuilder leg broke: built={built} mp={mp}")
                break
            if blobs != 1 or thr != 128 or has_bin != 1 or fault != 0:
                fails.append(f"use-door leg broke: blobs={blobs} thr={thr} "
                             f"bin={has_bin} fault={fault}")
                break
            if pushed != 1 or pushed2 != 1:
                fails.append(f"expose push leg broke: pushed={pushed} pushed2={pushed2}")
                break
        if ngs:
            fails.append(f"ng verdicts arrived: {ngs[:3]}")

        # 2. the pushed result packs arrived on the v3 wire, bytes intact.
        if len(frames["qa"]) < 5:
            fails.append(f"too few pushed frames on channel qa (got {len(frames['qa'])}, want >=5)")
        for fr in frames["qa"]:
            if fr.get("v") != 3:
                fails.append(f"frame is not XEX1-v3: v={fr.get('v')}")
                break
            vals = fr.get("values") or {}
            if vals.get("blob_count") != 1:
                fails.append(f"pushed blob_count wrong: {vals.get('blob_count')}")
                break
            if vals.get("seq") != fr.get("seq"):
                fails.append(f"$seq routing entry mismatch: values.seq={vals.get('seq')} "
                             f"frame.seq={fr.get('seq')}")
                break
            meta = vals.get("meta")
            if meta != {"origin": "script", "trigger_seq": fr.get("seq")}:
                fails.append(f"nested mp entry wrong on the wire: {meta!r}")
                break
            err = check_binary_image((fr.get("images") or {}).get("binary"))
            if err:
                fails.append(f"pushed binary image wrong: {err}")
                break

        # 3. the second channel: the script-built synthetic gray, byte-identical.
        if len(frames["qa2"]) < 5:
            fails.append(f"too few frames on channel qa2 (got {len(frames['qa2'])}, want >=5)")
        for fr in frames["qa2"]:
            err = check_binary_image((fr.get("images") or {}).get("gray"))
            if err:
                fails.append(f"qa2 synthetic image wrong: {err}")
                break

        # 4. wire seq strictly increasing per channel (sink-staged flush order).
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

    if fails:
        for f in fails:
            print("  -", f)
        print("VERDICT: FAIL: pack write-half loop (build -> door -> push) e2e")
        return 1
    print("VERDICT: PASS: ScriptPackBuilder -> use().process(door) -> "
          "use(expose).push() full loop pack-only in the live service "
          "(pixels byte-checked on the v3 wire)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
