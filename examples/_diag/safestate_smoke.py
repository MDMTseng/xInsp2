"""Phase C-2 smoke — the BE-crash error half of comms-as-plugin. A plugin
registers an emergency payload (host->set_safe_state), then crashes uncatchably.
The BE persists the payload to <project>/.xinsp_safestate before dying; the FE
(which outlives the BE and owns the PLC safe-state sink) reads it and forwards it
to the PLC. We assert the UDP "PLC" receives a safe_state ENTER carrying our
payload — proving the guarantee survives without resident comms in core.

Self-contained (FE + UDP PLC sim), modelled on examples/plc_safe_state.
Windows-only (xinsp-fe + PLC sink); SKIPs on non-nt.
"""
from __future__ import annotations
import json
import os
import socket
import subprocess
import sys
import time
from pathlib import Path
from harness import ROOT, REPO_ROOT, FE_EXE, safe_kill

PROJ = ROOT / "safestate_proj"
PORT = 7915


def main() -> int:
    if os.name != "nt":
        print("SKIP: xinsp-fe / PLC sink are Windows-only"); return 0
    if not FE_EXE.exists():
        print("SKIP: xinsp-fe not built"); return 0
    # stale payload from a prior run would be a false pass — clear it.
    try: (PROJ / ".xinsp_safestate").unlink()
    except FileNotFoundError: pass

    plc = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    plc.bind(("127.0.0.1", 0))
    plc_port = plc.getsockname()[1]
    plc.settimeout(0.5)
    print(f"[plc-sim] udp 127.0.0.1:{plc_port}")

    fe_log = open(ROOT / "safestate_fe.log", "wb")
    fe = subprocess.Popen(
        [str(FE_EXE), f"--port={PORT}", f"--project={PROJ}", "--autostart-fps=5",
         f"--safe-state=udp:127.0.0.1:{plc_port}", f"--be-log={ROOT / 'safestate_be.log'}"],
        cwd=str(FE_EXE.parent), stdout=fe_log, stderr=fe_log, stdin=subprocess.DEVNULL,
        creationflags=subprocess.CREATE_NEW_PROCESS_GROUP,
    )
    try:
        enters = []
        deadline = time.time() + 60
        while time.time() < deadline:
            try:
                data, _ = plc.recvfrom(4096)
            except socket.timeout:
                continue
            for line in data.decode("utf-8", "replace").splitlines():
                line = line.strip()
                if not line:
                    continue
                try:
                    m = json.loads(line)
                except Exception:
                    continue
                if m.get("event") == "safe_state" and m.get("state") == "enter":
                    enters.append(m)
            if enters:
                break

        print("\n==== SAFE-STATE PAYLOAD ====")
        with_payload = [m for m in enters if "estop" in str(m.get("payload", ""))]
        if enters:
            print("enter msg:", json.dumps(enters[0]))
        passed = bool(with_payload)
        print("VERDICT:", "PASS" if passed else "FAIL")
        return 0 if passed else 1
    finally:
        safe_kill(fe, "xinsp-fe.exe")
        plc.close()


if __name__ == "__main__":
    raise SystemExit(main())
