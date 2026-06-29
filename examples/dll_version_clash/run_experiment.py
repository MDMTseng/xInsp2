"""
DLL version-clash experiment.

Two plugins (depprobe_a, depprobe_b) each load a dependency named `dep.dll` from
their OWN folder. We ship a *different version* of that same-named DLL into each
plugin's folder (v1 -> A, v2 -> B), load both into the one backend process, and
ask each which version it actually got.

  Phase 1 (full-path load, same base name): expect NO collision — the modern
    loader keys on the FULL PATH, so each plugin gets its own copy even though the
    base names match (A=v1, B=v2).

  Phase 2 (by-name load, same base name == a static import): expect a COLLISION —
    whoever loads first wins; the second plugin's by-name load returns the
    already-resident module, so it silently runs the wrong version (both report v1).

  Phase 3 (by-name load, distinct names): expect NO collision — different base
    names are independent modules, each plugin gets its own version (A=v1, B=v2).

Run:  python examples/dll_version_clash/run_experiment.py
(Windows only; needs the backend built + MSVC, which the backend points us at.)
"""
from __future__ import annotations
import json, os, subprocess, sys, tempfile, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
from xinsp2 import Client  # noqa: E402

BACKEND = REPO / "backend" / "build" / "Release" / "xinsp-backend.exe"
DEP_SRC = ROOT / "dep_src.c"
PORT = int(os.environ.get("PORT", "7865"))


def build_dep(vcvars: str, out_dll: Path, ver: int) -> None:
    """Compile dep_src.c -> out_dll with dep_version() returning `ver`.
    Builds in the plugin's own build/ dir (gitignored) — cl's .obj/.lib/.exp
    land there too, no temp-dir lock dance."""
    d = out_dll.parent
    d.mkdir(parents=True, exist_ok=True)
    bat = d / "_buildep.bat"
    bat.write_text(
        "@echo off\r\n"
        f'call "{vcvars}" >nul 2>nul\r\n'
        f'cl /nologo /LD /D VER={ver} "{DEP_SRC}" /Fe:"{out_dll}" >nul 2>nul\r\n',
        encoding="ascii",
    )
    subprocess.run(["cmd", "/c", str(bat)], cwd=str(d), check=False)
    if not out_dll.exists():
        raise RuntimeError(f"failed to build {out_dll}")


def probe(c: Client, instance: str, cmd: str) -> dict:
    # exchange_instance returns the plugin's JSON reply already parsed into a dict.
    r = c.call("exchange_instance", {"name": instance, "cmd": cmd})
    return r if isinstance(r, dict) else json.loads(r)


def main() -> int:
    if os.name != "nt":
        print("SKIP: Windows-only"); return 0
    if not BACKEND.exists():
        print(f"SKIP: backend not built ({BACKEND})"); return 0

    log = open(ROOT / "backend.log", "w", encoding="utf-8")
    proc = subprocess.Popen([str(BACKEND), f"--port={PORT}"], stdout=log,
                            stderr=subprocess.STDOUT, cwd=str(REPO))
    failures: list[str] = []
    try:
        c = None
        for _ in range(80):
            try:
                c = Client(url=f"ws://127.0.0.1:{PORT}/", timeout=120); c.connect(); c.ping(); break
            except Exception:
                time.sleep(0.5); c = None
        if not c:
            print("FAIL: backend never came up"); return 1

        # Opening the project compiles + loads both plugins.
        c.call("open_project", {"path": str(ROOT)})

        # Where did each plugin DLL land? (deps go right beside it.)
        dirA = Path(probe(c, "probeA", "whereami")["dir"])
        dirB = Path(probe(c, "probeB", "whereami")["dir"])
        print(f"probeA dir: {dirA}")
        print(f"probeB dir: {dirB}")

        # Toolchain: ask the backend where MSVC is (dogfoods toolchain_health).
        health = c.call("toolchain_health")
        vcvars = next(x["path"] for x in health["components"] if x["key"] == "vcvars")
        print(f"vcvars: {vcvars}\n")

        def report(tag, a, b):
            print(f"=== {tag} ===")
            print(f"  probeA[{a['mode']}]: version={a['version']:<2} loaded={a['loaded']}")
            print(f"  probeB[{b['mode']}]: version={b['version']:<2} loaded={b['loaded']}")

        # Each phase uses a UNIQUE base name so an earlier phase's resident module
        # doesn't taint the next. v1 -> probeA's folder, v2 -> probeB's folder.

        # ---- Phase 1: full-path load, SAME base name -------------------------
        build_dep(vcvars, dirA / "dep1.dll", 1); build_dep(vcvars, dirB / "dep1.dll", 2)
        a = probe(c, "probeA", "fullpath dep1.dll"); b = probe(c, "probeB", "fullpath dep1.dll")
        report("Phase 1 — full-path load, same base name 'dep1.dll'", a, b)
        if a["version"] == 1 and b["version"] == 2:
            print("  -> NO collision: the loader keys on the FULL PATH, so each plugin\n"
                  "     gets its own copy even though the base names match.\n")
        else:
            failures.append(f"phase1: expected A=1/B=2, got A={a['version']}/B={b['version']}")

        # ---- Phase 2: by-name load, SAME base name (models a static import) --
        build_dep(vcvars, dirA / "dep2.dll", 1); build_dep(vcvars, dirB / "dep2.dll", 2)
        a = probe(c, "probeA", "byname dep2.dll"); b = probe(c, "probeB", "byname dep2.dll")
        report("Phase 2 — by-name load, same base name 'dep2.dll' (== static import)", a, b)
        if a["version"] == b["version"]:
            print(f"  -> COLLISION: probeB asked for 'dep2.dll' and got probeA's already-loaded\n"
                  f"     v{a['version']} (loaded from {Path(b['loaded']).parent.name}'s folder). The loader matched by\n"
                  "     BASE NAME and reused the resident module — this is the classic DLL clash.\n")
        else:
            failures.append(f"phase2: expected a by-name collision (equal versions), got A={a['version']}/B={b['version']}")

        # ---- Phase 3: by-name load, DISTINCT names (the fix) -----------------
        build_dep(vcvars, dirA / "dep3a.dll", 1); build_dep(vcvars, dirB / "dep3b.dll", 2)
        a = probe(c, "probeA", "byname dep3a.dll"); b = probe(c, "probeB", "byname dep3b.dll")
        report("Phase 3 — by-name load, distinct names dep3a.dll / dep3b.dll (the fix)", a, b)
        if a["version"] == 1 and b["version"] == 2:
            print("  -> NO collision: distinct base names are independent modules.\n")
        else:
            failures.append(f"phase3: expected A=1/B=2, got A={a['version']}/B={b['version']}")

        print("VERDICT:", "PASS" if not failures else "FAIL " + "; ".join(failures))
        return 0 if not failures else 1
    finally:
        try: c and c.call("close_project")
        except Exception: pass
        proc.terminate()
        try: proc.wait(5)
        except Exception: proc.kill()


if __name__ == "__main__":
    sys.exit(main())
