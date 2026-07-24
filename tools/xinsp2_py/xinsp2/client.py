"""Synchronous WS client for xInsp2.

One client = one connection. Not thread-safe; intended for scripts driven
by an AI agent (or a human at a REPL). Spec: docs/reference/ws-protocol.md.
"""
from __future__ import annotations

import hashlib
import hmac
import json
import threading
import time
from dataclasses import dataclass, field
from queue import Queue, Empty
from typing import Any, Callable

import websocket  # websocket-client


# ---- run-outcome wire contract -----------------------------------------------
#
# The backend emits a per-run `run_result` event carrying the outcome of one
# inspect. Its schema is versioned via the `schema` field. These constants let a
# consumer distinguish a genuine verdict from a system-generated outcome without
# hard-coding magic numbers at every call site.
RUN_RESULT_SCHEMA = "xi.run-outcome/1"

# Reserved system result codes (backend `XI_SYS_*`). These ride on the numeric
# `code` channel of a `run_result`. BREAKING (already on master): a caught
# inspect error now emits XI_SYS_CRASHED (was 0), and a run that set no result
# emits XI_SYS_NO_VERDICT (was 0). An explicit `xi::result(0)` is still 0 ("na").
XI_SYS_CRASHED = -999002      # caught inspect error (throw/crash); run did not verdict
XI_SYS_NO_VERDICT = -999005   # inspect ran but never called xi::result()
XI_SYS_DROPPED = -999001      # run dropped before inspect (e.g. backpressure)

# Derived outcome `class` strings (JSON key `class`; exposed on the Python side
# as `verdict_class`, since `class` is a reserved keyword).
CLASS_OK = "ok"
CLASS_NG = "ng"
CLASS_NA = "na"
CLASS_NO_VERDICT = "no_verdict"
CLASS_CRASHED = "crashed"
CLASS_DROPPED = "dropped"

# Map a system code to its class string (mirror of the backend's
# `outcome_class_for_code`, for the reserved codes a consumer cares about).
_SYS_CODE_CLASS = {
    XI_SYS_DROPPED: CLASS_DROPPED,
    XI_SYS_CRASHED: CLASS_CRASHED,
    XI_SYS_NO_VERDICT: CLASS_NO_VERDICT,
}


def outcome_class_for_code(code: int) -> str:
    """Best-effort class derivation from a numeric result code.

    Mirrors the backend so a consumer that only has the `code` (e.g. an older
    event missing the additive `class` field) can still classify it. Prefer the
    event's own `class` field when present — read `RunOutcome.verdict_class`.
    """
    if code in _SYS_CODE_CLASS:
        return _SYS_CODE_CLASS[code]
    if code > 0:
        return CLASS_OK
    if code == 0:
        return CLASS_NA
    # Valid ng band is <0 and above the reserved system band (< -990000).
    if code > -990000:
        return CLASS_NG
    return CLASS_NA


class ProtocolError(RuntimeError):
    """Raised when a backend cmd returns ok=false.

    `error` is the short error string from the rsp. `data` carries the
    structured `rsp.data` payload when the backend attaches one (e.g.
    compile-failure paths ship a `diagnostics` array there). Both
    default to None so callers that don't care can ignore them.
    """
    def __init__(self, message: str, *, error: str | None = None, data: Any = None):
        super().__init__(message)
        self.error = error
        self.data = data


class CmdTimeoutError(TimeoutError):
    """Raised by `Client.call(...)` when the rsp doesn't arrive within
    the requested timeout. The cmd is NOT cancelled on the backend —
    the SDK simply stopped waiting. Subsequent state queries may still
    observe the cmd's effects landing later. If the rsp eventually
    arrives it is dropped silently.

    Subclass of the builtin `TimeoutError` so callers using
    `except TimeoutError` (the natural idiom) catch it.
    """
    def __init__(self, name: str, timeout: float):
        super().__init__(
            f"cmd {name!r} timed out after {timeout}s waiting for rsp; "
            f"the backend is still running it (no server-side cancel — "
            f"avoid retrying without checking state first)."
        )
        self.cmd_name = name
        self.timeout = timeout


class ConnectionLostError(ConnectionError):
    """Raised when the SDK detects the WS connection has dropped, e.g.
    a `call()` is issued after the read loop exited. Callers looping on
    the connection should treat this as a stop signal."""
    pass


class AuthError(ConnectionError):
    """Raised when the backend rejects the WS handshake auth (HTTP 401).

    The backend (started with `--auth=<secret>` or `XINSP2_AUTH`) checks a
    shared secret entirely in the WebSocket upgrade handshake — before any
    `cmd`/`rsp` traffic — so a bad or missing credential is a clean, immediate
    401 close, NOT a hang: `connect()` translates the underlying
    `websocket.WebSocketBadStatusException` into this typed error so callers
    get a fast, catchable signal instead of a socket that dangles.

    Subclass of the builtin `ConnectionError` (alongside `ConnectionLostError`
    and the `ConnectionRefusedError` raised for a missing listener) so callers
    using `except ConnectionError` catch every connect-time failure uniformly.
    `.mode` records which flow was attempted ("bearer" | "hmac").
    """
    def __init__(self, message: str, *, mode: str | None = None, status_code: int | None = None):
        super().__init__(message)
        self.mode = mode
        self.status_code = status_code


class UnknownCommandError(ProtocolError):
    """Plugin's `exchange()` returned the canonical
    `{"error": "unknown_command", "command": "<name>"}` shape (see
    `docs/reference/c-abi.md` § "Unknown commands"). The SDK's
    `Client.exchange_instance()` raises this when it sees that
    payload, so callers don't have to remember to inspect
    `rsp.get("error")` themselves.
    """
    def __init__(self, plugin_name: str, command: str, raw: dict):
        super().__init__(
            f"plugin instance {plugin_name!r} did not recognise command "
            f"{command!r} (canonical unknown_command shape)",
            error="unknown_command",
            data=raw,
        )
        self.plugin_name = plugin_name
        self.command = command


class PartialStatusError(ProtocolError):
    """A lifecycle cmd reported a non-success `status`.

    BREAKING (already on master): both `load_project` and `commit_group` return
    `ok:false` on a non-success status (`load_project`: "partial"/"rejected",
    was `ok:true` with warning arrays; `commit_group`: "partial", after which the
    backend no longer auto-resumes dispatch). A consumer MUST NOT treat either as
    success. The backend's `ok:false` routes both through `ProtocolError`; the
    helpers re-raise it as this subclass, which surfaces the `status` (and any
    `data`) so callers can branch on it. (A defensive `ok:true` + non-success
    `status` path is also honoured, in case an older/other backend reports the
    status without flipping `ok`.)
    """
    def __init__(self, cmd: str, status: str, *, error: str | None = None, data: Any = None):
        super().__init__(
            f"cmd {cmd!r} returned status {status!r} (not a clean success)",
            error=error or status,
            data=data,
        )
        self.cmd = cmd
        self.status = status


def _enrich_compile_error(orig: ProtocolError, what: str, target: str) -> ProtocolError:
    """Re-raise a compile-failure ProtocolError with the diagnostics
    folded into the message. Bare `compile failed` text is useless on
    its own; the build log lives on `rsp.data["diagnostics"]` but
    nothing told you to look there until now."""
    diagnostics = []
    data = orig.data
    if isinstance(data, dict):
        diagnostics = data.get("diagnostics") or []
    lines = [f"{what} {target!r} failed: {orig.error or 'compile failed'}"]
    for d in diagnostics[:20]:
        if not isinstance(d, dict):
            continue
        loc = d.get("file", "")
        if d.get("line"):
            loc += f":{d['line']}"
            if d.get("col"):
                loc += f":{d['col']}"
        sev = d.get("severity", "error")
        code = d.get("code", "")
        msg = d.get("message", "")
        lines.append(f"  {loc} {sev} {code}: {msg}".rstrip())
    if len(diagnostics) > 20:
        lines.append(f"  ... and {len(diagnostics) - 20} more")
    if not diagnostics:
        lines.append("  (no structured diagnostics on rsp.data; check "
                     r"%TEMP%\xinsp2\script_build\*.log for the raw cl.exe output)")
    return ProtocolError("\n".join(lines), error=orig.error, data=orig.data)


@dataclass
class RunOutcome:
    """Parsed `run_result` event — the per-run verdict/outcome contract.

    Carries the existing fields (`code`, `msg`, `run_id`, `ms`, `source`,
    `group`) plus the additive identity/outcome fields introduced on master
    (schema `xi.run-outcome/1`). Every additive field is optional: missing keys
    parse to None/default so an outcome from an older backend still loads.

    Note the JSON `class` key is exposed as `verdict_class` (`class` is a Python
    keyword). Use the `CLASS_*` constants to compare, and `is_crashed` /
    `is_no_verdict` / `is_ng` / `is_ok` for the common checks.
    """
    code: int = 0
    msg: str = ""
    run_id: int | None = None
    ms: int | None = None
    source: str | None = None
    group: str | None = None
    # --- additive identity / outcome fields (schema xi.run-outcome/1) ---
    trigger_id: str | None = None
    boot_id: str | None = None
    station_id: str | None = None
    inspection_id: str | None = None
    schema: str | None = None
    verdict_class: str | None = None    # JSON key `class`
    reason_code: str | None = None
    script_generation: int | None = None
    raw: dict = field(default_factory=dict)

    @classmethod
    def from_event(cls, ev: dict) -> "RunOutcome":
        """Build from a `run_result` event dict (`{"name","data",...}`) or from a
        bare data dict. Backward-tolerant: unknown/missing keys are ignored."""
        data = ev.get("data", ev) if isinstance(ev, dict) else {}
        if not isinstance(data, dict):
            data = {}
        return cls(
            code=int(data.get("code", 0)),
            msg=data.get("msg", "") or "",
            run_id=data.get("run_id"),
            ms=data.get("ms"),
            source=data.get("source"),
            group=data.get("group"),
            trigger_id=data.get("trigger_id"),
            boot_id=data.get("boot_id"),
            station_id=data.get("station_id"),
            inspection_id=data.get("inspection_id"),
            schema=data.get("schema"),
            # `class` is a Python keyword — map it to `verdict_class`.
            verdict_class=data.get("class"),
            reason_code=data.get("reason_code"),
            script_generation=data.get("script_generation"),
            raw=data,
        )

    @property
    def cls(self) -> str:
        """The outcome class. Prefers the event's own `class` field; falls back
        to deriving it from `code` for older events that omit it."""
        return self.verdict_class or outcome_class_for_code(self.code)

    @property
    def is_ok(self) -> bool:
        return self.cls == CLASS_OK

    @property
    def is_ng(self) -> bool:
        return self.cls == CLASS_NG

    @property
    def is_na(self) -> bool:
        return self.cls == CLASS_NA

    @property
    def is_crashed(self) -> bool:
        return self.cls == CLASS_CRASHED or self.code == XI_SYS_CRASHED

    @property
    def is_no_verdict(self) -> bool:
        return self.cls == CLASS_NO_VERDICT or self.code == XI_SYS_NO_VERDICT

    @property
    def is_dropped(self) -> bool:
        return self.cls == CLASS_DROPPED or self.code == XI_SYS_DROPPED


@dataclass
class RunFinished:
    """Parsed `run_finished` event — inspect COMPUTE timing for a run.

    Both `ms` (legacy integer-ms) and the additive `inspect_compute_us`
    (microsecond precision) are inspect COMPUTE time only — they exclude queue
    wait, emit-gate wait, sink flush, JPEG encode and WS send, and are NOT
    cycle/decision latency. Prefer `inspect_compute_us` for precision.
    """
    run_id: int | None = None
    ms: int | None = None
    inspect_compute_us: int | None = None
    raw: dict = field(default_factory=dict)

    @classmethod
    def from_event(cls, ev: dict) -> "RunFinished":
        data = ev.get("data", ev) if isinstance(ev, dict) else {}
        if not isinstance(data, dict):
            data = {}
        return cls(
            run_id=data.get("run_id"),
            ms=data.get("ms"),
            inspect_compute_us=data.get("inspect_compute_us"),
            raw=data,
        )


@dataclass
class RunResult:
    """Outcome of one `inspect()` run.

    Generic and content-agnostic: it carries the run identity, timing,
    the raw `rsp.data` payload the backend returned (which includes the
    `verdict` when the script set one), and any events that landed during
    the run. This client does NOT collect or decode VARs or image
    previews — that domain model now lives behind the owning plugin's
    own webUI. Read `data` / `verdict` for the run outcome.

    The per-run `run_result` / `run_finished` events land in `events`; read
    them typed via the `outcome` and `finished` properties.
    """
    run_id: int
    ms: int
    data: dict = field(default_factory=dict)   # raw rsp.data for the run
    events: list[dict] = field(default_factory=list)

    @property
    def verdict(self):
        """The script's verdict if it set one, else None."""
        return self.data.get("verdict")

    def _event(self, name: str) -> dict | None:
        """Last event with the given name that landed during this run."""
        found = None
        for ev in self.events:
            if isinstance(ev, dict) and ev.get("name") == name:
                found = ev
        return found

    @property
    def outcome(self) -> RunOutcome | None:
        """Parsed `run_result` event for this run, or None if none landed.

        Exposes the additive identity/outcome fields (trigger_id, boot_id,
        inspection_id, schema, verdict_class, reason_code, script_generation)
        and lets a consumer distinguish crash/no-verdict/na via the numeric
        `code` (XI_SYS_*) and the derived `class`.
        """
        ev = self._event("run_result")
        return RunOutcome.from_event(ev) if ev is not None else None

    @property
    def finished(self) -> RunFinished | None:
        """Parsed `run_finished` event for this run (inspect compute timing),
        or None if none landed. Read `.inspect_compute_us` for µs precision."""
        ev = self._event("run_finished")
        return RunFinished.from_event(ev) if ev is not None else None


class Client:
    def __init__(
        self,
        url: str = "ws://127.0.0.1:7823/",
        timeout: float = 30.0,
        *,
        token: str | None = None,
        auth_mode: str = "bearer",
    ):
        """Create a client (one client = one connection).

        Auth
        ----
        The backend (started with `--auth=<secret>` or the `XINSP2_AUTH` env
        var) checks a shared secret in the WebSocket upgrade handshake. Pass
        `token=` to authenticate; leave it None for an unauthenticated backend.

        The backend offers TWO handshake shapes, and which one it accepts is
        fixed by how the *server* was started — it is NOT negotiated, and the
        client cannot auto-detect it (both refusals are an identical bare 401).
        Select the matching flow with `auth_mode`:

        - ``"bearer"`` (default): server started `--auth=<secret>`. The client
          sends `Authorization: Bearer <token>`; `token` is the secret itself.
          Anyone who can sniff the handshake can replay it — loopback / trusted
          LAN / behind a TLS proxy only.
        - ``"hmac"``: server started `--auth=hmac:<key>`. The client sends
          `X-Xi-Timestamp: <unix_seconds>` and
          `Authorization: Bearer <hex(hmac_sha256(key, <unix_seconds>))>`;
          `token` is the HMAC KEY, which never crosses the wire. The server
          accepts it only within ±60 s of its clock, so a captured handshake
          can be replayed for at most 60 s (post-handshake frames are still
          plaintext — use a TLS proxy for hostile networks). Despite the
          "challenge" framing elsewhere, there is no server-issued nonce: the
          client timestamps and signs; the exchange is single-shot.

        Do NOT try `"bearer"` against an `hmac:` server as a fallback — that
        would put the raw HMAC key on the wire as a failed bearer attempt,
        defeating the entire point of hmac mode. Pick the mode deliberately.
        """
        if auth_mode not in ("bearer", "hmac"):
            raise ValueError(
                f"auth_mode must be 'bearer' or 'hmac', got {auth_mode!r}"
            )
        self.url = url
        self.timeout = timeout
        self._token = token
        self._auth_mode = auth_mode
        self._ws: websocket.WebSocket | None = None
        self._next_id = 1
        self._rsp_waiters: dict[int, Queue] = {}
        self._inbox_events: Queue = Queue()
        self._inbox_logs: Queue = Queue()
        self._inbox_binary: Queue = Queue()        # raw binary WS frames (opaque bytes)
        self._on_binary: Callable[[bytes], None] | None = None
        self._reader: threading.Thread | None = None
        self._closed = False
        self._read_loop_dead = False
        self._on_log: Callable[[dict], None] | None = None

    # ---- lifecycle ----------------------------------------------------

    def _auth_headers(self) -> dict[str, str]:
        """Build the handshake auth headers for the configured mode.

        Returns {} when no token is set (unauthenticated backend). Mirrors the
        server verification in `backend/include/xi/xi_ws_server.hpp` — for hmac
        mode the message signed is the exact decimal timestamp STRING that is
        also sent in `X-Xi-Timestamp`, and the digest is lowercase hex.
        """
        if not self._token:
            return {}
        if self._auth_mode == "hmac":
            ts = str(int(time.time()))
            mac = hmac.new(
                self._token.encode("utf-8"), ts.encode("utf-8"), hashlib.sha256
            ).hexdigest()
            return {"Authorization": f"Bearer {mac}", "X-Xi-Timestamp": ts}
        return {"Authorization": f"Bearer {self._token}"}

    def connect(self) -> dict:
        try:
            self._ws = websocket.create_connection(
                self.url, timeout=self.timeout, header=self._auth_headers()
            )
        except websocket.WebSocketBadStatusException as e:
            # The backend answers a bad/absent credential with an immediate
            # HTTP 401 during the upgrade — a clean refusal, not a hang.
            # Translate the low-level status exception into the SDK's typed
            # AuthError so callers get an actionable, catchable signal.
            status = getattr(e, "status_code", None)
            if status == 401:
                hint = (
                    "check the token" if self._token else
                    "this backend requires --auth; pass token=..."
                )
                raise AuthError(
                    f"backend refused the handshake auth at {self.url} "
                    f"(HTTP 401, auth_mode={self._auth_mode!r}); {hint}.",
                    mode=self._auth_mode,
                    status_code=status,
                ) from e
            raise
        except (ConnectionRefusedError, OSError) as e:
            # Translate the kernel-level "no listener" error into a
            # plain-English hint about how to start the backend. The
            # bare ConnectionRefusedError tells the agent / human
            # nothing about what's supposed to be on this port.
            raise ConnectionRefusedError(
                f"can't reach xInsp2 backend at {self.url} ({e}). "
                f"If you're running outside VS Code, start it yourself: "
                f"`backend/build/Release/xinsp-backend.exe &` "
                f"(see tools/xinsp2_py/README.md). Inside VS Code the "
                f"extension auto-starts one when a project opens."
            ) from e
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()
        # `hello` arrives as an event right after connect
        ev = self._inbox_events.get(timeout=self.timeout)
        if ev.get("name") != "hello":
            raise ProtocolError(f"expected hello event, got {ev}")
        return ev.get("data", {})

    def close(self):
        self._closed = True
        if self._ws:
            try:
                self._ws.close()
            except Exception:
                pass

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, *_):
        self.close()

    # ---- low-level send/recv -----------------------------------------

    def call(self, name: str, args: dict | None = None, timeout: float | None = None) -> Any:
        """Send a `cmd` and block for its `rsp`.

        Parameters
        ----------
        name : str          cmd name (e.g. "ping", "open_project")
        args : dict | None  cmd args; defaults to {}
        timeout : float|None  override the Client's default timeout (seconds)

        Returns the parsed `rsp.data` on success.

        Raises
        ------
        CmdTimeoutError    no rsp within timeout. The backend is NOT
                           told to cancel — it keeps running the cmd
                           and any side effects still land. Callers
                           that retry MUST verify state hasn't already
                           been mutated by the prior call.
        ProtocolError      rsp.ok=false; carries `.error` and `.data`.
        ConnectionLostError  the WS read loop has exited (peer closed
                             the socket, or kill -9 on the backend).
        """
        if self._read_loop_dead:
            raise ConnectionLostError(
                f"cannot send cmd {name!r}: WS connection is closed"
            )
        cid = self._next_id
        self._next_id += 1
        q: Queue = Queue()
        self._rsp_waiters[cid] = q
        eff_timeout = timeout if timeout is not None else self.timeout
        self._ws.send(json.dumps({"type": "cmd", "id": cid, "name": name, "args": args or {}}))
        try:
            try:
                rsp = q.get(timeout=eff_timeout)
            except Empty:
                # The bare queue.Empty was confusing — callers wrote
                # `except TimeoutError` (the natural idiom) and never
                # caught the real exception. Promote to a typed,
                # subclassable error that preserves the catch-all
                # `except TimeoutError` semantics.
                if self._read_loop_dead:
                    raise ConnectionLostError(
                        f"cmd {name!r}: WS closed while waiting for rsp"
                    ) from None
                raise CmdTimeoutError(name, eff_timeout) from None
        finally:
            self._rsp_waiters.pop(cid, None)
        if rsp.get("_synthetic"):
            # Read loop signalled disconnect via the rsp queue.
            raise ConnectionLostError(
                f"cmd {name!r}: WS closed before rsp arrived"
            )
        if not rsp.get("ok"):
            err = rsp.get("error")
            raise ProtocolError(
                f"cmd {name!r} failed: {err}",
                error=err,
                data=rsp.get("data"),
            )
        return rsp.get("data")

    # ---- high-level ---------------------------------------------------

    def ping(self) -> dict:
        return self.call("ping")

    def version(self) -> dict:
        return self.call("version")

    def compile_and_load(self, path: str, timeout: float = 180) -> dict:
        """Compile an inspection script and hot-load the resulting DLL.

        On success returns the rsp data which the wire actually carries:
        `{"dll": "<absolute_path>", "diagnostics": [...],
          "resumed_continuous"?: true}`. The build log is delivered as a
        SEPARATE `log` text message during compile, not inside this
        return value. (Earlier versions of this docstring documented
        a `{build_log, instances, params}` shape that the backend never
        produced — fixed in the audit pass.)

        On compile failure raises `ProtocolError` whose message
        includes a formatted summary of cl.exe diagnostics (first 20
        lines + count of any extras). The full structured array is on
        `e.data["diagnostics"]`; e.g.::

            try:
                c.compile_and_load(path)
            except ProtocolError as e:
                for d in (e.data or {}).get("diagnostics", []):
                    fix(d["file"], d["line"], d["message"])

        Cold compiles dominate the wall-clock window (cl.exe ~3-4 s
        per source file). The default 180 s covers a typical project;
        bump for slow / many-source rebuilds via `timeout=`.
        """
        try:
            return self.call("compile_and_load", {"path": path}, timeout=timeout)
        except ProtocolError as e:
            raise _enrich_compile_error(e, "compile_and_load", path) from None

    def load_project(self, path: str) -> dict:
        """Read a `project.json` from disk into the backend's project
        state — but does NOT compile project plugins or instantiate
        from `instances/`. Use `open_project()` for the full flow
        (read + compile plugins + instantiate instances + run instance
        ctors). 90% of the time you want `open_project()`.

        Status semantics (already on master): the response carries a
        `status` of "ok" | "partial" | "rejected". A "partial" (recipe only
        partly restored) or "rejected" load now comes back as `ok:false`, so
        `Client.call` raises `ProtocolError` — this helper re-raises it as a
        `PartialStatusError` carrying `.status` so a consumer does NOT mistake a
        partial/rejected load for a clean success. On success returns the rsp
        data (which still includes `status:"ok"` plus any warning arrays).
        """
        try:
            data = self.call("load_project", {"path": path}, timeout=180)
        except ProtocolError as e:
            status = None
            if isinstance(e.data, dict):
                status = e.data.get("status")
            raise PartialStatusError(
                "load_project", status or "rejected", error=e.error, data=e.data
            ) from e
        # Defensive: if a backend ever returns ok:true with a non-ok status,
        # still refuse to treat it as success.
        if isinstance(data, dict) and data.get("status") not in (None, "ok"):
            raise PartialStatusError("load_project", data.get("status"), data=data)
        return data

    def commit_group(self, group: str | None = None, **kwargs) -> dict:
        """Commit a staged config group.

        Status semantics (already on master): the response carries a `status`
        of "committed" | "partial", and a PARTIAL commit comes back as
        `ok:false` (the backend no longer auto-resumes dispatch — it latches a
        config fault). So `Client.call` raises `ProtocolError`, which this helper
        re-raises as `PartialStatusError` (carrying `.status` and the rsp `data`)
        so the caller can decide how to recover (e.g. re-stage and re-commit,
        then explicitly resume). Returns the rsp `data` on a clean "committed".
        """
        args = dict(kwargs)
        if group is not None:
            args["group"] = group
        try:
            data = self.call("commit_group", args)
        except ProtocolError as e:
            status = e.data.get("status") if isinstance(e.data, dict) else None
            raise PartialStatusError(
                "commit_group", status or "partial", error=e.error, data=e.data
            ) from e
        # Defensive: an ok:true rsp that still reports a non-"committed" status
        # (older/other backend that didn't flip ok) is not a clean success.
        if isinstance(data, dict):
            status = data.get("status")
            if status is not None and status != "committed":
                raise PartialStatusError("commit_group", status, data=data)
        return data

    def metrics(self) -> dict:
        """Fetch the backend metrics snapshot (`cmd:metrics`).

        Wire note (already on master): the inspect-timing key was renamed from
        `latency_ms` to `inspect_compute_ms` (with subkeys
        `.count/.sum_ms/.mean_ms/.buckets`) because the value is inspect COMPUTE
        time, not decision latency. This helper returns the raw snapshot; use
        `metrics_inspect_compute()` for a rename-tolerant accessor.
        """
        return self.call("metrics")

    def metrics_inspect_compute(self, snapshot: dict | None = None) -> dict | None:
        """Return the inspect-compute timing block from a metrics snapshot,
        tolerating the `latency_ms` -> `inspect_compute_ms` rename.

        Prefers the new `inspect_compute_ms` key; falls back to the legacy
        `latency_ms` so a snapshot from an older backend still resolves.
        Returns None if neither key is present.
        """
        snap = snapshot if snapshot is not None else self.metrics()
        if not isinstance(snap, dict):
            return None
        return snap.get("inspect_compute_ms", snap.get("latency_ms"))

    def open_project(self, path: str, timeout: float = 180) -> dict:
        """Open a project folder end-to-end: scan `instances/`, compile
        every project-local plugin, instantiate each instance, restore
        each instance's saved `def`. The reply includes the project's
        plugin list (with manifest blocks) and instance list. Cold
        opens with N project plugins compile each plugin under cl.exe;
        a 180 s timeout is the SDK default to cover that — bump it via
        `timeout=` if you have many large plugins.

        The backend accepts both `path` and `folder` as the args key
        for `cmd:open_project`; this helper sends `path`. Older driver
        scripts may use `c.call("open_project", {"folder": ...})` —
        same effect, prefer the helper for new code.

        Re-opening a project: tearing down the old project's plugins
        unloads their DLLs, which means any persisted `xi::state()`
        from a script linked against those plugins won't survive. If
        you reopen the same project mid-session, expect script state
        to reset on the next `compile_and_load`. Re-prime by replaying
        prior frames if the script's logic depends on accumulated
        state. (The agent feedback loop hit this on the trend_monitor
        case.)
        """
        return self.call("open_project", {"path": path}, timeout=timeout)

    def recompile_project_plugin(self, plugin: str) -> dict:
        """Hot-rebuild one project-local plugin. Linchpin for live tuning.

        Returns `{plugin, diagnostics, reattached}` on success. On compile
        failure raises `ProtocolError` whose message summarises the
        cl.exe diagnostics; the full structured array is on
        `e.data["diagnostics"]`. The previous DLL stays loaded so the
        inspection keeps working.
        """
        try:
            return self.call("recompile_project_plugin", {"plugin": plugin}, timeout=120)
        except ProtocolError as e:
            raise _enrich_compile_error(e, "recompile_project_plugin", plugin) from None

    def set_param(self, name: str, value) -> None:
        self.call("set_param", {"name": name, "value": value})

    # ---- discovery / introspection -----------------------------------

    def list_plugins(self) -> list[dict]:
        """Every registered plugin (global + project-local).

        Each entry: `{name, description, folder, has_ui, loaded, origin,
        cert, manifest?}`. `manifest` is the optional structured
        `params / inputs / outputs / exchange` block from the plugin's
        `plugin.json` (see docs/reference/c-abi.md).
        """
        return self.call("list_plugins")

    def list_instances(self) -> dict:
        """Trigger an `instances` message and return its payload.

        Unlike most cmds, the backend replies via the
        `instances` text-message channel (not in the rsp data). We
        block briefly to grab it; if you need fully async access,
        use raw `call("list_instances")` and consume the inbox.
        """
        # The cmd's rsp itself just acks; the real payload comes as
        # an `instances` message dispatched as an event by our reader.
        self._drain(self._inbox_events)
        self.call("list_instances")
        # The reader pushes `instances` messages into _inbox_events
        # under the synthesised name "instances".
        ev = self._inbox_events.get(timeout=self.timeout)
        if ev.get("name") != "instances":
            # Older clients may receive direct rsp data — allow both.
            return ev if isinstance(ev, dict) else {}
        return ev.get("data", {})

    def set_instance_def(self, name: str, def_obj: dict) -> None:
        self.call("set_instance_def", {"name": name, "def": def_obj})

    def exchange_instance(self, name: str, cmd_obj: dict) -> Any:
        """Invoke a plugin instance's `exchange()` method directly.

        Returns whatever the plugin chose to send back, JSON-parsed.
        Convention: most plugins return their post-mutation `get_def()`,
        so you typically get back a dict in the same shape as the
        instance's stored config plus any read-only telemetry the
        plugin chose to expose. Example:

            >>> c.exchange_instance("det", {"command": "set_hue_range",
            ...                              "lo": 110, "hi": 130})
            {'hue_lo': 110, 'hi_hi': 130, 'min_area': 300,
             'frames_processed': 8, 'last_count': 4}

        Unknown commands: per `docs/reference/c-abi.md` § "Unknown
        commands", a well-behaved plugin returns the canonical
        `{"error": "unknown_command", "command": "<name>"}` for
        misspelt commands. The SDK detects that shape and raises
        `UnknownCommandError` (subclass of `ProtocolError`), so a
        driver typo doesn't silently get a no-op `get_def()`. To opt
        out of the auto-raise and inspect the raw shape yourself, use
        `c.call("exchange_instance", ...)` directly.

        See `docs/reference/c-abi.md` ("Return shape convention").
        """
        rsp = self.call("exchange_instance", {"name": name, "cmd": cmd_obj})
        # Surface the canonical unknown_command shape as a typed error.
        # Plugins are free to return any other error shape — those
        # remain in the dict for the caller to inspect.
        if isinstance(rsp, dict) and rsp.get("error") == "unknown_command":
            cmd_name = cmd_obj.get("command", "") if isinstance(cmd_obj, dict) else ""
            raise UnknownCommandError(name, cmd_name, rsp)
        return rsp

    # ---- runtime control ---------------------------------------------

    def shutdown(self) -> None:
        """Tell the backend to close the socket and exit. After this
        the client should not reuse the connection."""
        try:
            self.call("shutdown", timeout=5.0)
        except Exception:
            # Backend often closes before sending a clean rsp.
            pass

    # ---- observability -----------------------------------------------

    def image_pool_stats(self) -> dict:
        """Per-owner ImagePool footprint. Use to spot leaks: a
        plugin/script whose handle count keeps climbing across runs is
        holding pool entries it should be releasing.
        """
        return self.call("image_pool_stats")

    def status(self) -> dict:
        """Snapshot of every component's latest sticky status string.

        Returns a dict keyed by source ("@script" for the inspection script,
        else the instance name) -> {"text", "ts_ms", "seq"}. This is the
        delivery guarantee: call it on every (re)connect to re-sync the latest
        status, since the backend retains it last-write-wins. The `status`
        event is a best-effort live accelerator between snapshots.
        """
        return self.call("status")

    def recent_errors(self, since_ms: int | None = None) -> list[dict]:
        """Errors captured by the cross-channel ring (rsp / log / event).

        After calling a cmd, poll this with the previous max ts_ms to
        catch any side-channel errors that landed in the same beat.
        """
        args = {}
        if since_ms is not None:
            args["since_ms"] = since_ms
        return self.call("recent_errors", args)

    def run(self, frame_path: str | None = None, timeout: float | None = None) -> RunResult:
        """Run one inspect() and return its outcome.

        Blocks on the `cmd:run` reply, which carries the run identity,
        timing, and verdict. Events that landed during the run are then
        scooped non-blocking into `RunResult.events`. (The backend also
        emits a `run_finished` event per run; this method keys completion
        off the rsp and does not wait on it.)

        This is a GENERIC driver: it does not collect or decode VARs or
        image previews. Read `RunResult.verdict` / `RunResult.data` for
        the outcome; consume binary/preview streams via the owning
        plugin's own UI, or via `exchange_instance` for plugin telemetry.

        `_inbox_events` is deliberately left intact between runs — earlier
        calls' events (e.g. `state_dropped` after a `compile_and_load`)
        stay queued so the caller can read them out.
        """
        args = {"frame_path": frame_path} if frame_path else {}
        data = self.call("run", args, timeout=timeout or self.timeout)

        # collect any events that landed during the run
        events = []
        while True:
            try:
                events.append(self._inbox_events.get_nowait())
            except Empty:
                break

        return RunResult(
            run_id=data["run_id"],
            ms=int(data.get("ms", 0)),
            data=data,
            events=events,
        )

    # ---- log subscription --------------------------------------------

    def on_log(self, fn: Callable[[dict], None]) -> None:
        self._on_log = fn

    # ---- internals ----------------------------------------------------

    def _drain(self, q: Queue):
        while True:
            try:
                q.get_nowait()
            except Empty:
                return

    def _read_loop(self):
        try:
            while not self._closed:
                op, data = self._ws.recv_data()
                if op == websocket.ABNF.OPCODE_TEXT:
                    self._handle_text(json.loads(data.decode("utf-8")))
                elif op == websocket.ABNF.OPCODE_BINARY:
                    self._handle_binary(data)
                elif op == websocket.ABNF.OPCODE_CLOSE:
                    return
        except Exception:
            return
        finally:
            # Signal callers blocked in call() that the connection is
            # gone. Without this they'd block until their individual
            # timeouts expire.
            self._read_loop_dead = True
            # Wake any pending rsp-waiters with a synthetic shutdown
            # marker so call() can raise ConnectionLostError instead of
            # hanging until its timeout.
            for q in list(self._rsp_waiters.values()):
                try:
                    q.put({"ok": False, "error": "connection_lost",
                           "data": None, "_synthetic": True})
                except Exception:
                    pass

    def _handle_text(self, msg: dict):
        t = msg.get("type")
        if t == "rsp":
            q = self._rsp_waiters.get(msg.get("id"))
            if q is not None:
                q.put(msg)
        elif t == "event":
            self._inbox_events.put(msg)
        elif t == "log":
            self._inbox_logs.put(msg)
            if self._on_log:
                try:
                    self._on_log(msg)
                except Exception:
                    pass
        elif t == "instances":
            # not used by the SDK directly; surface as an event for consumers
            self._inbox_events.put({"name": "instances", "data": msg})

    def _handle_binary(self, data: bytes):
        # Binary frames are OPAQUE to this generic client — it does not interpret
        # any plugin's payload format. They're queued raw (bytes) on _inbox_binary
        # so a caller that knows a plugin's format can drain + decode them itself
        # (e.g. an `expose` consumer decoding XEX1). The client stays content-
        # agnostic; on_binary, if set, also gets the raw bytes.
        self._inbox_binary.put(data)
        cb = self._on_binary
        if cb is not None:
            try:
                cb(data)
            except Exception:
                pass

    def on_binary(self, cb):
        """Register a callback fired with each raw binary WS frame (bytes).
        Generic passthrough — no decoding. Pass None to clear."""
        self._on_binary = cb

    def drain_binary(self) -> list[bytes]:
        """Pop all raw binary frames queued since the last drain."""
        out: list[bytes] = []
        try:
            while True:
                out.append(self._inbox_binary.get_nowait())
        except Empty:
            pass
        return out
