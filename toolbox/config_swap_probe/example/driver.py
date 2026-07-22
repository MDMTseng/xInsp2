"""config_swap_probe example — proof that a config swap is ATOMIC per frame.

Live traffic runs the whole time. Mid-run the driver stages a new config and
then commits it, and asserts the frame stream partitions into exactly three
phases, in order, with nothing in between:

    (active=42, staged=0)   before prepare
    (active=42, staged=1)   staged, loaded, and STILL NOT LIVE   <-- the point
    (active=99, staged=0)   committed

Both halves are load-bearing. The positive: after commit, frames see 99. The
negative, and the one that actually distinguishes this design from "just call
set_def": phase B must be NON-EMPTY, and every frame in it must still observe
42. A staged config that leaked into live traffic — or a swap that landed
mid-frame — shows up as `last_seen != active` on some frame, which is checked
on every single one.

Run:  python toolbox/config_swap_probe/example/driver.py
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

PORT = int(os.environ.get("PORT", "0")) or free_port()
OLD, NEW = 42, 99
MSG_RE = re.compile(
    r"swap seq=(-?\d+) door=(\d) active=(-?\d+) staged=(-?\d+) sval=(-?\d+) "
    r"last=(-?\d+) proc=(-?\d+)")


def main() -> int:
    if not backend_built():
        print("SKIP: backend not built"); return 0
    fails: list[str] = []
    oks: list[tuple] = []          # (seq, door, active, staged, sval, last, proc)
    ngs: list[str] = []
    proc_h = spawn_backend(PORT, ROOT / f"backend_{PORT}.log", tag="xi_cswap_ex")

    def pump(c) -> None:
        while True:
            try:
                ev = c._inbox_events.get_nowait()
            except Exception:
                return
            if ev.get("name") != "run_result":
                continue
            d = ev.get("data", {}) or {}
            m = MSG_RE.search(d.get("msg", ""))
            if d.get("code", 0) > 0 and m:
                oks.append(tuple(int(m.group(i)) for i in range(1, 8)))
            else:
                ngs.append(f"code={d.get('code')} msg={d.get('msg')!r}")

    def until(c, cond, timeout_s: float) -> bool:
        end = time.time() + timeout_s
        while time.time() < end:
            pump(c)
            if cond():
                return True
            time.sleep(0.05)
        pump(c)
        return bool(cond())

    try:
        c = connect(PORT)
        assert c, "no connect"
        c.call("open_project", {"path": str(ROOT)})   # seeds probe via set_def(42)
        c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=300)

        c.call("start", {"fps": 0})           # trigger-only: the camera drives
        pump(c); oks.clear(); ngs.clear()     # zero the baseline
        c.exchange_instance("cam", {"command": "start"})

        # ---- phase A: live traffic on the OLD config ------------------------
        if not until(c, lambda: len(oks) >= 6, 12.0):
            fails.append(f"phase A: only {len(oks)} frames before prepare")

        # ---- STAGE: build the new config in the background ------------------
        # Traffic must NOT pause and must NOT switch over. This call returns as
        # soon as the resource is built into the staging slot.
        n_at_prepare = len(oks)
        c.call("prepare_instance", {"name": "probe", "def": {"value": NEW, "_schema": 1}})
        if not until(c, lambda: sum(1 for v in oks[n_at_prepare:] if v[3] == 1) >= 4, 12.0):
            fails.append("phase B: staged=1 never became visible under live "
                         "traffic — nothing was staged, or traffic stopped")

        # ---- COMMIT: drain barrier, atomic swap, resume ---------------------
        res = c.commit_group(instances=["probe"])
        for r in (res or {}).get("results", []):
            if r.get("name") == "probe" and not r.get("ok", False):
                fails.append(f"commit_group reported probe not ok: {r!r}")

        # ---- phase C: live traffic on the NEW config ------------------------
        if not until(c, lambda: sum(1 for v in oks if v[2] == NEW) >= 6, 12.0):
            fails.append("phase C: too few frames on the new config after commit")

        c.exchange_instance("cam", {"command": "stop"})
        until(c, lambda: False, 0.8)          # final drain
        c.call("stop")
        final = c.exchange_instance("probe", {"command": "get_status"})

        by_seq = sorted(oks, key=lambda v: v[0])
        phases = [(v[2], v[3]) for v in by_seq]
        PH_A, PH_B, PH_C = (OLD, 0), (OLD, 1), (NEW, 0)
        counts = {p: phases.count(p) for p in (PH_A, PH_B, PH_C)}
        print(f"frames={len(oks)} ng={len(ngs)} final={final}")
        print(f"  phase A (42,none)={counts[PH_A]}  "
              f"B (42,staged)={counts[PH_B]}  C (99,none)={counts[PH_C]}")

        if ngs:
            fails.append(f"ng verdicts arrived: {ngs[:2]}")
        if not oks:
            fails.append("no frames at all")

        # -- ATOMICITY, on every single frame ---------------------------------
        for (seq, door, active, staged, sval, last, _p) in by_seq:
            if door != 1:
                fails.append(f"seq={seq}: the pack door drive failed"); break
            if last != active:
                fails.append(f"seq={seq}: TORN — the door saw {last} while the "
                             f"live slot holds {active}; the swap landed mid-frame")
                break

        # -- exactly three legal states, in order, none of them empty ---------
        illegal = [p for p in phases if p not in (PH_A, PH_B, PH_C)]
        if illegal:
            fails.append(f"illegal (active,staged) states observed: {illegal[:4]}")
        for name, p in (("A", PH_A), ("B", PH_B), ("C", PH_C)):
            if counts[p] < 1:
                fails.append(f"phase {name} {p} is empty — the run did not "
                             "actually exercise the staged/committed transition")
        expected = [PH_A] * counts[PH_A] + [PH_B] * counts[PH_B] + [PH_C] * counts[PH_C]
        if phases != expected:
            fails.append("phases interleave — the stream is not "
                         f"42/none -> 42/staged -> 99/none in seq order: {phases[:14]}")

        # -- the NEGATIVE half: staging alone changes nothing a frame sees ----
        for v in by_seq:
            if (v[2], v[3]) == PH_B:
                if v[2] != OLD or v[5] != OLD:
                    fails.append(f"seq={v[0]}: a STAGED config leaked into live "
                                 f"traffic (active={v[2]} last_seen={v[5]})")
                    break
                if v[4] != NEW:
                    fails.append(f"seq={v[0]}: staged_value={v[4]} != {NEW}")
                    break

        # -- the door ran exactly once per frame -------------------------------
        procs = [v[6] for v in by_seq]
        if procs != list(range(1, len(by_seq) + 1)):
            fails.append(f"probe proc counter is not 1..N in seq order "
                         f"(head {procs[:8]}) — a door call was lost or doubled")
        if isinstance(final, dict) and (final.get("active") != NEW or
                                        final.get("staged") is not False):
            fails.append(f"final status not settled on the new config: {final}")

        c.call("close_project"); c.close()
    except Exception as e:
        fails.append(f"{e}")
    finally:
        proc_h.terminate()
        try: proc_h.wait(5)
        except Exception: proc_h.kill()

    print("VERDICT:", "PASS" if not fails else "FAIL: " + "; ".join(fails))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
