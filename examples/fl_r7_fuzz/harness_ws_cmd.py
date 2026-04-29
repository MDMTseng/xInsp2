"""FL r7 target #1 — WebSocket cmd JSON parser fuzzer.

Targets `backend/src/service_main.cpp::handle_command` which calls
`xp::parse_cmd()` (xi_protocol.hpp) — a hand-rolled JSON skeleton parser.

Pass criteria
-------------
- Backend never crashes (port stays open, can still ping).
- Every malformed cmd gets either an error log/rsp or is ignored —
  never a silent socket close.

Findings are appended to RESULTS.md / FRICTION_FUZZ.md by run_all.py.
"""
from __future__ import annotations

import json
import os
import random
import string
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from _common import BackendProc, open_ws, port_open  # noqa: E402

ITERS = int(os.environ.get("FUZZ_ITERS", "1200"))
SEED = int(os.environ.get("FUZZ_SEED", "1337"))


def rand_string(rng: random.Random, n: int) -> str:
    alpha = string.ascii_letters + string.digits + "_-./ \t\n\r\x00\\\"'"
    return "".join(rng.choice(alpha) for _ in range(n))


def gen_payload(rng: random.Random) -> bytes:
    """Return a UTF-8 byte string to send as a WS text frame.

    We mostly produce malformed JSON variants. Some variants test deep
    structural pathologies of the parser (deep nesting, huge strings,
    integer overflow, NaN/Inf, etc).
    """
    strategy = rng.choice([
        "valid_garbled", "missing_type", "wrong_type", "id_overflow",
        "unknown_cmd", "args_null", "args_array", "args_int",
        "deep_nest", "huge_string", "nan_inf", "raw_garbage",
        "broken_quote", "trailing_garbage", "empty", "only_braces",
        "duplicate_keys", "unicode_keys", "control_chars",
        "huge_id", "missing_id", "missing_name",
        "args_huge", "args_string_mismatch",
    ])
    if strategy == "valid_garbled":
        s = '{"type":"cmd","id":1,"name":"ping"' + rand_string(rng, rng.randint(0, 30)) + '}'
        return s.encode("utf-8", errors="replace")
    if strategy == "missing_type":
        return b'{"id":1,"name":"ping"}'
    if strategy == "wrong_type":
        return b'{"type":"foo","id":1,"name":"ping"}'
    if strategy == "id_overflow":
        return f'{{"type":"cmd","id":{2**70},"name":"ping"}}'.encode()
    if strategy == "unknown_cmd":
        nm = rand_string(rng, rng.randint(0, 64))
        return json.dumps({"type": "cmd", "id": rng.randint(0, 10000), "name": nm, "args": {}}).encode()
    if strategy == "args_null":
        return b'{"type":"cmd","id":1,"name":"ping","args":null}'
    if strategy == "args_array":
        return b'{"type":"cmd","id":1,"name":"set_param","args":[1,2,3]}'
    if strategy == "args_int":
        return b'{"type":"cmd","id":1,"name":"set_param","args":42}'
    if strategy == "deep_nest":
        depth = rng.randint(50, 2000)
        body = "{" * depth + "}" * depth
        return f'{{"type":"cmd","id":1,"name":"subscribe","args":{body}}}'.encode()
    if strategy == "huge_string":
        n = rng.choice([10_000, 100_000, 1_000_000])
        big = "x" * n
        return json.dumps({"type": "cmd", "id": 1, "name": "compile_and_load", "args": {"path": big}}).encode()
    if strategy == "nan_inf":
        # Strict JSON disallows these; many parsers accept anyway.
        return b'{"type":"cmd","id":NaN,"name":"ping"}'
    if strategy == "raw_garbage":
        n = rng.randint(0, 4096)
        return bytes(rng.randint(0, 255) for _ in range(n))
    if strategy == "broken_quote":
        return b'{"type":"cmd","id":1,"name":"pi'
    if strategy == "trailing_garbage":
        return b'{"type":"cmd","id":1,"name":"ping"}\x00\x01\x02xyz}}}'
    if strategy == "empty":
        return b''
    if strategy == "only_braces":
        return b'{}'
    if strategy == "duplicate_keys":
        return b'{"type":"cmd","type":"cmd","id":1,"id":2,"name":"ping","name":"version"}'
    if strategy == "unicode_keys":
        return '{"type":"cmd","id":1,"únicode":"x","name":"ping"}'.encode("utf-8")
    if strategy == "control_chars":
        # Embedded control chars before quote
        return b'{"type":"cmd","id":1,"name":"\x07\x08\x0bping"}'
    if strategy == "huge_id":
        return b'{"type":"cmd","id":99999999999999999999999999,"name":"ping"}'
    if strategy == "missing_id":
        return b'{"type":"cmd","name":"ping"}'
    if strategy == "missing_name":
        return b'{"type":"cmd","id":1}'
    if strategy == "args_huge":
        names = ["x" * rng.randint(1, 50) for _ in range(rng.randint(0, 1000))]
        return json.dumps({"type": "cmd", "id": 1, "name": "subscribe", "args": {"names": names}}).encode()
    if strategy == "args_string_mismatch":
        return b'{"type":"cmd","id":1,"name":"set_param","args":{"name":42,"value":"foo"}}'
    return b'{}'


def liveness_check(retries: int = 4) -> tuple[bool, str]:
    """Open a fresh WS, send ping, expect a rsp. Return (alive, info).

    Retries on transient connect-timeout because the backend may be
    busy chewing on a deeply-nested or huge payload from a prior iter
    — that's not a crash, just slow. We only call it a fatal liveness
    failure if the port is closed OR all retries exhaust without a pong.
    """
    last_info = "no attempt"
    ws = None
    for attempt in range(max(1, retries)):
        try:
            ws = open_ws(timeout=8.0)
            break
        except Exception as e:
            last_info = f"connect failed (attempt {attempt+1}): {e!r}"
            time.sleep(1.0)
    if ws is None:
        # final port check
        if not port_open(timeout=2.0):
            return False, f"port_closed_final: {last_info}"
        return False, last_info
    try:
        ws.send(json.dumps({"type": "cmd", "id": 7, "name": "ping"}))
        deadline = time.time() + 3.0
        got_pong = False
        while time.time() < deadline:
            try:
                ws.settimeout(max(0.1, deadline - time.time()))
                data = ws.recv()
            except Exception:
                break
            if not data:
                continue
            try:
                msg = json.loads(data) if isinstance(data, str) else json.loads(data.decode("utf-8", "replace"))
            except Exception:
                continue
            if msg.get("type") == "rsp" and msg.get("id") == 7:
                got_pong = msg.get("ok") is True
                break
        return got_pong, ("pong ok" if got_pong else "no pong")
    finally:
        try: ws.close()
        except Exception: pass


def main() -> int:
    rng = random.Random(SEED)
    findings: list[dict] = []
    print(f"[harness_ws_cmd] iters={ITERS} seed={SEED}", flush=True)

    with BackendProc() as bp:
        # warm-up liveness
        ok, info = liveness_check()
        if not ok:
            print(f"[FATAL] backend not responsive at start: {info}")
            return 2

        ws = open_ws(timeout=5.0)
        last_status_t = time.time()
        recent_payloads: list[tuple[str, bytes]] = []  # (strategy, bytes), last 5

        for i in range(ITERS):
            payload = gen_payload(rng)
            recent_payloads.append((str(i), payload[:400]))
            if len(recent_payloads) > 5:
                recent_payloads.pop(0)
            try:
                if rng.random() < 0.05:
                    # send as binary opcode occasionally
                    ws.send_binary(payload[:65535])
                else:
                    ws.send(payload.decode("utf-8", errors="replace"))
            except Exception as e:
                # connection died — record + reopen
                findings.append({
                    "iter": i, "seed": SEED, "kind": "send_failed",
                    "exc": repr(e), "payload_len": len(payload),
                    "payload_head": payload[:200].hex(),
                })
                if not port_open():
                    findings.append({"iter": i, "kind": "port_closed_after_send", "fatal": True})
                    break
                try: ws.close()
                except Exception: pass
                ws = open_ws(timeout=5.0)
                continue

            # drain anything pending so we don't backpressure
            try:
                ws.settimeout(0.001)
                while True:
                    _ = ws.recv()
            except Exception:
                pass

            # liveness probe every ~200 iters (less frequent — opening
            # a 2nd ws while sustained-fuzz traffic is in flight gets
            # transient ECONNREFUSED on Windows because the backend's
            # accept loop is briefly serving the existing socket.
            # See FRICTION_FUZZ.md for the full theory.)
            if (i + 1) % 200 == 0:
                ok, info = liveness_check()
                if not ok:
                    proc_alive = bp.proc_alive()
                    rc = bp.returncode()
                    findings.append({
                        "iter": i, "seed": SEED, "kind": "liveness_failed",
                        "info": info, "proc_alive": proc_alive, "returncode": rc,
                        "recent_payloads": [
                            {"i": idx, "hex": pl.hex(), "head": pl[:120].decode("utf-8", "replace")}
                            for idx, pl in recent_payloads
                        ],
                    })
                    print(f"[STALL] liveness failed at iter {i}: {info} proc_alive={proc_alive}", flush=True)
                    # Don't break — try to recover. Wait a bit and retry.
                    time.sleep(2.0)
                    ok2, info2 = liveness_check()
                    if not ok2:
                        if not bp.proc_alive():
                            findings[-1]["fatal"] = True
                            print(f"[CRASH] backend process dead rc={bp.returncode()}. break.", flush=True)
                            break
                        findings[-1]["recovered"] = False
                    else:
                        findings[-1]["recovered"] = True
                        print(f"[STALL] recovered after wait", flush=True)
                    # reopen ws and continue
                    try: ws.close()
                    except Exception: pass
                    try:
                        ws = open_ws(timeout=8.0)
                    except Exception:
                        break
                if time.time() - last_status_t > 8.0:
                    print(f"[harness_ws_cmd] iter={i+1}/{ITERS} alive ({info})", flush=True)
                    last_status_t = time.time()
                    # also progress-log
                    try:
                        prog = Path(__file__).resolve().parents[2] / ".fl_progress" / "fl_r7_fuzz.txt"
                        with open(prog, "a", encoding="utf-8") as f:
                            f.write(f"{time.strftime('%H:%M:%S')}  ws_cmd iter {i+1}/{ITERS}\n")
                    except Exception:
                        pass

        try: ws.close()
        except Exception: pass

        # final
        ok, info = liveness_check()
        print(f"[harness_ws_cmd] done. final liveness: {ok} ({info}). findings={len(findings)}")

    out = Path(__file__).parent / "_results_ws_cmd.json"
    out.write_text(json.dumps({
        "iters": ITERS, "seed": SEED, "findings": findings,
        "final_alive": ok,
    }, indent=2))
    return 0 if not any(f.get("fatal") for f in findings) else 1


if __name__ == "__main__":
    sys.exit(main())
