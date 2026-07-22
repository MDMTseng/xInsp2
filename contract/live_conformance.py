#!/usr/bin/env python3
"""Live-wire conformance — the THIRD contract leg (schemas <-> fixtures <-> LIVE bytes).

contract/ describes the xInsp2 wire two ways, and until now BOTH were hand-authored
mirrors of the C++:

  * schemas/       — the shapes, written by hand.
  * protocol/fixtures + contract/examples — sample bytes, written by hand.

validate.py checks the fixtures against the schemas. But nothing checked either
against what the LIVE backend actually sends. A C++ change that alters the wire
and skips the schema+fixture update passes every gate green — the two mirrors
still agree with each other; they just no longer describe reality. That is the
gap polaris2 vision A named.

This test closes it. It spawns the REAL backend, drives a representative session,
captures every inbound message, and:

  1. VALIDATES each captured message whose (type + command/name) a schema
     describes, against that schema with jsonschema — the SAME discriminator
     logic fixtures-map.json encodes, applied to live bytes. Events route by their
     schema's own `type`/`name` consts (auto-derived); rsps route by originating
     command via live-wire-map.json; every rsp is additionally envelope-checked
     against the generic rsp.schema.json (which no fixture otherwise exercises).
     A live message that violates its schema FAILS the test, printing the offender.

  2. RATCHETS every message no schema describes against contract/live-allowlist.json:
     a known key is counted and reported; an UNKNOWN key FAILS ('a new unschema'd
     wire message shipped — add a schema or allowlist it'). New message types can
     never reach the wire silently undescribed.

  3. COVERAGE: asserts the session actually observed the load-bearing schema'd
     messages (hello, run_result/finished, health_changed, dispatch_stats,
     metrics, get_health, commit_group, load_project) — so a green here means the
     wire was exercised, not that the session quietly produced nothing.

If the live bytes DISAGREE with a schema, the BACKEND is truth: fix the schema (and
refresh contract/baseline via `python contract/baseline_gate.py --update-baseline`)
in the same commit — that is this test earning its keep.

Self-skips (exit 0, loud 'SKIP - NOT A PASS') when the backend exe is not built or
a Python dep (jsonschema/referencing, or the xinsp2 websocket client) is missing,
mirroring contract/validate.py's hardened skip — UNLESS XINSP2_REQUIRE_SCHEMA_GATE
is set, which turns any would-be skip into a hard FAIL. gate.py's `live` stage runs
this script that way, under the gate's own python, against the freshly built
backend. It is also wired as the `contract_live` ctest (label 'contract') for bare
`ctest` completeness; gate.py excludes that duplicate (-E) since the session is
expensive (cold cl.exe compile).

    python contract/live_conformance.py
"""
from __future__ import annotations

import contextlib
import json
import os
import socket
import tempfile
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CONTRACT = ROOT / "contract"
SCHEMA_DIR = CONTRACT / "schemas"
MAP_PATH = CONTRACT / "live-wire-map.json"
ALLOWLIST_PATH = CONTRACT / "live-allowlist.json"
def _backend_exe() -> Path:
    """The built backend, cross-platform + build-layout aware.

    `.exe` only on Windows; probe the gate's multi-config Release/Debug dirs
    and the Linux single-config build-linux tree. XINSP_BACKEND_EXE overrides
    (CI / non-default build dirs). Returns the first that exists, else the
    canonical Release candidate so a missing build raises a clear error."""
    override = os.environ.get("XINSP_BACKEND_EXE")
    if override:
        return Path(override)
    suf = ".exe" if os.name == "nt" else ""
    base = ROOT / "backend"
    cands = [base / b / f"xinsp-backend{suf}"
             for b in ("build/Release", "build/Debug", "build-linux", "build")]
    return next((c for c in cands if c.exists()), cands[0])


BACKEND = _backend_exe()
PROJECT = ROOT / "qa" / "qa_recipe_script_instance"

REQUIRE = bool(os.environ.get("XINSP2_REQUIRE_SCHEMA_GATE"))


def _skip(msg: str) -> int:
    # Mirror validate.py: a genuine environment gap is a loud SKIP, not a silent
    # pass — unless the gate.py-set env var forbids skipping.
    if REQUIRE:
        print(f"contract-live: FAIL - {msg}; XINSP2_REQUIRE_SCHEMA_GATE is set, "
              f"so skipping is not allowed.")
        return 1
    print(f"contract-live: SKIP (NOT A PASS) - {msg}; the live wire was NOT checked.")
    return 0


# --- deps + build artifact (skip if absent) --------------------------------- #
if not BACKEND.exists():
    raise SystemExit(_skip(f"backend not built at {BACKEND.relative_to(ROOT).as_posix()}"))

try:
    from jsonschema import Draft202012Validator
    from jsonschema.exceptions import best_match
    from referencing import Registry, Resource
    from referencing.jsonschema import DRAFT202012
except ImportError as e:  # pragma: no cover - environment guard
    raise SystemExit(_skip(f"jsonschema/referencing not installed ({e}); pip install jsonschema"))

sys.path.insert(0, str(ROOT / "tools" / "xinsp2_py"))
try:
    from xinsp2 import Client  # noqa: E402  (needs websocket-client)
except ImportError as e:  # pragma: no cover - environment guard
    raise SystemExit(_skip(f"xinsp2 client unavailable ({e}); pip install websocket-client"))


def load(p: Path) -> dict:
    return json.loads(p.read_text(encoding="utf-8"))


# --- schema registry (mirrors validate.py) ---------------------------------- #
def build_registry():
    schemas: dict[str, dict] = {}
    resources: list[tuple[str, Resource]] = []
    for sp in sorted(SCHEMA_DIR.glob("*.json")):
        doc = load(sp)
        schemas[sp.name] = doc
        sid = doc.get("$id")
        if sid:
            resources.append((sid, Resource(contents=doc, specification=DRAFT202012)))
    registry = Registry().with_resources(resources)
    return schemas, registry


def event_schema_by_name(schemas: dict[str, dict]) -> dict[str, str]:
    """Auto-derive event routing from each schema's own consts.

    A schema whose `properties.type.const == 'event'` and `properties.name.const`
    is a string describes the event of that name. This is exactly the dispatch
    table contract/README.md says a real build would generate from the schemas.
    """
    out: dict[str, str] = {}
    for name, doc in schemas.items():
        props = doc.get("properties", {})
        tconst = props.get("type", {}).get("const")
        nconst = props.get("name", {}).get("const")
        if tconst == "event" and isinstance(nconst, str):
            if nconst in out:
                raise SystemExit(f"contract-live: FAIL - two schemas claim event "
                                 f"{nconst!r}: {out[nconst]} and {name}")
            out[nconst] = name
    return out


# --- capturing client ------------------------------------------------------- #
class CapturingClient(Client):
    """A Client that records every inbound TEXT message and the command each
    outbound cmd id carries, so a captured rsp can be routed by its command."""

    def __init__(self, *a, **k):
        super().__init__(*a, **k)
        self.captured: list[dict] = []
        self.id_to_cmd: dict[int, str] = {}
        self._cap_lock = threading.Lock()

    def call(self, name, args=None, timeout=None):
        # The id super().call() is about to use is the current _next_id; the
        # driver sends single-threaded, so recording it here is race-free.
        with self._cap_lock:
            self.id_to_cmd[self._next_id] = name
        return super().call(name, args, timeout)

    def _handle_text(self, msg: dict):
        with self._cap_lock:
            self.captured.append(msg)
        super()._handle_text(msg)


# --- backend spawn (mirrors tests/test_auth_live.py) ------------------------ #
def _free_port() -> int:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


@contextlib.contextmanager
def spawn_backend():
    port = _free_port()
    # Isolate the temp dir so a parallel gate run's build cache/state doesn't
    # collide. This used to be Path(LOCALAPPDATA or TEMP or ".") / "Temp" / ...
    # which on POSIX (neither var set) resolved to a RELATIVE "./Temp/..." —
    # created relative to THIS process's cwd, then handed to a backend spawned
    # with cwd=ROOT. It only ever worked because a stray <repo>/Temp/ happened to
    # exist; delete that and the backend aborts on the missing dir. Absolute, and
    # TMPDIR too, since that is the POSIX knob (TEMP/TMP are the Windows ones).
    iso = Path(tempfile.gettempdir()) / "xi_contract_live"
    iso.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env["TMPDIR"] = env["TEMP"] = env["TMP"] = str(iso)
    proc = subprocess.Popen(
        [str(BACKEND), f"--port={port}", "--host=127.0.0.1"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, cwd=str(ROOT), env=env,
    )
    try:
        deadline = time.time() + 25
        while time.time() < deadline:
            if proc.poll() is not None:
                out = proc.stdout.read() if proc.stdout else ""
                raise RuntimeError(f"backend exited early ({proc.returncode}): {out}")
            with contextlib.suppress(OSError):
                with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                    break
            time.sleep(0.1)
        else:
            raise RuntimeError("backend did not open its port in time")
        yield f"ws://127.0.0.1:{port}/"
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=8)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)


# --- the representative session --------------------------------------------- #
def drive_session(c: CapturingClient) -> None:
    """Exercise the load-bearing wire surface. Best-effort per step: a step that
    raises (e.g. a partial-status helper) is swallowed AFTER its bytes are already
    captured — we validate what the backend SENT, not whether the helper liked it."""
    def _try(label, fn):
        try:
            fn()
        except Exception as e:  # noqa: BLE001 - the bytes are captured regardless
            print(f"  (session step {label!r} raised {type(e).__name__}: {e}; "
                  f"its captured bytes are still validated)")

    c.connect()                                        # -> hello event
    _try("ping", lambda: c.ping())
    _try("dispatch_stats", lambda: c.call("dispatch_stats"))
    _try("get_health", lambda: c.call("get_health"))
    _try("open_project", lambda: c.call("open_project", {"path": str(PROJECT)}, timeout=300))
    _try("compile_and_load",
         lambda: c.compile_and_load(str(PROJECT / "inspect.cpp"), timeout=300))
    # A frame -> run_result + run_finished events.
    _try("run", lambda: c.call("run", {"meta": {"probe": "conformance"}}))
    # State transitions -> health_changed events (start enters running; stop leaves it).
    _try("start", lambda: c.call("start"))
    time.sleep(0.4)
    _try("stop", lambda: c.call("stop"))
    time.sleep(0.4)
    # commit_group needs a real target: the script declares instance "scaler".
    _try("set_instance_def", lambda: c.set_instance_def("scaler", {"factor": 7}))
    _try("commit_group", lambda: c.call("commit_group", {"instances": ["scaler"]}))
    _try("load_project", lambda: c.call("load_project", {"path": str(PROJECT / "project.json")}))
    _try("metrics", lambda: c.call("metrics"))
    _try("list_instances", lambda: c.list_instances())
    # Give any trailing async events (health_changed, status) time to land.
    time.sleep(0.6)


# --- classify + validate ---------------------------------------------------- #
def classify(msg: dict, id_to_cmd: dict[int, str]) -> str:
    t = msg.get("type")
    if t == "event":
        return f"event:{msg.get('name')}"
    if t == "rsp":
        return f"rsp:{id_to_cmd.get(msg.get('id'), '<unknown-id>')}"
    if isinstance(t, str):
        return t  # 'log', 'instances', ...
    return "<no-type>"


def main() -> int:
    schemas, registry = build_registry()
    if "rsp.schema.json" not in schemas:
        print("contract-live: FAIL - generic rsp.schema.json missing")
        return 1
    ev_map = event_schema_by_name(schemas)          # 'hello' -> 'hello.schema.json'
    wire_map = load(MAP_PATH)
    rsp_map = wire_map["rsp"]                        # 'metrics' -> {schema, validate}
    allow = load(ALLOWLIST_PATH)["allow"]

    # Sanity: every schema the maps reference must exist.
    for cmd, spec in rsp_map.items():
        if spec["schema"] not in schemas:
            print(f"contract-live: FAIL - live-wire-map rsp:{cmd} -> "
                  f"{spec['schema']} not found in contract/schemas/")
            return 1

    # Build the specific-schema routing keyed like classify()'s output.
    #   event:<name> -> (schema, 'envelope')
    #   rsp:<cmd>    -> (schema, 'envelope'|'data')
    specific: dict[str, tuple[str, str]] = {}
    for evname, sfile in ev_map.items():
        specific[f"event:{evname}"] = (sfile, "envelope")
    for cmd, spec in rsp_map.items():
        specific[f"rsp:{cmd}"] = (spec["schema"], spec.get("validate", "envelope"))

    def validate(instance, schema_name: str) -> str | None:
        v = Draft202012Validator(schemas[schema_name], registry=registry)
        err = best_match(v.iter_errors(instance))
        if err is None:
            return None
        loc = "/".join(str(x) for x in err.absolute_path) or "<root>"
        return f"at '{loc}': {err.message}"

    # --- drive the live backend ---
    print(f"contract-live: spawning {BACKEND.relative_to(ROOT).as_posix()}")
    with spawn_backend() as url:
        c = CapturingClient(url, timeout=60)
        try:
            drive_session(c)
        finally:
            with contextlib.suppress(Exception):
                c.close()
        captured = list(c.captured)
        id_to_cmd = dict(c.id_to_cmd)

    print(f"contract-live: captured {len(captured)} inbound message(s)")

    failures: list[str] = []
    validated: dict[str, int] = {}     # key -> count validated against a specific schema
    allowed: dict[str, int] = {}       # key -> count matched to the allowlist
    envelope_checked = 0               # rsps checked against generic rsp.schema.json

    for msg in captured:
        key = classify(msg, id_to_cmd)

        # Every rsp is envelope-checked against the generic schema (the only place
        # rsp.schema.json is exercised against real bytes).
        if msg.get("type") == "rsp":
            err = validate(msg, "rsp.schema.json")
            if err is not None:
                failures.append(f"[{key}] violates generic rsp.schema.json {err}\n"
                                f"        message: {json.dumps(msg)[:400]}")
            else:
                envelope_checked += 1

        if key in specific:
            schema_name, target = specific[key]
            instance = msg if target == "envelope" else msg.get("data")
            if target == "data" and not isinstance(instance, dict):
                failures.append(f"[{key}] expected an object `data` payload for "
                                f"{schema_name}, got {type(instance).__name__}\n"
                                f"        message: {json.dumps(msg)[:400]}")
                continue
            err = validate(instance, schema_name)
            if err is not None:
                failures.append(f"[{key}] LIVE bytes violate {schema_name} {err}\n"
                                f"        message: {json.dumps(msg)[:600]}")
            else:
                validated[key] = validated.get(key, 0) + 1
        elif key in allow:
            allowed[key] = allowed.get(key, 0) + 1
        else:
            failures.append(
                f"[{key}] NEW unschema'd wire message — no schema describes it and it "
                f"is not in contract/live-allowlist.json. Add a schema (+ fixture + "
                f"map entry) or, if it is genuinely shapeless, allowlist it with a "
                f"reason.\n        message: {json.dumps(msg)[:400]}")

    # --- coverage: the session must actually have exercised the schema'd wire ---
    REQUIRED_KEYS = [
        "event:hello", "event:run_result", "event:run_finished", "event:health_changed",
        "rsp:dispatch_stats", "rsp:metrics", "rsp:get_health",
        "rsp:commit_group", "rsp:load_project",
    ]
    for key in REQUIRED_KEYS:
        if validated.get(key, 0) == 0:
            failures.append(
                f"[coverage] expected to capture+validate at least one {key}, but the "
                f"session produced none — the wire leg did not actually exercise it "
                f"(a green with a hole is worse than a red).")

    # --- report ---
    print("contract-live: validated against specific schemas:")
    for k in sorted(validated):
        print(f"    {validated[k]:3d} x {k} -> {specific[k][0]}")
    print(f"contract-live: {envelope_checked} rsp(s) envelope-checked vs rsp.schema.json")
    print("contract-live: allowlisted (no per-message schema):")
    for k in sorted(allowed):
        print(f"    {allowed[k]:3d} x {k}")

    if failures:
        print(f"contract-live: FAIL ({len(failures)} problem(s)):")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("contract-live: OK - live wire conforms to the committed schemas + allowlist")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
