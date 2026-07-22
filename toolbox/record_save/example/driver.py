"""record_save example — proof that the receipt matches what is on disk.

Two phases, because a recorder is only trustworthy if BOTH halves hold:

  phase 1 (enabled)  every capture's ack (`saved=1 count=N base=cap_00000N.xex1`)
                     corresponds to a real file that decodes as XEX1-v3 and
                     whose pixels still add up to the psum the script sealed in.
  phase 2 (disabled) the acks say `saved=0 reason=disabled` and NOT ONE new file
                     appears. Without this half, phase 1 would pass just as well
                     against a sink that ignores `enabled` and always writes.

Run:  python toolbox/record_save/example/driver.py
"""
from __future__ import annotations
import os, re, shutil, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[2]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
sys.path.insert(0, str(REPO / "qa" / "lib"))
from ports import free_port          # noqa: E402
from backends import backend_built, spawn_backend, connect  # noqa: E402
from xex1 import decode_xex1         # noqa: E402

PORT = int(os.environ.get("PORT", "0")) or free_port()
CAPTURES = ROOT / "captures"         # must match instances/rec/instance.json

SAVED_RE = re.compile(
    r"save seq=(-?\d+) saved=1 count=(\d+) base=(\S+) bytes=(\d+) psum=(-?\d+)")
OFF_RE = re.compile(r"save seq=(-?\d+) saved=0 reason=(\S+)")


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


def files() -> list[Path]:
    return sorted(CAPTURES.glob("*.xex1")) if CAPTURES.exists() else []


def main() -> int:
    if not backend_built():
        print("SKIP: backend not built"); return 0
    fails: list[str] = []
    shutil.rmtree(CAPTURES, ignore_errors=True)   # a stale set would fake phase 1
    proc = spawn_backend(PORT, ROOT / f"backend_{PORT}.log", tag="xi_recsave_ex")
    try:
        c = connect(PORT)
        assert c, "no connect"
        c.call("open_project", {"path": str(ROOT)})
        c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=300)
        c.call("start", {"fps": 0})               # trigger-only: the camera drives

        # ---- phase 1: recording ------------------------------------------
        c.call("exchange_instance", {"name": "cam", "cmd": {"command": "start"}})
        runs = drain(c, 4.0)
        c.call("exchange_instance", {"name": "cam", "cmd": {"command": "stop"}})
        runs += drain(c, 1.0)                      # let in-flight frames land
        on_disk = files()
        saves = [SAVED_RE.search(m) for _, m in runs]
        saves = [m for m in saves if m]
        print(f"phase1: run_results={len(runs)} saved_acks={len(saves)} "
              f"files={len(on_disk)}")
        if runs:
            print(f"   {runs[0][1]}")

        if len(saves) < 5:
            fails.append(f"only {len(saves)} saved acks — the sink barely ran")
        if len(on_disk) != len(saves):
            fails.append(f"receipt/disk mismatch: {len(saves)} acks claimed a save "
                         f"but {len(on_disk)} .xex1 files exist")
        for i, m in enumerate(saves, start=1):
            seq, count, base, nbytes, psum = (m.group(1), int(m.group(2)),
                                              m.group(3), int(m.group(4)),
                                              int(m.group(5)))
            if count != i:
                fails.append(f"capture counter jumped: ack #{i} says count={count}")
                break
            path = CAPTURES / base
            if not path.exists():
                fails.append(f"ack named {base} but no such file"); break
            if path.stat().st_size != nbytes:
                fails.append(f"{base}: ack said {nbytes} bytes, file is "
                             f"{path.stat().st_size}")
                break
            try:
                fr = decode_xex1(path.read_bytes())
            except Exception as e:
                fails.append(f"{base}: does not decode as XEX1: {e!r}"); break
            if fr.get("v") != 3 or fr.get("channel") != "cap":
                fails.append(f"{base}: header wrong: v={fr.get('v')} "
                             f"channel={fr.get('channel')!r}")
                break
            vals = fr.get("values") or {}
            img = (fr.get("images") or {}).get("frame") or {}
            if vals.get("meta") != {"origin": "cam", "trigger_seq": vals.get("seq")}:
                fails.append(f"{base}: nested meta did not survive: {vals.get('meta')!r}")
                break
            if str(vals.get("seq")) != seq or fr.get("seq") != vals.get("seq"):
                fails.append(f"{base}: seq drift: ack={seq} entry={vals.get('seq')} "
                             f"header={fr.get('seq')}")
                break
            pix_sum = sum(bytes(img.get("pixels") or b""))
            if pix_sum != psum or vals.get("psum") != psum:
                fails.append(f"{base}: pixels add up to {pix_sum}, ack psum={psum}, "
                             f"stored psum={vals.get('psum')}")
                break

        # ---- phase 2: the gate -------------------------------------------
        before = len(on_disk)
        c.call("exchange_instance",
               {"name": "rec", "cmd": {"command": "set_enabled", "value": False}})
        c.call("exchange_instance", {"name": "cam", "cmd": {"command": "start"}})
        off_runs = drain(c, 3.0)
        c.call("exchange_instance", {"name": "cam", "cmd": {"command": "stop"}})
        off_runs += drain(c, 1.0)
        after = len(files())
        offs = [OFF_RE.search(m) for _, m in off_runs]
        offs = [m for m in offs if m]
        print(f"phase2 (disabled): run_results={len(off_runs)} "
              f"disabled_acks={len(offs)} files {before} -> {after}")

        if len(offs) < 3:
            fails.append(f"only {len(offs)} disabled acks — the gate was never "
                         "exercised")
        elif any(m.group(2) != "disabled" for m in offs):
            fails.append(f"unexpected reason: {[m.group(2) for m in offs][:3]}")
        if any(code not in (0, None) for code, _ in off_runs):
            fails.append(f"a disabled capture was not reported as NA: "
                         f"{sorted({c for c, _ in off_runs})}")
        if after != before:
            fails.append(f"the sink wrote {after - before} file(s) while disabled")

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
