"""FL r7 target #4 — emit_trigger / RPC_EMIT_TRIGGER fuzzer (in-process side).

The RPC_EMIT_TRIGGER frames are *originated by workers* and consumed by
the host's TriggerBus singleton via xi_process_instance.hpp. The
worker-side raw injection lives in target #3 (evil_worker.cpp). This
harness covers the *in-process* path: a script / plugin running
in-process calling `host->emit_trigger(ev)` directly, with adversarial
TriggerEvent payloads. We drive that via the WS `cmd:emit_trigger`
surface if it exists, OR via `exchange_instance` on a source plugin
that supports a `burst` / `emit` command.

In practice the multi_source_surge example exposes `exchange_instance`
with a `burst` command on `burst_source`. We drive that with malformed
args (huge counts, negative counts, missing fields, etc.) and check
the trigger bus survives.

Pass criteria
-------------
- Backend stays pingable.
- exchange_instance returns either ok-with-data or ProtocolError.
- No silent socket close.
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
from _common import BackendProc, WS_URL  # noqa: E402

from xinsp2 import Client, ProtocolError  # noqa: E402

ITERS = int(os.environ.get("FUZZ_ITERS", "800"))
SEED = int(os.environ.get("FUZZ_SEED", "9090"))
PROJECT = Path(__file__).resolve().parents[1] / "multi_source_surge"


def rstr(rng: random.Random, n: int) -> str:
    alpha = string.ascii_letters + string.digits + "_-./ \t\n\r\x00"
    return "".join(rng.choice(alpha) for _ in range(n))


def gen_exchange_args(rng: random.Random) -> dict:
    cmd = rng.choice([
        "burst", "set_fps", "start", "stop",
        "no_such_command", "", "burst" + rstr(rng, rng.randint(0, 200)),
        "BURST",  # case mismatch
    ])
    args: dict = {"command": cmd}
    extra = rng.choice([
        {"count": rng.randint(-10**9, 10**9)},
        {"count": rng.choice([0, 1, -1, 2**31, 2**31 - 1, -2**31, "huge"])},
        {"value": rng.choice([None, "x", -1, 1.5e308, 999999])},
        {"count": [1, 2, 3]},
        {"count": {"nested": True}},
        {rstr(rng, 8): rstr(rng, rng.randint(0, 200))},
        {"command": "second"},  # duplicate-ish key (will be merged by dict)
        {},
        {"count": rng.randint(0, 10)},
    ])
    args.update(extra)
    return args


def gen_set_param_args(rng: random.Random) -> dict:
    # Hits the trigger-related side: set_param affects sources too.
    return {
        "name": rng.choice(["fps", "burst", "trigger_source", "x" * 1000, ""]),
        "value": rng.choice([None, "", 0, -1, 99999, [1, 2], {"a": 1},
                              "\x00\x01embed", "x" * 5000]),
    }


def main() -> int:
    rng = random.Random(SEED)
    findings: list[dict] = []
    print(f"[harness_emit_trigger] iters={ITERS} seed={SEED}", flush=True)

    last_status_t = time.time()

    with BackendProc():
        c = Client(WS_URL)
        c.connect()
        try:
            # Open the multi_source_surge project so we have real source instances.
            try:
                c.open_project(str(PROJECT), timeout=120)
            except ProtocolError as e:
                print(f"[harness_emit_trigger] open_project failed: {e}; "
                      f"continuing without project — only set_param paths covered.")
                findings.append({"setup": "open_project_failed", "err": str(e)[:300]})
                # Continue — we'll just get errors from exchange_instance, no crash test value lost
            except Exception as e:
                print(f"[harness_emit_trigger] open_project exception: {e!r}")

            instances = ["source_steady", "source_burst", "source_variable",
                         "detector_fast", "detector_slow"]
            try:
                # Compile the inspect script too if available
                inspect = PROJECT / "inspect.cpp"
                if inspect.exists():
                    try:
                        c.compile_and_load(str(inspect))
                    except Exception as e:
                        print(f"[harness_emit_trigger] compile_and_load failed: {e!r}; ok, continuing")
            except Exception:
                pass

            for i in range(ITERS):
                inst = rng.choice(instances + ["nonexistent_inst", "", "x" * 200])
                op = rng.choice(["exchange", "exchange", "set_param"])
                err = None
                try:
                    if op == "exchange":
                        c.exchange_instance(inst, gen_exchange_args(rng))
                    else:
                        c.call("set_param", gen_set_param_args(rng), timeout=5.0)
                except ProtocolError as e:
                    err = {"kind": "protocol_error", "msg": str(e)[:300]}
                except Exception as e:
                    err = {"kind": "exception", "msg": repr(e)[:400]}

                # liveness probe
                try:
                    c.ping()
                except Exception as e:
                    findings.append({
                        "iter": i, "seed": SEED, "kind": "ping_failed",
                        "instance": inst, "op": op, "exc": repr(e)[:300],
                        "fatal": True,
                    })
                    print(f"[CRASH] ping failed at iter {i}: {e!r}", flush=True)
                    break

                if err and err["kind"] == "exception":
                    findings.append({
                        "iter": i, "seed": SEED, "kind": "non_protocol_exception",
                        "op": op, "instance": inst, "err": err,
                    })

                if (i + 1) % 100 == 0 and time.time() - last_status_t > 8.0:
                    print(f"[harness_emit_trigger] iter={i+1}/{ITERS}", flush=True)
                    last_status_t = time.time()
                    try:
                        prog = Path(__file__).resolve().parents[2] / ".fl_progress" / "fl_r7_fuzz.txt"
                        with open(prog, "a", encoding="utf-8") as f:
                            f.write(f"{time.strftime('%H:%M:%S')}  emit_trigger iter {i+1}/{ITERS}\n")
                    except Exception:
                        pass
        finally:
            try: c.shutdown()
            except Exception: pass
            try: c.close()
            except Exception: pass

    out = Path(__file__).parent / "_results_emit_trigger.json"
    out.write_text(json.dumps({"iters": ITERS, "seed": SEED, "findings": findings}, indent=2))
    print(f"[harness_emit_trigger] done. findings={len(findings)}")
    return 0 if not any(f.get("fatal") for f in findings) else 1


if __name__ == "__main__":
    sys.exit(main())
