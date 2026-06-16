"""Synchronous WS client for xInsp2.

One client = one connection. Not thread-safe; intended for scripts driven
by an AI agent (or a human at a REPL). Spec: docs/reference/ws-protocol.md.
"""
from __future__ import annotations

import json
import struct
import threading
from dataclasses import dataclass, field
from queue import Queue, Empty
from typing import Any, Callable

import websocket  # websocket-client


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
    `next_vars()` is called after the read loop exited. Callers
    looping on `next_vars()` should treat this as a stop signal."""
    pass


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
class PreviewFrame:
    gid: int
    codec: int           # 0=JPEG, 1=BMP, 2=PNG
    width: int
    height: int
    channels: int
    payload: bytes

    @property
    def codec_name(self) -> str:
        return {0: "jpeg", 1: "bmp", 2: "png"}.get(self.codec, f"unknown({self.codec})")


@dataclass
class RunResult:
    run_id: int
    ms: int
    vars: list[dict]                  # raw items array from `vars` message
    previews: dict[int, PreviewFrame] # gid -> PreviewFrame
    events: list[dict] = field(default_factory=list)

    def var(self, name: str) -> dict | None:
        return next((v for v in self.vars if v["name"] == name), None)

    def value(self, name: str, default=None):
        v = self.var(name)
        return default if v is None else v.get("value", default)

    def image(self, name: str) -> PreviewFrame | None:
        v = self.var(name)
        if not v or v.get("kind") != "image":
            return None
        return self.previews.get(v["gid"])


class Client:
    def __init__(self, url: str = "ws://127.0.0.1:7823/", timeout: float = 30.0):
        self.url = url
        self.timeout = timeout
        self._ws: websocket.WebSocket | None = None
        self._next_id = 1
        self._rsp_waiters: dict[int, Queue] = {}
        self._inbox_vars: Queue = Queue()
        self._inbox_previews: Queue = Queue()
        self._inbox_events: Queue = Queue()
        self._inbox_logs: Queue = Queue()
        self._reader: threading.Thread | None = None
        self._closed = False
        self._read_loop_dead = False
        self._on_log: Callable[[dict], None] | None = None

    # ---- lifecycle ----------------------------------------------------

    def connect(self) -> dict:
        try:
            self._ws = websocket.create_connection(self.url, timeout=self.timeout)
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
        """
        return self.call("load_project", {"path": path}, timeout=180)

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

    def resume(self) -> dict:
        """Release a script parked at `xi::breakpoint("label")`.

        Returns `{resumed: bool, label: str}`. `resumed=false` if no
        breakpoint was active.
        """
        return self.call("resume")

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

    def next_vars(self, timeout: float | None = None) -> dict | None:
        """Pop the next `vars` message from the queue, blocking up to
        `timeout` seconds. Returns the raw vars dict (`{"type":"vars",
        "run_id":N,"items":[...]}`) or `None` on timeout.

        Raises `ConnectionLostError` if the WS read loop has already
        exited. Loop callers should treat that as a stop signal —
        without it a `while next_vars(1) is not None` after disconnect
        would spin forever returning None.

        Use this to consume the stream produced by `cmd:start`
        (continuous mode), which doesn't follow the request/reply
        shape of `cmd:run`. The caller is responsible for wiring any
        per-frame logic — the SDK doesn't auto-correlate previews
        here. For the typical "drive 100 frames continuously and
        score them" pattern see the hot_reload_run / stereo_sync
        example drivers.
        """
        if self._read_loop_dead and self._inbox_vars.empty():
            raise ConnectionLostError(
                "WS connection is closed; no further vars will arrive"
            )
        try:
            return self._inbox_vars.get(timeout=timeout or self.timeout)
        except Empty:
            if self._read_loop_dead:
                raise ConnectionLostError(
                    "WS closed while waiting for vars"
                ) from None
            return None

    def run(self, frame_path: str | None = None, timeout: float | None = None) -> RunResult:
        """Run one inspect() and collect the resulting vars + previews.

        Synchronisation: blocks until the `vars` message for this run's
        `run_id` arrives, then drains exactly the previews referenced
        by that vars message (one per `kind:image` item). Events that
        landed during the run are scooped non-blocking into
        `RunResult.events`. (Earlier docstring versions claimed this
        waited for a `run_finished` event — that event is not currently
        emitted by the backend; the rsp + matching `vars` is the
        completion signal.)

        IMPORTANT: this drains stale `vars` and `previews` queues but
        DOES NOT drain `events` — earlier calls' events (e.g.
        `state_dropped` after a `compile_and_load`) stay queued so the
        caller can read them out via `_inbox_events.get_nowait()`.
        Vars/previews are drained because they're tightly coupled to a
        specific run_id and stale ones would mismatch.
        """
        self._drain(self._inbox_vars)
        self._drain(self._inbox_previews)
        # Note: _inbox_events deliberately left intact — see docstring.

        args = {"frame_path": frame_path} if frame_path else {}
        data = self.call("run", args, timeout=timeout or self.timeout)
        run_id = data["run_id"]

        # vars message
        vars_msg = self._inbox_vars.get(timeout=timeout or self.timeout)
        if vars_msg.get("run_id") != run_id:
            raise ProtocolError(f"vars run_id {vars_msg.get('run_id')} != {run_id}")

        # collect previews — one per image item
        wanted_gids = {it["gid"] for it in vars_msg["items"] if it["kind"] == "image"}
        previews: dict[int, PreviewFrame] = {}
        deadline = (timeout or self.timeout)
        while wanted_gids:
            try:
                pf: PreviewFrame = self._inbox_previews.get(timeout=deadline)
            except Empty:
                break
            previews[pf.gid] = pf
            wanted_gids.discard(pf.gid)

        # collect any events that landed during the run
        events = []
        while True:
            try:
                events.append(self._inbox_events.get_nowait())
            except Empty:
                break

        return RunResult(
            run_id=run_id,
            ms=int(data.get("ms", 0)),
            vars=vars_msg["items"],
            previews=previews,
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
            # Signal callers waiting on next_vars / call() that the
            # connection is gone. Without this they'd block until their
            # individual timeouts expire and then return None forever.
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
        elif t == "vars":
            self._inbox_vars.put(msg)
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
        if len(data) < 20:
            return
        gid, codec, w, h, ch = struct.unpack(">IIIII", data[:20])
        self._inbox_previews.put(PreviewFrame(
            gid=gid, codec=codec, width=w, height=h, channels=ch, payload=data[20:],
        ))
