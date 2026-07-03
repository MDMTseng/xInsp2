"""
qa_kv_reload — xi::kv() cross-frame state survives a hot reload AND a schema
migration, live-service (U2, docs/new_gen/16-script-state-shape.md).

The kv port of hot_reload_run2's counter pattern, QA-gated. Continuous mode
(cmd:start fps=20) drives a script that counts frames in xi::kv() and surfaces
the counter pack-only (ScriptPackBuilder -> xi::use("expose").push, channel
"kvqa"; no xi::Record anywhere in the scripts):

  phase A — inspect_v1.cpp: XI_KV_SCHEMA(1), counts in kv["count"].
  reload  — inspect_v2.cpp: XI_KV_SCHEMA(2) + xi::set_kv_migrate renaming
            count -> frames and stamping migrated_from. compile_and_load swaps
            it in WITHOUT re-issuing cmd:start (auto-resume).
  phase B — v2 frames continue from the carried counter.

Asserts:
  1. >= 10 v1 frames, count strictly increasing (the kv store accumulates).
  2. >= 10 v2 frames after the swap; the FIRST v2 "frames" value continues
     from the last v1 "count" (the store crossed the host boundary bytes:
     get_kv -> migrate_kv -> set_kv).
  3. every v2 frame carries migrated_from == 1 (the migrator ran; the host
     did NOT drop on the 1 -> 2 schema mismatch).
  4. the live WS wire carried event:state_migrated with data.store == "kv"
     and old/new schema 1 -> 2 (and no state_dropped for the kv store).

Run:  python examples/qa_kv_reload/driver.py    (Windows; backend built)
"""
from __future__ import annotations
import os, queue, shutil, subprocess, sys, time
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
INSPECT = ROOT / "inspect.cpp"
V1 = ROOT / "inspect_v1.cpp"
V2 = ROOT / "inspect_v2.cpp"
CHANNEL = "kvqa"


def spawn(port):
    iso = Path(os.environ["LOCALAPPDATA"]) / "Temp" / "xi_kv_reload_iso"
    iso.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ); env["TEMP"] = env["TMP"] = str(iso)
    log = open(ROOT / f"backend_{port}.log", "w", encoding="utf-8")
    return subprocess.Popen([str(BACKEND), f"--port={port}"], stdout=log,
                            stderr=subprocess.STDOUT, cwd=str(REPO), env=env)


def connect(port):
    for _ in range(80):
        try:
            c = Client(url=f"ws://127.0.0.1:{port}/", timeout=60)
            c.connect(); c.ping(); return c
        except Exception:
            time.sleep(0.5)
    return None


def drain_events(c, sink: list[dict]) -> None:
    while True:
        try:
            sink.append(c._inbox_events.get_nowait())
        except queue.Empty:
            break
        except Exception:
            break


def collect(c, seconds: float, frames: list[dict], events: list[dict]) -> None:
    end = time.time() + seconds
    while time.time() < end:
        got = False
        for fr in collect_frames(c):
            if fr.get("channel") == CHANNEL:
                frames.append(fr); got = True
        drain_events(c, events)
        if not got:
            time.sleep(0.05)


def main() -> int:
    if os.name != "nt":
        print("SKIP: Windows-only"); return 0
    if not BACKEND.exists():
        print(f"SKIP: backend not built ({BACKEND})"); return 0

    shutil.copy2(V1, INSPECT)
    fails: list[str] = []
    events: list[dict] = []
    v1_frames: list[dict] = []
    v2_frames: list[dict] = []

    proc = spawn(PORT)
    try:
        c = connect(PORT)
        assert c, "no connect"
        c.call("open_project", {"path": str(ROOT)})
        c.compile_and_load(str(INSPECT), timeout=300)
        subscribe(c, [CHANNEL])
        c.drain_binary()
        drain_events(c, events); events.clear()

        c.call("start", {"fps": 20})

        # phase A: v1 accumulates in kv["count"]
        collect(c, 2.0, v1_frames, events)

        # hot-swap to v2 (schema 2 + migrator); do NOT re-issue cmd:start
        shutil.copy2(V2, INSPECT)
        rsp = c.compile_and_load(str(INSPECT), timeout=300)
        resumed = bool(rsp.get("resumed_continuous"))

        # phase B: v2 continues from the migrated store
        collect(c, 2.0, v2_frames, events)

        c.call("stop")

        v1_counts = [f.get("values", {}).get("count") for f in v1_frames
                     if f.get("values", {}).get("version") == 1]
        v2_vals = [f.get("values", {}) for f in v2_frames
                   if f.get("values", {}).get("version") == 2]
        v2_counts = [v.get("frames") for v in v2_vals]
        print(f"v1_frames={len(v1_counts)} v2_frames={len(v2_counts)} "
              f"last_v1={v1_counts[-1] if v1_counts else None} "
              f"first_v2={v2_counts[0] if v2_counts else None} resumed={resumed}")

        # 1. v1 accumulated
        if len(v1_counts) < 10:
            fails.append(f"too few v1 frames (got {len(v1_counts)}, want >=10)")
        if any(b <= a for a, b in zip(v1_counts, v1_counts[1:])):
            fails.append(f"v1 count not strictly increasing: {v1_counts[:20]}")

        # 2. the counter CARRIED across the reload (host get_kv -> migrate -> set_kv)
        if len(v2_counts) < 10:
            fails.append(f"too few v2 frames (got {len(v2_counts)}, want >=10)")
        if not resumed:
            fails.append("continuous run did not auto-resume across the reload")
        if v1_counts and v2_counts:
            if v2_counts[0] <= v1_counts[-1] - 1:   # allow one in-flight frame skew
                fails.append(f"kv store did NOT carry: last v1 count={v1_counts[-1]}, "
                             f"first v2 frames={v2_counts[0]} (a drop restarts at 1)")
            if v2_counts[0] <= 3:
                fails.append(f"first v2 frames={v2_counts[0]} looks like a fresh store "
                             f"(migration dropped?)")
        if any(b <= a for a, b in zip(v2_counts, v2_counts[1:])):
            fails.append(f"v2 frames not strictly increasing: {v2_counts[:20]}")

        # 3. the migrator ran (rename carried provenance into every v2 frame)
        if v2_vals and any(v.get("migrated_from") != 1 for v in v2_vals):
            fails.append(f"migrated_from != 1 in v2 frames: "
                         f"{[v.get('migrated_from') for v in v2_vals[:10]]}")

        # 4. the kv migration was announced on the live wire
        kv_migrated = [e for e in events if e.get("name") == "state_migrated"
                       and (e.get("data") or {}).get("store") == "kv"]
        kv_dropped = [e for e in events if e.get("name") == "state_dropped"
                      and (e.get("data") or {}).get("store") == "kv"]
        if not kv_migrated:
            fails.append("no event:state_migrated with store=kv on the wire")
        else:
            d = kv_migrated[0].get("data") or {}
            if (d.get("old_schema"), d.get("new_schema")) != (1, 2):
                fails.append(f"kv state_migrated schemas wrong: {d}")
        if kv_dropped:
            fails.append(f"kv store was dropped, not migrated: {kv_dropped[0]}")

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
        print("VERDICT: FAIL: xi::kv() hot-reload carry / schema migration")
        return 1
    print("VERDICT: PASS: xi::kv() carried across a live hot reload and a "
          "schema 1->2 migration (count -> frames), pack-only output, "
          "state_migrated(store=kv) on the wire")
    return 0


if __name__ == "__main__":
    sys.exit(main())
