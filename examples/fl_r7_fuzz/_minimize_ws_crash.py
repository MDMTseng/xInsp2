"""Narrow the iter-49 crash from harness_ws_cmd. Replay each of the
last-5 candidate payloads in a fresh backend and see which one(s)
kill it."""
from __future__ import annotations
import json, sys, time, subprocess
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from _common import BackendProc, open_ws, port_open

CANDIDATES = [
    ('set_param_args_array',  '{"type":"cmd","id":1,"name":"set_param","args":[1,2,3]}'),
    ('set_param_name_int',    '{"type":"cmd","id":1,"name":"set_param","args":{"name":42,"value":"foo"}}'),
    ('ping_trailing_garbage', '{"type":"cmd","id":1,"name":"ping"}\x00\x01\x02xyz}}}'),
    ('ping_ctrl_in_name',     '{"type":"cmd","id":1,"name":"\x07\x08\x0bping"}'),
]

def test(label: str, payload: str, repeat: int = 5) -> dict:
    with BackendProc() as bp:
        result = {"label": label, "payload": payload, "killed_at": None,
                  "proc_alive_after": None}
        ws = open_ws(timeout=5.0)
        try:
            for k in range(repeat):
                try:
                    ws.send(payload)
                except Exception as e:
                    result["killed_at"] = k
                    result["send_exc"] = repr(e)
                    break
                # drain
                try:
                    ws.settimeout(0.1)
                    while True:
                        ws.recv()
                except Exception:
                    pass
                # sleep + check
                time.sleep(0.2)
                if not bp.alive():
                    result["killed_at"] = k
                    break
        finally:
            try: ws.close()
            except Exception: pass
        time.sleep(0.5)
        result["proc_alive_after"] = bp.alive()
        result["port_open_after"] = port_open()
    return result

if __name__ == "__main__":
    out = []
    for label, payload in CANDIDATES:
        print(f"\n=== {label} ===", flush=True)
        r = test(label, payload, repeat=10)
        out.append(r)
        print(json.dumps(r, indent=2, ensure_ascii=False), flush=True)
    Path(__file__).parent.joinpath("_minimize_results.json").write_text(json.dumps(out, indent=2, ensure_ascii=False))
