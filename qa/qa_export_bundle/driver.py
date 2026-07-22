"""
export_bundle + AOT — a deployment bundle runs on a target with NO toolchain.

Exports an existing project (qa_group_parallelism: a plugin + a script) via
tools/export_bundle.py, then runs the BUNDLE's backend with --aot and a STRIPPED
PATH (no MSVC / no cl.exe reachable). Asserts: the backend loads the prebuilt
plugin + script DLLs (log "AOT: loading prebuilt", no "compiling project plugin")
and actually inspects (run_results flow). This is the no-toolchain proof.

Run:  python qa/qa_export_bundle/driver.py   (Windows; backend built)
"""
from __future__ import annotations
import os, shutil, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
SRC_PROJECT = REPO / "qa" / "qa_group_parallelism"   # plugin + script, source-driven
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
sys.path.insert(0, str(REPO / "qa" / "lib"))
from xinsp2 import Client  # noqa: E402
from ports import free_port  # noqa: E402

BACKEND = REPO / "backend" / "build" / "Release" / "xinsp-backend.exe"
PORT = int(os.environ.get("PORT", "0")) or free_port()          # bundle run port
EXPORT_PORT = free_port()                                       # baked into the bundle


def main() -> int:
    if os.name != "nt":
        print("SKIP: Windows-only"); return 0
    if not BACKEND.exists():
        print(f"SKIP: backend not built ({BACKEND})"); return 0
    fails: list[str] = []
    bundle = Path(os.environ["LOCALAPPDATA"]) / "Temp" / "xi_qa_bundle"
    if bundle.exists(): shutil.rmtree(bundle, ignore_errors=True)
    try:
        # 1. Export (dev box has the toolchain). --fps=-1: source-driven, trigger-only.
        r = subprocess.run([sys.executable, str(REPO / "tools" / "export_bundle.py"),
                            str(SRC_PROJECT), str(bundle), "--fps=-1", f"--port={EXPORT_PORT}"],
                           cwd=str(REPO), capture_output=True, text=True, timeout=400)
        if r.returncode != 0:
            print(r.stdout[-1500:]); fails.append(f"export failed rc={r.returncode}")
            print("VERDICT: FAIL: " + "; ".join(fails)); return 1
        for need in ["bin/xinsp-backend.exe", "project/script.dll",
                     "project/plugins/burst_source/build", "fe.json", "run.bat"]:
            if not (bundle / need).exists(): fails.append(f"bundle missing {need}")

        # 2. Run the bundle backend with a STRIPPED PATH (no MSVC / no cl.exe) + --aot.
        sysroot = os.environ.get("SystemRoot", r"C:\Windows")
        tmp = bundle / "_tmp"; tmp.mkdir(exist_ok=True)
        env = {"SystemRoot": sysroot, "windir": sysroot,
               "PATH": f"{bundle / 'bin'};{sysroot}\\System32;{sysroot}",
               "TEMP": str(tmp), "TMP": str(tmp),
               "LOCALAPPDATA": os.environ["LOCALAPPDATA"], "USERPROFILE": os.environ.get("USERPROFILE", "")}
        if shutil.which("cl", path=env["PATH"]):
            fails.append("cl.exe IS on the stripped PATH — test can't prove no-toolchain")
        log = bundle / "aot_be.log"
        be = subprocess.Popen([str(bundle / "bin" / "xinsp-backend.exe"), "--aot",
                               f"--project={bundle / 'project'}", f"--port={PORT}", "--autostart-fps=-1"],
                              stdout=open(log, "w"), stderr=subprocess.STDOUT, env=env, cwd=str(bundle / "bin"))
        try:
            c = None
            for _ in range(80):
                try: c = Client(url=f"ws://127.0.0.1:{PORT}/", timeout=30); c.connect(); c.ping(); break
                except Exception: time.sleep(0.5)
            if not c: fails.append("bundle backend did not come up")
            else:
                n = 0; end = time.time() + 3
                while time.time() < end:
                    try: ev = c._inbox_events.get(timeout=1)
                    except Exception: break
                    if ev.get("name") == "run_result": n += 1
                print(f"AOT bundle run_results in 3s: {n}")
                if n < 5: fails.append(f"AOT bundle produced too few run_results ({n})")
                c.close()
        finally:
            be.terminate()
            try: be.wait(5)
            except Exception: be.kill()

        txt = log.read_text(errors="replace") if log.exists() else ""
        if "AOT: loading prebuilt" not in txt: fails.append("log missing 'AOT: loading prebuilt'")
        if "compiling project plugin" in txt:  fails.append("backend COMPILED (AOT not honored)")
        print(f"log: prebuilt-load={'AOT: loading prebuilt' in txt} compiled={'compiling project plugin' in txt}")
    except Exception as e:
        fails.append(f"{e}")
    finally:
        shutil.rmtree(bundle, ignore_errors=True)
        shutil.rmtree(Path(os.environ['LOCALAPPDATA']) / 'Temp' / 'xi_export_dev', ignore_errors=True)

    print("VERDICT:", "PASS" if not fails else "FAIL: " + "; ".join(fails))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
