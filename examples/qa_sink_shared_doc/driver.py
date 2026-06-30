"""qa_sink_shared_doc — ordered-sink $seq shared-doc double-stamp regression.

A source-less script stages ONE Record to TWO sink instances every synthetic tick.
Both `xi::use(sink).process(rec)` calls share_out the SAME underlying doc, so the
host's staged flush sees two staged items pointing at one doc. The host stamps the
reserved key "$seq" (the frame arrival id) onto each delivery. If the flush stamps it
IN PLACE (add-not-put, no copy-on-write), the SECOND sink receives a doc carrying TWO
"$seq" keys — a malformed packet and a corruption of a doc the first sink (and any
other holder) may still read.

Each sink counts the "$seq" keys it sees and reports the max. PASS = both sinks max==1.
Before the fix (flush_staged_emits_ COW-when-shared + put-semantics), sinkB reported 2.

Run:  python driver.py

TODO(linux): backend compile (cl.exe) is Windows-only; SKIPs on non-nt.
"""
from __future__ import annotations
import os, socket, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO_ROOT = ROOT.parents[1]
sys.path.insert(0, str(REPO_ROOT / "tools" / "xinsp2_py"))
from xinsp2 import Client  # noqa: E402

SUF = ".exe" if os.name == "nt" else ""
BE = REPO_ROOT / "backend" / "build" / "Release" / f"xinsp-backend{SUF}"
PORT = 7884
COLLECT_S = 2.5


def port_open(p, timeout=0.3):
    try:
        with socket.create_connection(("127.0.0.1", p), timeout):
            return True
    except OSError:
        return False


def main() -> int:
    if os.name != "nt":
        print("SKIP: backend compile is Windows-only here")
        return 0
    if not BE.exists():
        sys.exit(f"FAIL: missing {BE} (build xinsp_backend)")

    blog = open(ROOT / "be.log", "wb")
    be = subprocess.Popen([str(BE), f"--port={PORT}"], cwd=str(BE.parent),
                          stdout=blog, stderr=blog, stdin=subprocess.DEVNULL)
    a = b = None
    try:
        deadline = time.time() + 20
        while time.time() < deadline and not port_open(PORT):
            if be.poll() is not None:
                raise SystemExit(f"FAIL: backend exited during boot rc={be.poll()}")
            time.sleep(0.2)
        with Client(url=f"ws://127.0.0.1:{PORT}/", timeout=60) as c:
            c.open_project(str(ROOT), timeout=300)
            c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=180)
            c.call("start", {"fps": 120})
            time.sleep(COLLECT_S)
            try:
                c.call("stop")
            except Exception:
                pass
            a = c.exchange_instance("sinkA", {"command": "stats"})
            b = c.exchange_instance("sinkB", {"command": "stats"})
    finally:
        try:
            import websocket
            ws = websocket.create_connection(f"ws://127.0.0.1:{PORT}/", timeout=2)
            ws.send('{"type":"cmd","id":999,"name":"shutdown"}'); ws.close()
        except Exception:
            pass
        try:
            be.wait(timeout=6)
        except subprocess.TimeoutExpired:
            be.kill()
        blog.close()

    print(f"[sinkA] {a}")
    print(f"[sinkB] {b}")
    fails = []
    if not a or a.get("calls", 0) < 5:
        fails.append(f"sinkA got too few calls: {a}")
    if not b or b.get("calls", 0) < 5:
        fails.append(f"sinkB got too few calls: {b}")
    if a and a.get("max_seq_keys") != 1:
        fails.append(f"sinkA max_seq_keys={a.get('max_seq_keys')} (expected 1)")
    if b and b.get("max_seq_keys") != 1:
        fails.append(f"sinkB max_seq_keys={b.get('max_seq_keys')} (expected 1 — >1 = double-stamp bug)")

    print("=" * 48)
    if fails:
        print("VERDICT: FAIL")
        for f in fails:
            print("  -", f)
        return 1
    print("VERDICT: PASS — both sinks saw exactly one $seq per delivery")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
