"""
qa_pack_stream — e2e for the doc-18 streaming-via-chunking convention
(docs/new_gen/18-stream-chunking-convention.md: $stream/$part/$eof sealed-pack
sequences with overlap regions; reserved keys xi::pack_contract::kStream/
kPart/kEof).

mock_camera runs in PACK MODE and drives the script per trigger. The script
plays both sides of the convention over real sealed packs (ScriptPackBuilder
producer, doc-18 consumer state machine) and publishes a structural summary on
the expose channel "stream" plus an ok/ng run_result verdict. Zero xi::Record.

Asserts (per summary frame, all frames):
  1. BOUNDARY FEATURE EXACTLY ONCE: feat_a==1 — the 4x4 feature spanning the
     row-16 chunk boundary was found once and only once, and overlap_needed==1
     proves NO zero-overlap chunking sees it whole (the overlap rows are the
     only reason it is detectable).
  2. OVERLAP DEDUP: feat_b==1, feat_other==0 — the feature fully inside the
     shared overlap band is seen whole by BOTH chunks but reported by exactly
     one (the ownership-band rule).
  3. COMPLETION + BOUNDED WINDOW: s1_complete==1 (the stream completes on
     $eof) and win==1 (the reassembly window never held more than one chunk).
  4. FAULT POISON: s2_fault_abort==1 — a deliberately injected mid-stream
     fault pack aborts the stream with the ORIGINAL reason (sensor_drop),
     partials discarded, stragglers dropped.
  5. TIMEOUT: s3_timeout==1 — a stream whose $eof never arrives aborts
     stream_timeout at the end-of-lane sweep.
  5b. FOREIGN FAULT IGNORED (F3): s4_foreign_fault_ignored==1 — a fault tagged
     for ANOTHER stream (and a stream-less fault) delivered mid-flight does NOT
     poison this consumer; the stream still completes with both features found.
  6. VERDICT: >=1 ok run_result arrived and ZERO ng run_results (every run's
     in-script assertion bundle passed).

Run:  python examples/qa_pack_stream/driver.py   (Windows; backend built)
"""
from __future__ import annotations
import os, queue, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
sys.path.insert(0, str(ROOT.parents[0] / "lib"))
from xinsp2 import Client  # noqa: E402
from ports import free_port  # noqa: E402
from xex1 import collect_frames, subscribe  # noqa: E402

BACKEND = REPO / "backend" / "build" / "Release" / "xinsp-backend.exe"
PORT = int(os.environ.get("PORT", "0")) or free_port()


def spawn(port):
    iso = Path(os.environ["LOCALAPPDATA"]) / "Temp" / "xi_pack_stream_iso"
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


def drain_verdicts(c) -> tuple[int, int]:
    """Count (ok, ng) run_result events queued on the client's event inbox."""
    ok = ng = 0
    while True:
        try:
            ev = c._inbox_events.get_nowait()
        except queue.Empty:
            break
        except Exception:
            break
        if ev.get("name") != "run_result":
            continue
        code = (ev.get("data", {}) or {}).get("code", 0)
        if code > 0:
            ok += 1
        elif code < 0:
            ng += 1
    return ok, ng


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
        subscribe(c, ["stream"])          # gate the expose channel ON so frames push

        c.call("start", {"fps": 0})       # enable the pipeline; the source drives it
        c.drain_binary()                  # zero the binary baseline
        drain_verdicts(c)                 # zero the event baseline

        c.exchange_instance("cam", {"command": "start"})

        summaries: list[dict] = []
        ok_verdicts = ng_verdicts = 0
        end = time.time() + 8.0
        while time.time() < end:
            for fr in collect_frames(c):
                if fr.get("channel") != "stream":
                    continue
                vals = fr.get("values") or {}
                if "feat_a" not in vals:
                    continue
                summaries.append(vals)
            ok, ng = drain_verdicts(c)
            ok_verdicts += ok; ng_verdicts += ng
            if len(summaries) >= 12:      # plenty — stop early once we have a run
                break
            time.sleep(0.1)

        c.exchange_instance("cam", {"command": "stop"})
        c.call("stop")
        ok, ng = drain_verdicts(c)
        ok_verdicts += ok; ng_verdicts += ng

        print(f"summaries={len(summaries)} first={summaries[0] if summaries else None} "
              f"ok_verdicts={ok_verdicts} ng_verdicts={ng_verdicts}")

        if len(summaries) < 5:
            fails.append(f"too few stream summaries reached the wire (got {len(summaries)}, want >=5)")

        want = {"feat_a": 1, "feat_b": 1, "feat_other": 0, "s1_complete": 1,
                "win": 1, "overlap_needed": 1, "s2_fault_abort": 1, "s3_timeout": 1,
                "s4_foreign_fault_ignored": 1}
        for i, s in enumerate(summaries):
            bad = {k: s.get(k) for k, v in want.items() if int(s.get(k, -1)) != v}
            if bad:
                fails.append(f"summary[{i}] structural flags off (want {want}): got {bad}")
                break

        if ok_verdicts < 1:
            fails.append("no ok run_result verdict arrived")
        if ng_verdicts > 0:
            fails.append(f"{ng_verdicts} ng run_result verdict(s) — an in-script stream assertion failed")

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
        print("VERDICT: FAIL: doc-18 stream chunking e2e")
        return 1
    print("VERDICT: PASS: $stream/$part/$eof chunk sequence reassembled - boundary feature "
          "exactly once via overlap, $eof completion, fault poison + timeout abort verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
