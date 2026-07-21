"""
qa_multi_graph — ONE project, ONE script, TWO independent inspection pipelines.

Line A: camA (64x48 @30fps, dispatch group "line_a") -> detA -> expose ch "a".
Line B: camB (96x72 @10fps, dispatch group "line_b") -> detB -> expose ch "b".
The script routes by t.primary_source(); the dispatcher already routed each
trigger to its own GroupLane (own queue + worker) via the emitting instance's
instance.json "group". camA seqs [60,120) are DELIBERATELY poisoned in-script
(U1 fault path: ScriptPackBuilder::fault -> detA short-circuits -> NG).

Asserts:
  1. dispatch_stats reports both lanes (line_a / line_b).
  2. Routing purity: every "line_a" verdict is an "A ..." message and every
     "line_b" verdict a "B ..." message — no cross-line contamination.
  3. Line A correct + fault-windowed: OK verdicts (blob=1, w=64, h=48,
     prov=detA) exactly for seqs OUTSIDE [60,120); NG verdicts inside the
     window, each with sc=1 reason=frame_timeout prov=detA (the door
     short-circuited; it never ran on poison).
  4. Line B correct everywhere: ALL verdicts OK with blob=2, w=96, h=72,
     prov=detB; zero NG; seqs strictly increasing; count at its own ~10fps
     cadence (and line A's ~30fps count clearly outruns it).
  5. COUNT-BASED ISOLATION: within the wall-clock window spanned by line A's
     NG burst, line B's verdict rate is >= 60% of its pre-fault rate and
     >= 50% of its nominal 10fps — the injected line-A outage does not
     perturb line B.
  6. WIRE: per-channel XEX1 $seq strictly monotone independently on "a" and
     "b"; channel "a" keeps flowing THROUGH the fault window (fault=1 frames
     interleaved, seq monotone across the boundary).

Run:  python examples/qa_multi_graph/driver.py   (Windows; backend built)
"""
from __future__ import annotations
import os
import tempfile, queue, re, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
sys.path.insert(0, str(REPO / "examples" / "lib"))
from xinsp2 import Client  # noqa: E402
from ports import free_port, backend_exe  # noqa: E402
from xex1 import collect_frames, subscribe  # noqa: E402

BACKEND = backend_exe()
PORT = int(os.environ.get("PORT", "0")) or free_port()

FAULT_FROM, FAULT_TO = 60, 120          # camA seqs poisoned by the script
OK_RE = re.compile(r"^([AB]) seq=(-?\d+) blob=(-?\d+) w=(\d+) h=(\d+) c=(\d+) prov=(\S*)$")
NG_RE = re.compile(r"^A seq=(-?\d+) FAULT sc=(\d) reason=(\S+) prov=(\S+)$")


def spawn(port):
    iso = Path(tempfile.gettempdir()) / "xi_multigraph_iso"
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
    now = time.time()
    out = []
    while True:
        try:
            ev = c._inbox_events.get_nowait()
        except queue.Empty:
            break
        except Exception:
            break
        if ev.get("name") == "run_result":
            d = ev.get("data", {}) or {}
            d["_t"] = now
            out.append(d)
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
        subscribe(c, ["a", "b"])                 # live XEX1 stream for both channels

        c.call("start", {"fps": 0})              # trigger-only; the cameras drive it
        drain_verdicts(c); c.drain_binary()      # zero the baselines
        c.exchange_instance("camA", {"command": "start"})
        c.exchange_instance("camB", {"command": "start"})
        t_start = time.time()

        verdicts: list[dict] = []                # run_result events (+ arrival time)
        frames: list[dict] = []                  # decoded XEX1 frames
        end = t_start + 8.0
        while time.time() < end:
            verdicts += drain_verdicts(c)
            frames += collect_frames(c)
            time.sleep(0.05)

        c.exchange_instance("camA", {"command": "stop"})
        c.exchange_instance("camB", {"command": "stop"})
        time.sleep(0.5)
        verdicts += drain_verdicts(c)
        frames += collect_frames(c)

        st = c.call("dispatch_stats")
        c.call("stop"); c.call("close_project")

        # ---- 1. both lanes exist -------------------------------------------
        lanes = {g["name"] for g in (st.get("groups") or [])}
        print("lanes:", sorted(lanes))
        if lanes != {"line_a", "line_b"}:
            fails.append(f"expected lanes line_a/line_b, got {sorted(lanes)}")

        # ---- parse + bucket the verdicts ------------------------------------
        a_ok, a_ng, b_ok, b_ng, misrouted, malformed = [], [], [], [], [], []
        for d in verdicts:
            code, msg, grp = d.get("code", 0), d.get("msg", ""), d.get("group")
            if code == 0:
                continue                          # timer NA / drops: not a line verdict
            mo, mn = OK_RE.match(msg), NG_RE.match(msg)
            line = mo.group(1) if mo else ("A" if mn else "?")
            if line == "?":
                malformed.append(f"code={code} msg={msg!r}"); continue
            want_grp = "line_a" if line == "A" else "line_b"
            if grp != want_grp:
                misrouted.append(f"line {line} verdict on lane {grp!r}: {msg!r}"); continue
            rec = {"t": d.get("_t", 0.0), "code": code, "msg": msg,
                   "seq": int((mo or mn).group(2 if mo else 1))}
            if mo:
                rec.update(blob=int(mo.group(3)), w=int(mo.group(4)),
                           h=int(mo.group(5)), c=int(mo.group(6)), prov=mo.group(7))
                (a_ok if line == "A" else b_ok).append(rec) if code > 0 else \
                    (a_ng if line == "A" else b_ng).append(rec)
            else:
                rec.update(sc=int(mn.group(2)), reason=mn.group(3), prov=mn.group(4))
                (a_ng if code < 0 else a_ok).append(rec)
        print(f"lineA: ok={len(a_ok)} ng={len(a_ng)}   lineB: ok={len(b_ok)} ng={len(b_ng)}   "
              f"misrouted={len(misrouted)} malformed={len(malformed)}")

        # ---- 2. routing purity ----------------------------------------------
        if misrouted:
            fails.append(f"cross-line routing: {misrouted[:3]}")
        if malformed:
            fails.append(f"malformed verdicts: {malformed[:3]}")

        # ---- 3. line A: correct results + fault exactly in the window --------
        if len(a_ok) < 30:
            fails.append(f"line A produced too few OK verdicts ({len(a_ok)})")
        if len(a_ng) < 20:
            fails.append(f"line A fault window produced too few NG verdicts ({len(a_ng)})")
        for r in a_ok:
            if FAULT_FROM <= r["seq"] < FAULT_TO:
                fails.append(f"line A OK verdict INSIDE fault window: {r['msg']!r}"); break
            if r["code"] < 0 or r.get("blob") != 1 or r.get("w") != 64 \
                    or r.get("h") != 48 or r.get("c") != 3 or r.get("prov") != "detA":
                fails.append(f"line A wrong result: {r['msg']!r}"); break
        for r in a_ng:
            if not (FAULT_FROM <= r["seq"] < FAULT_TO):
                fails.append(f"line A NG verdict OUTSIDE fault window: {r['msg']!r}"); break
            if r.get("sc") != 1 or r.get("reason") != "frame_timeout" or r.get("prov") != "detA":
                fails.append(f"line A fault leg broke (short-circuit/provenance): {r['msg']!r}"); break

        # ---- 4. line B: correct everywhere, own cadence -----------------------
        if b_ng:
            fails.append(f"line B produced NG verdicts: {[r['msg'] for r in b_ng[:3]]}")
        if len(b_ok) < 30:
            fails.append(f"line B produced too few verdicts ({len(b_ok)})")
        for r in b_ok:
            if r.get("blob") != 2 or r.get("w") != 96 or r.get("h") != 72 \
                    or r.get("c") != 3 or r.get("prov") != "detB":
                fails.append(f"line B wrong result: {r['msg']!r}"); break
        b_seqs = [r["seq"] for r in b_ok]
        if any(y <= x for x, y in zip(b_seqs, b_seqs[1:])):
            fails.append("line B verdict seqs not strictly increasing")
        n_a, n_b = len(a_ok) + len(a_ng), len(b_ok)
        if not (n_a > n_b):
            fails.append(f"line A (30fps, {n_a}) did not outrun line B (10fps, {n_b})")

        # ---- 5. count-based isolation across the fault window -----------------
        if a_ng and b_ok:
            t0 = min(r["t"] for r in a_ng)
            t1 = max(r["t"] for r in a_ng)
            span = t1 - t0
            if span < 1.0:
                fails.append(f"fault window too short to measure ({span:.2f}s)")
            else:
                b_in = sum(1 for r in b_ok if t0 <= r["t"] <= t1)
                rate_in = b_in / span
                pre = max(t0 - t_start, 0.001)
                b_pre = sum(1 for r in b_ok if r["t"] < t0)
                rate_pre = b_pre / pre
                print(f"isolation: fault span {span:.2f}s  lineB rate in-window "
                      f"{rate_in:.1f}/s  pre-window {rate_pre:.1f}/s (nominal 10/s)")
                if rate_in < 5.0:
                    fails.append(f"line B throughput dented in fault window "
                                 f"({rate_in:.1f}/s < 5/s nominal floor)")
                if rate_pre > 0 and rate_in < 0.6 * rate_pre:
                    fails.append(f"line B rate dropped across fault window "
                                 f"({rate_pre:.1f}/s -> {rate_in:.1f}/s)")

        # ---- 6. wire: per-channel $seq monotone, independently ----------------
        cha = [f for f in frames if f.get("channel") == "a"]
        chb = [f for f in frames if f.get("channel") == "b"]
        other = [f for f in frames if f.get("channel") not in ("a", "b")]
        print(f"wire: ch a {len(cha)} frames, ch b {len(chb)} frames, other {len(other)}")
        if len(cha) < 30: fails.append(f"channel a too few wire frames ({len(cha)})")
        if len(chb) < 30: fails.append(f"channel b too few wire frames ({len(chb)})")
        for name, chf in (("a", cha), ("b", chb)):
            seqs = [f.get("seq") for f in chf]
            bad = [(x, y) for x, y in zip(seqs, seqs[1:]) if y <= x]
            if bad:
                fails.append(f"channel {name} $seq not strictly monotone: first {bad[0]}")
        # channel a flowed THROUGH the outage: fault frames present, and normal
        # frames exist on both sides of the window (monotone already checked).
        a_fault = [f for f in cha if f.get("values", {}).get("fault") == 1]
        a_seqs = [f.get("seq") for f in cha]
        if not a_fault:
            fails.append("channel a carried no fault-window frames")
        if not (a_seqs and min(a_seqs) < FAULT_FROM and max(a_seqs) >= FAULT_TO):
            fails.append(f"channel a did not span the fault window "
                         f"(seqs {min(a_seqs) if a_seqs else '-'}..{max(a_seqs) if a_seqs else '-'})")

        c.close()
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
        print("VERDICT: FAIL: multi-graph (two lanes, two pipelines) e2e")
        return 1
    print("VERDICT: PASS: two independent pipelines in one project/script — "
          "own lanes, own cadence, correct per-line results, per-channel $seq "
          "monotone, and a line-A fault window that never dented line B")
    return 0


if __name__ == "__main__":
    sys.exit(main())
