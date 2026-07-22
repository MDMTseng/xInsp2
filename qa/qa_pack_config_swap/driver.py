"""
qa_pack_config_swap — the F1 composing example: the CONFIG-SWAP pattern
(prepare/commit under live traffic) driven PACK-ONLY
(docs/new_gen/12-pack-parity-matrix.md row F1; docs/new_gen/10 gate P2's
owed-example clause).

mock_camera (PACK MODE) drives a running graph. Per trigger the script chains
the trigger pack into config_swap_probe's xi.pack@1 door
(xi::use("probe").process(t.pack())) and verdicts the probe's get_status
observation on the run_result plane. Mid-run THIS driver swaps the probe's def
along the orchestrator path the config-swap pattern teaches:

  instance.json config {value:42}  --open_project-->  set_def   (tier-1 seed)
  prepare_instance {value:99}      -- stage, live traffic keeps flowing on 42
  commit_group ["probe"]           -- drain-barrier, frame-perfect swap to 99

Asserts:
  a. CONFIG REFLECTED: the seq-ordered verdict stream partitions into exactly
     three phases (active=42/staged=0 -> 42/staged=1,staged_value=99 -> 99/0),
     each phase non-empty, no other state ever observed, and last_seen==active
     on EVERY frame (the door's pack-path observation matches the live slot of
     its own run — old XOR new config, never torn/half-committed).
  b. NO RUN LOST/DUPLICATED: every admitted frame produces exactly one door
     call and one verdict — the probe's proc counter is exactly 1..N in seq
     order, final get_status proc == number of verdicts, and seqs are strictly
     increasing with NO duplicates and NO gaps, with ONE documented exception:
     commit_group's drain-barrier clears the trigger sink for its no-process
     window (service_dispatch.cpp quiesce_dispatch_for_lifecycle_op_), so a
     free-running source can lose a couple of ticks THERE and only there. The
     driver allows at most one gap of <=3 seqs and REQUIRES it to sit exactly
     on the staged->committed phase boundary — a gap anywhere else, any
     duplicate, or any proc skew is a failure.
  c. PACK-ONLY: zero `xi::Record` (the literal string "Record") in inspect.cpp,
     which drives the probe exclusively through the pack door.

Run:  python qa/qa_pack_config_swap/driver.py   (Windows; backend built)
"""
from __future__ import annotations
import os
import tempfile, queue, re, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
sys.path.insert(0, str(REPO / "qa" / "lib"))
from xinsp2 import Client  # noqa: E402
from ports import free_port, backend_exe  # noqa: E402

BACKEND = backend_exe()
PORT = int(os.environ.get("PORT", "0")) or free_port()
MSG_RE = re.compile(
    r"cswap seq=(-?\d+) door=(\d) active=(-?\d+) staged=(-?\d+) "
    r"sval=(-?\d+) last=(-?\d+) proc=(-?\d+)")
OLD, NEW = 42, 99


def spawn(port):
    iso = Path(tempfile.gettempdir()) / "xi_pack_cswap_iso"
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
    oks: list[tuple] = []            # (seq, door, active, staged, sval, last, proc)
    ngs: list[str] = []

    def collect(c, need, timeout_s):
        """Drain verdicts until `need(oks)` is true (or timeout). False = timeout."""
        end = time.time() + timeout_s
        while time.time() < end:
            for d in drain_verdicts(c):
                code, msg = d.get("code", 0), d.get("msg", "")
                m = MSG_RE.search(msg)
                if code > 0 and m:
                    oks.append(tuple(int(m.group(i)) for i in range(1, 8)))
                else:
                    ngs.append(f"code={code} msg={msg!r}")
            if need(oks):
                return True
            time.sleep(0.05)
        return False

    proc = spawn(PORT)
    try:
        c = connect(PORT)
        assert c, "no connect"
        c.call("open_project", {"path": str(ROOT)})   # seeds probe via set_def(42)
        c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=300)

        c.call("start", {"fps": 0})       # enable the pipeline; the source drives it
        drain_verdicts(c)                 # zero the event baseline
        c.exchange_instance("cam", {"command": "start"})

        # ---- phase A: live traffic on the OLD config -------------------------
        if not collect(c, lambda o: len(o) >= 6, 10.0):
            fails.append(f"phase A: too few verdicts before prepare ({len(oks)})")

        # ---- STAGE: prepare the NEW config; traffic keeps flowing on OLD -----
        c.call("prepare_instance",
               {"name": "probe", "def": {"value": NEW, "_schema": 1}})
        n_at_prepare = len(oks)
        if not collect(c, lambda o: sum(1 for v in o[n_at_prepare:] if v[3] == 1) >= 4,
                       10.0):
            fails.append("phase B: staged=1 never became visible under live traffic")

        # ---- COMMIT: drain-barrier, frame-perfect swap ------------------------
        commit = c.commit_group(instances=["probe"])   # raises on partial
        for r in (commit or {}).get("results", []):
            if r.get("name") == "probe" and not r.get("ok", False):
                fails.append(f"commit_group reported probe not ok: {r!r}")

        # ---- phase C: live traffic on the NEW config --------------------------
        if not collect(c, lambda o: sum(1 for v in o if v[2] == NEW) >= 6, 10.0):
            fails.append("phase C: too few verdicts on the new config after commit")

        c.exchange_instance("cam", {"command": "stop"})
        time.sleep(0.5)
        collect(c, lambda o: False, 0.6)   # final drain of stragglers
        c.call("stop")

        final = c.exchange_instance("probe", {"command": "get_status"})
        print(f"ok_verdicts={len(oks)} ng={len(ngs)} final_status={final} "
              f"first_ok={oks[0] if oks else None} last_ok={oks[-1] if oks else None}")

        if ngs:
            fails.append(f"ng verdicts arrived: {ngs[:3]}")
        if not oks:
            fails.append("no verdicts at all")

        by_seq = sorted(oks, key=lambda v: v[0])

        # -- b. no run lost/duplicated across the swap ---------------------------
        # Strictly increasing, no duplicates; the ONLY tolerated discontinuity
        # is the commit barrier's documented no-process window (trigger sink
        # cleared during quiesce), and it must sit exactly on the
        # staged->committed boundary.
        seqs = [v[0] for v in by_seq]
        states = [(v[2], v[3]) for v in by_seq]
        gaps = []
        for i in range(1, len(seqs)):
            if seqs[i] <= seqs[i - 1]:
                fails.append(f"duplicate/regressed seq at {i}: "
                             f"{seqs[i-1]} -> {seqs[i]}")
                break
            if seqs[i] != seqs[i - 1] + 1:
                gaps.append(i)
        if len(gaps) > 1:
            fails.append(f"{len(gaps)} seq gaps — frames lost outside the "
                         f"commit barrier: {[(seqs[i-1], seqs[i]) for i in gaps]}")
        elif len(gaps) == 1:
            i = gaps[0]
            lost = seqs[i] - seqs[i - 1] - 1
            if lost > 3:
                fails.append(f"commit-barrier gap too wide: lost {lost} seqs "
                             f"({seqs[i-1]} -> {seqs[i]})")
            if not (states[i - 1] == (OLD, 1) and states[i] == (NEW, 0)):
                fails.append(f"seq gap {seqs[i-1]} -> {seqs[i]} is NOT at the "
                             f"swap point (states {states[i-1]} -> {states[i]}) "
                             f"— a frame was lost outside the commit barrier")
        procs = [v[6] for v in by_seq]
        if procs != list(range(1, len(by_seq) + 1)):
            fails.append(f"probe proc counter is not exactly 1..N in seq order "
                         f"(head: {procs[:8]}) — a door call was lost or doubled")
        if isinstance(final, dict):
            if final.get("proc") != len(oks):
                fails.append(f"final proc={final.get('proc')} != verdicts={len(oks)}")
            if final.get("active") != NEW or final.get("staged") is not False:
                fails.append(f"final status not settled on the new config: {final}")
        else:
            fails.append(f"final get_status not a dict: {final!r}")

        # -- a. pack results reflect the new config after the swap point --------
        # Every frame: the door observation equals the live slot of its own run.
        for v in by_seq:
            seq, door, active, staged, sval, last, _ = v
            if door != 1:
                fails.append(f"seq={seq}: pack door drive failed (door={door})")
                break
            if last != active:
                fails.append(f"seq={seq}: TORN observation last={last} != "
                             f"active={active} — swap not frame-perfect")
                break
        # The stream partitions into exactly the three legal phases, in order.
        PH_A, PH_B, PH_C = (OLD, 0), (OLD, 1), (NEW, 0)
        phases = [(v[2], v[3]) for v in by_seq]
        illegal = [p for p in phases if p not in (PH_A, PH_B, PH_C)]
        if illegal:
            fails.append(f"illegal (active,staged) states observed: {illegal[:4]}")
        counts = {p: phases.count(p) for p in (PH_A, PH_B, PH_C)}
        if min(counts.values() or [0]) < 1:
            fails.append(f"a phase is empty: {counts} (swap did not happen mid-run)")
        expected = ([PH_A] * counts[PH_A] + [PH_B] * counts[PH_B]
                    + [PH_C] * counts[PH_C])
        if phases != expected:
            fails.append("phases interleave — the stream is not "
                         f"42/none -> 42/staged -> 99 in seq order: {phases[:12]}...")
        for v in by_seq:                    # staged frames must stage exactly NEW
            if (v[2], v[3]) == PH_B and v[4] != NEW:
                fails.append(f"seq={v[0]}: staged_value={v[4]} != {NEW}")
                break

        # -- c. zero xi::Record in the script ------------------------------------
        src = (ROOT / "inspect.cpp").read_text(encoding="utf-8")
        if "Record" in src:
            fails.append("inspect.cpp mentions the legacy currency (Record)")
        if "t.pack()" not in src or 'xi::use("probe").process' not in src:
            fails.append("inspect.cpp is not the pack-door drive it claims to be")

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
        print("VERDICT: FAIL: pack-mode config swap (prepare/commit under live pack traffic)")
        return 1
    print("VERDICT: PASS: config swap composed pack-only — prepare/commit_group "
          "swapped the def mid-run, frame-perfect (last_seen==active on every "
          "frame), no run lost or duplicated across the swap (proc==1..N, seqs "
          "strict, any barrier gap pinned to the swap point), zero xi::Record "
          "in the script")
    return 0


if __name__ == "__main__":
    sys.exit(main())
