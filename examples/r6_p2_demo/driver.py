"""r6_p2_demo — exercises FL r6 deferred P2 fixes end-to-end.

Verifies:
  P2-2  t.primary_source() / t.has_source(name) on a single-source project
  P2-4  exchange returns {"error":"unknown_command", ...} on bad cmd
  P2-5  sources() handles short names without truncation (smoke; the heap
        retry path is exercised when leader name > 256 bytes — out of scope
        for this demo, asserted by the implementation contract)
  P2-6  compile_started / compile_finished events arrive

Run from this dir:

    python driver.py
"""
from __future__ import annotations

import json
import sys
import time
from pathlib import Path
from queue import Empty

from xinsp2 import Client

ROOT = Path(__file__).parent
COLLECT_S = 1.5

OK   = "PASS"
FAIL = "FAIL"


def main() -> int:
    print("r6_p2_demo — P2-2 / P2-4 / P2-6 smoke")
    results: list[tuple[str, str, str]] = []

    with Client() as c:
        # P2-6: open_project triggers project plugin compile too, but the
        # `compile_started` / `compile_finished` events fire specifically
        # for cmd:compile_and_load (the script-side compile). Subscribe
        # to events first so we don't miss them.
        c.open_project(str(ROOT))

        # Drain events queue, then issue compile_and_load and watch for
        # the bracketing events.
        try:
            while True: c._inbox_events.get_nowait()
        except Empty:
            pass

        compile_started = False
        compile_finished = False
        compile_finished_ok = None

        # compile_and_load is sync — but the events fire on the WS thread
        # before/after the rsp. They land in _inbox_events.
        c.compile_and_load(str(ROOT / "inspect.cpp"))

        # Collect events for a short window — they should all be there
        # already since compile_and_load returned synchronously.
        deadline = time.monotonic() + 1.0
        while time.monotonic() < deadline:
            try:
                ev = c._inbox_events.get(timeout=0.1)
            except Empty:
                continue
            n = ev.get("name")
            if n == "compile_started":
                compile_started = True
            elif n == "compile_finished":
                compile_finished = True
                compile_finished_ok = ev.get("data", {}).get("ok")

        results.append(("P2-6 compile_started event",
                        OK if compile_started else FAIL,
                        "received" if compile_started else "missing"))
        results.append(("P2-6 compile_finished event",
                        OK if (compile_finished and compile_finished_ok) else FAIL,
                        f"ok={compile_finished_ok}"))

        # P2-4: send a bogus exchange command and assert error shape.
        try:
            rsp = c.call("exchange_instance",
                         {"name": "src_main", "command": "burts"})
            # rsp is the parsed JSON return from the plugin's exchange().
            # Shape varies: SDK may return raw dict or wrap it.
            payload = rsp if isinstance(rsp, dict) else {}
            # Walk a couple levels — the cmd handler typically wraps the
            # plugin reply under a "data" / "rsp" key. Grab the inner dict.
            inner = payload
            for k in ("rsp", "data", "result"):
                if isinstance(inner, dict) and k in inner and isinstance(inner[k], dict):
                    inner = inner[k]
            # Some plugins return the manifest; the helper returns the
            # canonical error shape. Check both top-level and inner.
            err_kind = (payload.get("error")
                        if isinstance(payload, dict) else None) or \
                       (inner.get("error")
                        if isinstance(inner, dict) else None)
            ok = err_kind == "unknown_command"
            results.append(("P2-4 exchange_unknown_command",
                            OK if ok else FAIL,
                            f"error={err_kind!r}, payload={json.dumps(payload)[:160]}"))
        except Exception as e:
            results.append(("P2-4 exchange_unknown_command", FAIL,
                            f"call raised: {e!r}"))

        # P2-2 + P2-5: run for a moment, collect VAR events, assert the
        # script-side accessors returned the right values.
        c.call("start", {"fps": 1})

        primary_seen   = set()
        has_main_seen  = set()
        has_other_seen = set()
        n_sources_seen = set()

        deadline = time.monotonic() + COLLECT_S
        while time.monotonic() < deadline:
            try:
                ev = c._inbox_vars.get(timeout=0.1)
            except Empty:
                continue
            items = {it["name"]: it.get("value") for it in (ev.get("items") or [])}
            if items.get("active") is not True:
                continue
            if "primary"   in items: primary_seen.add(items["primary"])
            if "has_main"  in items: has_main_seen.add(items["has_main"])
            if "has_other" in items: has_other_seen.add(items["has_other"])
            if "n_sources" in items: n_sources_seen.add(items["n_sources"])

        c.call("stop")

        results.append(("P2-2 primary_source == src_main",
                        OK if primary_seen == {"src_main"} else FAIL,
                        f"saw {primary_seen}"))
        results.append(("P2-2 has_source('src_main')==True",
                        OK if has_main_seen == {True} else FAIL,
                        f"saw {has_main_seen}"))
        results.append(("P2-2 has_source('nope')==False",
                        OK if has_other_seen == {False} else FAIL,
                        f"saw {has_other_seen}"))
        results.append(("P2-5 sources().size()==1",
                        OK if n_sources_seen == {1} else FAIL,
                        f"saw {n_sources_seen}"))

    print()
    print(f"{'check':<40}{'result':<8}{'detail'}")
    print("-" * 80)
    for label, status, detail in results:
        print(f"{label:<40}{status:<8}{detail}")
    print("-" * 80)
    n_pass = sum(1 for _, s, _ in results if s == OK)
    n_total = len(results)
    print(f"{n_pass}/{n_total} checks passed")
    return 0 if n_pass == n_total else 1


if __name__ == "__main__":
    sys.exit(main())
