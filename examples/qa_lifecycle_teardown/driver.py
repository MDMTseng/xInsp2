"""qa_lifecycle_teardown — shutdown/lifecycle UAF regression (core bug-hunt 2026-06-30).

Three deterministic checks that a teardown/lifecycle op exits/continues CLEANLY and
writes NO spurious minidump to %TEMP%/xinsp2/crashdumps:

  #2  bind failure (port already exclusively held) must `return 1` cleanly. Before the
      fix the always-on watchdog std::thread was still joinable at static destruction →
      std::terminate → the crash filter fabricated a minidump (the FE reads a routine
      port-in-use as a backend CRASH). Fixed by a WatchdogJoiner RAII that stops+joins
      the watchdog on every early return.

  #7  cmd:shutdown with a project (plugin instances) open must exit 0. Before the fix
      no normal exit path called close_project, so ~PluginManager destroyed instances
      AFTER FreeLibrary (destroy_fn into unmapped code) and against an already-destroyed
      ImagePool singleton → access violation on a graceful shutdown. Fixed by
      controlled_shutdown_teardown_ calling close_project (instances die while the pool
      is alive + DLLs mapped), a ~PluginManager backstop that clears instances before
      FreeLibrary, and a g_image_pool_alive guard on the adapter's image sweep.

  #6  close_project while a free-running source (frame_source) keeps emitting on its own
      thread must not UAF into the unloaded plugin DLL. Before the fix the bus sink
      stayed installed across the FreeLibrary and a one-shot inspect could launch on the
      source's emit thread into freed code. Fixed by quiesce_dispatch_for_lifecycle_op_
      now pausing detached launches + clearing the sink + draining before the unload
      (reversed by the guard for ops that resume).

Run:  python driver.py

Uses sibling example projects as fixtures: ../qa_sink_shared_doc (2 instances) for #7,
../burst_pipeline (a free-running source) for #6.

TODO(linux): backend compile (cl.exe) is Windows-only; SKIPs on non-nt.
"""
from __future__ import annotations
import os, socket, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO_ROOT = ROOT.parents[1]
sys.path.insert(0, str(REPO_ROOT / "tools" / "xinsp2_py"))
sys.path.insert(0, str(REPO_ROOT / "examples" / "lib"))
from xinsp2 import Client  # noqa: E402
from ports import free_port  # noqa: E402

SUF = ".exe" if os.name == "nt" else ""
BE = REPO_ROOT / "backend" / "build" / "Release" / f"xinsp-backend{SUF}"
DUMPS = Path(os.environ.get("TEMP", "/tmp")) / "xinsp2" / "crashdumps"
PROJ_INSTANCES = REPO_ROOT / "examples" / "qa_sink_shared_doc"
PROJ_SOURCE    = REPO_ROOT / "examples" / "burst_pipeline"


def dump_count():
    return len(list(DUMPS.glob("*.dmp"))) if DUMPS.exists() else 0


def port_open(p, timeout=0.3):
    try:
        with socket.create_connection(("127.0.0.1", p), timeout):
            return True
    except OSError:
        return False


def wait_port(p, proc, secs=20):
    deadline = time.time() + secs
    while time.time() < deadline and not port_open(p):
        if proc.poll() is not None:
            return False
        time.sleep(0.15)
    return port_open(p)


def test_bind_fail():
    d0 = dump_count()
    # SO_EXCLUSIVEADDRUSE: the WS server sets SO_REUSEADDR, so on Windows a plain second
    # backend would hijack-bind the same port and just run. Exclusive-hold forces the
    # bind to genuinely fail so we exercise the early `return 1` path.
    # NB: let the EXCLUSIVE holder pick the ephemeral port itself (bind :0) — routing
    # through ports.free_port() would taint the port with SO_REUSEADDR first, and a
    # subsequent SO_EXCLUSIVEADDRUSE bind on that port fails with WSAEADDRINUSE.
    holder = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    if os.name == "nt":
        holder.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
    holder.bind(("127.0.0.1", 0)); holder.listen(1)
    port = holder.getsockname()[1]
    try:
        b = subprocess.Popen([str(BE), f"--port={port}"], cwd=str(BE.parent),
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                             stdin=subprocess.DEVNULL)
        try:
            rc = b.wait(timeout=15)
        except subprocess.TimeoutExpired:
            b.kill()
            return ["bind_fail: backend did not exit cleanly (hung) on a held port"]
        time.sleep(0.5)
        fails = []
        if rc != 1:
            fails.append(f"bind_fail: exit code {rc} (expected 1)")
        if dump_count() - d0 != 0:
            fails.append("bind_fail: a minidump was written on a routine port-in-use")
        return fails
    finally:
        holder.close()


def test_clean_shutdown_with_project():
    port = free_port()
    d0 = dump_count()
    blog = open(ROOT / "be.log", "wb")
    be = subprocess.Popen([str(BE), f"--port={port}"], cwd=str(BE.parent),
                          stdout=blog, stderr=blog, stdin=subprocess.DEVNULL)
    try:
        if not wait_port(port, be):
            return ["clean_shutdown: backend never bound"]
        with Client(url=f"ws://127.0.0.1:{port}/", timeout=60) as c:
            c.open_project(str(PROJ_INSTANCES), timeout=300)
            c.compile_and_load(str(PROJ_INSTANCES / "inspect.cpp"), timeout=180)
            c.call("start", {"fps": 60})
            time.sleep(1.0)
            try: c.call("shutdown")
            except Exception: pass
        try:
            rc = be.wait(timeout=10)
        except subprocess.TimeoutExpired:
            be.kill()
            return ["clean_shutdown: backend hung after cmd:shutdown"]
        time.sleep(0.5)
        fails = []
        if rc != 0:
            fails.append(f"clean_shutdown: exit code {rc} (expected 0)")
        if dump_count() - d0 != 0:
            fails.append("clean_shutdown: a minidump was written on a graceful shutdown")
        return fails
    finally:
        if be.poll() is None: be.kill()
        blog.close()


def test_open_close_cycle():
    port = free_port()
    cycles = 8
    d0 = dump_count()
    blog = open(ROOT / "be_cycle.log", "wb")
    be = subprocess.Popen([str(BE), f"--port={port}"], cwd=str(BE.parent),
                          stdout=blog, stderr=blog, stdin=subprocess.DEVNULL)
    try:
        if not wait_port(port, be):
            return ["open_close_cycle: backend never bound"]
        with Client(url=f"ws://127.0.0.1:{port}/", timeout=60) as c:
            for i in range(cycles):
                c.open_project(str(PROJ_SOURCE), timeout=300)
                c.compile_and_load(str(PROJ_SOURCE / "inspect.cpp"), timeout=180)
                c.call("start", {"fps": 120})
                time.sleep(0.4)
                c.call("close_project")          # the #6 race: close mid-stream
                if be.poll() is not None:
                    return [f"open_close_cycle: backend CRASHED on cycle {i} (rc={be.poll()})"]
            c.call("dispatch_stats")             # still serving + healthy
        fails = []
        if be.poll() is not None:
            fails.append(f"open_close_cycle: backend exited (rc={be.poll()})")
        if dump_count() - d0 != 0:
            fails.append(f"open_close_cycle: a minidump was written across {cycles} cycles")
        return fails
    finally:
        try:
            import websocket
            ws = websocket.create_connection(f"ws://127.0.0.1:{port}/", timeout=2)
            ws.send('{"type":"cmd","id":999,"name":"shutdown"}'); ws.close()
        except Exception:
            pass
        try: be.wait(timeout=8)
        except subprocess.TimeoutExpired: be.kill()
        blog.close()


def main() -> int:
    if os.name != "nt":
        print("SKIP: backend compile is Windows-only here")
        return 0
    if not BE.exists():
        sys.exit(f"FAIL: missing {BE} (build xinsp_backend)")
    all_fails = []
    for label, fn in [("#2 bind-fail clean exit", test_bind_fail),
                      ("#7 clean shutdown with project", test_clean_shutdown_with_project),
                      ("#6 open/close cycle under source load", test_open_close_cycle)]:
        print(f"== {label} ==")
        fails = fn()
        print("  ", "PASS" if not fails else "FAIL")
        for f in fails: print("   -", f)
        all_fails += fails
    print("=" * 48)
    print("VERDICT:", "PASS" if not all_fails else "FAIL")
    return 1 if all_fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
