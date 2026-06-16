"""
export_bundle.py — build a self-contained xInsp2 deployment bundle.

Runs on a DEV box (needs the MSVC toolchain + a built backend). It compiles the
project's script + plugins to DLLs, then assembles a folder you can copy to a
TARGET PC and run with NO toolchain (the backend loads the prebuilt DLLs via
--aot; the HMI runs in the machine's browser). See docs/guides/deploy.md.

    python tools/export_bundle.py <project_dir> <out_dir> [--fps N] [--port P]

Bundle layout:
    <out>/bin/      exes + runtime DLLs (OpenCV/accelerators) + VC++ redist
    <out>/project/  project.json (script -> script.dll), instances/, plugins/
                    (with build/*.dll), script.dll, dashboard*.json
    <out>/hmi/      static HMI artifact
    <out>/fe.json   FE launch config (project=./project, --aot via be_arg)
    <out>/run.bat   starts the FE supervisor (passes --aot to the backend)
    <out>/README.txt
"""
from __future__ import annotations
import json, os, shutil, subprocess, sys, time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
RELEASE = REPO / "backend" / "build" / "Release"
BACKEND = RELEASE / "xinsp-backend.exe"
HMI = REPO / "hmi"
# vcomp140.dll covers scripts compiled with project.json "openmp": true (/openmp).
# Harmless to bundle when unused; required when used.
VC_REDIST = ["msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll", "concrt140.dll",
             "vcomp140.dll"]
HMI_FILES = ["index.html", "app.mjs", "cards.mjs", "layout.mjs", "protocol.mjs"]


def log(m): print(f"[export] {m}")


def compile_via_backend(project: Path, port: int) -> str:
    """Open the project (compiles plugins) + compile the script. Returns the
    script DLL path. Uses a throwaway dev backend with the toolchain."""
    sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
    from xinsp2 import Client
    iso = Path(os.environ["LOCALAPPDATA"]) / "Temp" / "xi_export_dev"
    iso.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ); env["TEMP"] = env["TMP"] = str(iso)
    be = subprocess.Popen([str(BACKEND), f"--port={port}"], cwd=str(REPO), env=env,
                          stdout=open(iso / "dev_be.log", "w"), stderr=subprocess.STDOUT)
    try:
        c = None
        for _ in range(80):
            try: c = Client(url=f"ws://127.0.0.1:{port}/", timeout=60); c.connect(); c.ping(); break
            except Exception: time.sleep(0.5)
        if not c: raise RuntimeError("dev backend did not come up")
        log("opening project (compiles plugins)…")
        c.call("open_project", {"path": str(project)})          # -> plugins/<n>/build/*.dll
        script = json.load(open(project / "project.json")).get("script", "inspect.cpp")
        log(f"compiling script {script} (Release /O2)…")
        r = c.call("compile_and_load", {"path": str(project / script), "optimize": True}, timeout=300)
        dll = r.get("dll")
        if not dll or not Path(dll).exists(): raise RuntimeError(f"no script DLL from compile_and_load: {r}")
        c.call("close_project"); c.close()
        return dll
    finally:
        be.terminate()
        try: be.wait(5)
        except Exception: be.kill()


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = {a.split("=")[0]: (a.split("=")[1] if "=" in a else True) for a in sys.argv[1:] if a.startswith("--")}
    if os.name != "nt":
        log("Windows-only (MSVC bundle)"); return 0
    if len(args) < 2:
        print(__doc__); return 2
    if not BACKEND.exists():
        log(f"backend not built: {BACKEND}"); return 1
    project = Path(args[0]).resolve()
    out = Path(args[1]).resolve()
    fps = int(opts.get("--fps", 10))
    port = int(opts.get("--port", 7920))
    if not (project / "project.json").exists():
        log(f"no project.json in {project}"); return 1

    # 1. Compile script + plugins on the dev box.
    script_dll = compile_via_backend(project, port)
    log(f"script DLL: {script_dll}")

    # 2. Assemble the bundle.
    if out.exists(): shutil.rmtree(out)
    (out / "bin").mkdir(parents=True)
    (out / "hmi").mkdir(parents=True)

    # bin/: all exes + every DLL beside them (OpenCV/accelerators).
    for f in RELEASE.iterdir():
        if f.suffix.lower() in (".exe", ".dll") and not f.name.endswith(".locked.exe") \
                and not f.name.endswith(".locked2.exe"):
            shutil.copy2(f, out / "bin" / f.name)
    # VC++ redist (so the target needs nothing installed).
    sys32 = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32"
    redist_missing = []
    for d in VC_REDIST:
        src = sys32 / d
        if src.exists(): shutil.copy2(src, out / "bin" / d)
        else: redist_missing.append(d)
    if redist_missing: log(f"WARNING: VC++ redist not found, target needs it installed: {redist_missing}")

    # project/: copy the whole project (incl. plugins/<n>/build/*.dll), drop in the
    # script DLL, and point project.json at it.
    shutil.copytree(project, out / "project",
                    ignore=shutil.ignore_patterns(".xinsp_work", "snapshots", "*.log", "_results_*.json"))
    shutil.copy2(script_dll, out / "project" / "script.dll")
    pj = json.load(open(out / "project" / "project.json"))
    pj["script"] = "script.dll"           # AOT: backend loads the .dll directly
    pj.setdefault("aot", True)
    json.dump(pj, open(out / "project" / "project.json", "w"), indent=2)

    # hmi/: static artifact (the dashboard travels in the project; BE serves it).
    for f in HMI_FILES:
        if (HMI / f).exists(): shutil.copy2(HMI / f, out / "hmi" / f)

    # fe.json + run.bat + README.
    json.dump({"project": "./project", "autostart_fps": fps, "be_log": "./be.log",
               "safe_state": "log", "be_args": ["--aot"]},
              open(out / "fe.json", "w"), indent=2)
    (out / "run.bat").write_text(
        "@echo off\r\n"
        "set HERE=%~dp0\r\n"
        'start "" "%HERE%bin\\xinsp-fe.exe" --project="%HERE%project" '
        f'--autostart-fps={fps} --be-arg=--aot --be-log="%HERE%be.log"\r\n'
        'echo xInsp2 started. Open the HMI in a browser:\r\n'
        '  echo   file:///%HERE%hmi/index.html?ws=ws://127.0.0.1:7823/\r\n',
        encoding="utf-8")
    (out / "README.txt").write_text(
        "xInsp2 deployment bundle (AOT — no toolchain needed on this machine).\r\n\r\n"
        "Run:  double-click run.bat  (starts FE -> backend --aot -> comms).\r\n"
        "HMI:  open  hmi\\index.html?ws=ws://127.0.0.1:7823/  in Edge/Chrome\r\n"
        "      (kiosk: msedge --kiosk --app=file:///.../hmi/index.html?ws=ws://127.0.0.1:7823/).\r\n\r\n"
        "Requires: Windows x64. VC++ runtime DLLs are bundled in bin\\.\r\n"
        "The script + plugins are PREBUILT (.dll); the backend loads them with --aot.\r\n",
        encoding="utf-8")

    n_dll = len(list((out / "bin").glob("*.dll")))
    log(f"bundle ready: {out}  ({n_dll} DLLs in bin/)")
    log("copy the whole folder to the target PC and run run.bat.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
