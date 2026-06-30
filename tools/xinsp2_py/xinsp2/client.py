"""Synchronous WS client for xInsp2.

One client = one connection. Not thread-safe; intended for scripts driven
by an AI agent (or a human at a REPL). Spec: docs/reference/ws-protocol.md.
"""
from __future__ import annotations

import base64
import json
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
    """Raised when the SDK detects the WS connection has dropped, e.g. a
    `call()` is issued after the read loop exited. Callers looping on the
    client should treat this as a stop signal."""
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


_NONFINITE = {"NaN": float("nan"), "Infinity": float("inf"), "-Infinity": float("-inf")}


def _restore_nonfinite(v):
    """Map a single quoted sentinel string to its float; pass anything else through."""
    return _NONFINITE.get(v, v) if isinstance(v, str) else v


def _restore_nonfinite_deep(v):
    """Recursively restore non-finite sentinels inside a record's `data` (dict/list).
    A plugin can write a NaN/Inf field into a nested Record; the backend emits it as
    the quoted string "NaN" deep inside kind:"record" data. Restoring only the
    top-level number var left these as strings -> `str > float` raised TypeError on a
    threshold compare. Mirrors restoreNonFiniteDeep in ui-components/src/protocol.mjs."""
    if isinstance(v, str):
        return _restore_nonfinite(v)
    if isinstance(v, list):
        return [_restore_nonfinite_deep(x) for x in v]
    if isinstance(v, dict):
        return {k: _restore_nonfinite_deep(x) for k, x in v.items()}
    return v


# ---- msgpack decoder (minimal subset for the XEX1 frame) -----------------
#
# Hand-rolled to avoid a `msgpack` pip dependency. Decodes exactly the subset
# the `expose` plugin's encoder emits (plugins/expose/src/expose.cpp): fixmap,
# fixarray/array16/array32, fixstr/str8/str16/str32, bin8/bin16/bin32,
# positive-fixint/uint8/uint16/uint32/uint64, plus nil/bool (and negative
# fixint, which costs nothing to accept). All multi-byte fields big-endian.
# It is NOT a general msgpack decoder — float/int64-signed/ext are out of
# scope because the producer never emits them inside this frame.

class MsgpackError(ValueError):
    """Raised when the XEX1 msgpack body is malformed or uses a type
    outside the fixed subset the `expose` frame is defined over."""
    pass


def _mp_decode(data: bytes, i: int) -> tuple[Any, int]:
    """Decode one msgpack value at offset `i`; return (value, next_offset)."""
    if i >= len(data):
        raise MsgpackError("unexpected end of msgpack body")
    b = data[i]
    i += 1

    # positive fixint
    if b <= 0x7F:
        return b, i
    # negative fixint
    if b >= 0xE0:
        return b - 0x100, i
    # fixstr
    if 0xA0 <= b <= 0xBF:
        n = b & 0x1F
        return _mp_take_str(data, i, n)
    # fixmap
    if 0x80 <= b <= 0x8F:
        return _mp_map(data, i, b & 0x0F)
    # fixarray
    if 0x90 <= b <= 0x9F:
        return _mp_array(data, i, b & 0x0F)

    if b == 0xC0:  # nil
        return None, i
    if b == 0xC2:  # false
        return False, i
    if b == 0xC3:  # true
        return True, i

    if b == 0xCC:  # uint8
        return data[i], i + 1
    if b == 0xCD:  # uint16
        return int.from_bytes(data[i:i + 2], "big"), i + 2
    if b == 0xCE:  # uint32
        return int.from_bytes(data[i:i + 4], "big"), i + 4
    if b == 0xCF:  # uint64
        return int.from_bytes(data[i:i + 8], "big"), i + 8

    if b == 0xD9:  # str8
        n = data[i]; i += 1
        return _mp_take_str(data, i, n)
    if b == 0xDA:  # str16
        n = int.from_bytes(data[i:i + 2], "big"); i += 2
        return _mp_take_str(data, i, n)
    if b == 0xDB:  # str32
        n = int.from_bytes(data[i:i + 4], "big"); i += 4
        return _mp_take_str(data, i, n)

    if b == 0xC4:  # bin8
        n = data[i]; i += 1
        return _mp_take_bin(data, i, n)
    if b == 0xC5:  # bin16
        n = int.from_bytes(data[i:i + 2], "big"); i += 2
        return _mp_take_bin(data, i, n)
    if b == 0xC6:  # bin32
        n = int.from_bytes(data[i:i + 4], "big"); i += 4
        return _mp_take_bin(data, i, n)

    if b == 0xDC:  # array16
        n = int.from_bytes(data[i:i + 2], "big"); i += 2
        return _mp_array(data, i, n)
    if b == 0xDD:  # array32
        n = int.from_bytes(data[i:i + 4], "big"); i += 4
        return _mp_array(data, i, n)

    raise MsgpackError(f"unsupported msgpack tag 0x{b:02X} at offset {i - 1}")


def _mp_take_str(data: bytes, i: int, n: int) -> tuple[str, int]:
    end = i + n
    if end > len(data):
        raise MsgpackError("msgpack str overruns body")
    return data[i:end].decode("utf-8"), end


def _mp_take_bin(data: bytes, i: int, n: int) -> tuple[bytes, int]:
    end = i + n
    if end > len(data):
        raise MsgpackError("msgpack bin overruns body")
    return data[i:end], end


def _mp_array(data: bytes, i: int, n: int) -> tuple[list, int]:
    out = []
    for _ in range(n):
        v, i = _mp_decode(data, i)
        out.append(v)
    return out, i


def _mp_map(data: bytes, i: int, n: int) -> tuple[dict, int]:
    out = {}
    for _ in range(n):
        k, i = _mp_decode(data, i)
        v, i = _mp_decode(data, i)
        out[k] = v
    return out, i


@dataclass
class ExposeFrame:
    """One decoded `XEX1` record from the `expose` plugin.

    `values` is the record's scalar tree (`json.loads` of the frame's `json`
    field, with non-finite sentinels restored). `images` maps each image key
    to its JPEG-encoded bytes.
    """
    channel: str
    seq: int
    values: dict
    images: dict[str, bytes] = field(default_factory=dict)
    v: int = 1

    def image(self, key: str) -> bytes | None:
        """JPEG bytes for one image key, or None if the frame has no such key."""
        return self.images.get(key)


def decode_xex1(data: bytes) -> ExposeFrame:
    """Decode a single atomic `XEX1` binary frame into an `ExposeFrame`.

    Wire shape (see plugins/expose/src/expose.cpp):
        [0..3] ASCII magic 'XEX1', then a msgpack map
        { v:1, channel:<str>, seq:<uint>, json:<str>, images:[{key,jpeg}] }

    Raises `MsgpackError` if the magic/version gate fails or the body is
    malformed. The magic + `v` gate is what kills the F-6 stale-decoder drift.
    """
    if len(data) < 4 or data[:4] != b"XEX1":
        raise MsgpackError(
            f"not an XEX1 frame (magic={data[:4]!r}); refusing to decode"
        )
    body, _ = _mp_decode(data, 4)
    if not isinstance(body, dict):
        raise MsgpackError("XEX1 body is not a msgpack map")
    v = body.get("v", 1)
    if v != 1:
        raise MsgpackError(f"unsupported XEX1 frame version {v!r} (decoder handles v=1)")
    raw_json = body.get("json", "{}")
    values = json.loads(raw_json) if raw_json else {}
    if isinstance(values, (dict, list)):
        values = _restore_nonfinite_deep(values)
    images: dict[str, bytes] = {}
    for ent in body.get("images") or []:
        if isinstance(ent, dict) and "key" in ent and isinstance(ent.get("jpeg"), (bytes, bytearray)):
            images[ent["key"]] = bytes(ent["jpeg"])
    return ExposeFrame(
        channel=body.get("channel", "default"),
        seq=int(body.get("seq", 0)),
        values=values if isinstance(values, dict) else {"_": values},
        images=images,
        v=int(v),
    )


@dataclass
class RunResult:
    """Result of one `run()`: the latest `expose` frame collected per channel.

    The pre-v9 `vars`/`gid`/per-image-name model is gone — output now arrives
    as atomic XEX1 frames, one per subscribed channel (subscribe first via
    `client.subscribe(...)`). `frames` is empty for a run whose channels have
    no subscriber.
    """
    run_id: int
    ms: int
    frames: dict[str, ExposeFrame] = field(default_factory=dict)
    events: list[dict] = field(default_factory=list)

    def expose(self, channel: str) -> ExposeFrame | None:
        """The latest `ExposeFrame` for `channel`, or None if not present."""
        return self.frames.get(channel)

    def values(self, channel: str) -> dict | None:
        """Convenience: the scalar values dict for `channel`, or None."""
        f = self.frames.get(channel)
        return f.values if f else None

    def image(self, channel: str, key: str) -> bytes | None:
        """JPEG bytes for image `key` on `channel`, or None if absent."""
        f = self.frames.get(channel)
        return f.image(key) if f else None


class Client:
    def __init__(self, url: str = "ws://127.0.0.1:7823/", timeout: float = 30.0):
        self.url = url
        self.timeout = timeout
        self._ws: websocket.WebSocket | None = None
        self._next_id = 1
        self._rsp_waiters: dict[int, Queue] = {}
        self._inbox_frames: Queue = Queue()   # decoded ExposeFrame (XEX1 binary)
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
        # Output (`expose` frames) is subscription-gated in the plugin, not a
        # backend-WS subscribe. Opt into channels explicitly via
        # `client.subscribe([...])` (see docs/roadmap/expose-plugin-and-output-
        # transport.md §4) — nothing to do here.
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

    # ---- expose: subscription + pull ---------------------------------
    #
    # Output rides the `expose` plugin (the VAR replacement). Subscription is
    # tracked IN THE PLUGIN over its `exchange` channel — NOT a backend WS
    # command — because the core's `emit_binary` is a dumb broadcast pipe
    # (docs/roadmap/expose-plugin-and-output-transport.md §4 / §10). Subscribed
    # channels are JPEG-encoded + pushed as XEX1 frames each run; the SDK
    # filters the broadcast by channel. Pull-latest stays available via
    # `get_expose`.

    EXPOSE_INSTANCE = "expose"

    def subscribe(self, channels: str | list[str], *, instance: str | None = None) -> dict:
        """Subscribe one or more `expose` channels so each run pushes their
        XEX1 frame. Returns the plugin's `{ok, subscribed:[...]}` reply."""
        chans = [channels] if isinstance(channels, str) else list(channels)
        return self.exchange_instance(
            instance or self.EXPOSE_INSTANCE,
            {"command": "subscribe", "channels": chans},
        )

    def unsubscribe(self, channels: str | list[str], *, instance: str | None = None) -> dict:
        """Stop pushing the given `expose` channel(s). Returns the plugin's
        `{ok, subscribed:[...]}` reply."""
        chans = [channels] if isinstance(channels, str) else list(channels)
        return self.exchange_instance(
            instance or self.EXPOSE_INSTANCE,
            {"command": "unsubscribe", "channels": chans},
        )

    def list_channels(self, *, instance: str | None = None) -> dict:
        """Channel metadata from the `expose` plugin:
        `{count, channels:{name:{seen, image_count, subscribed}}}`."""
        return self.exchange_instance(
            instance or self.EXPOSE_INSTANCE, {"command": "list_channels"})

    def get_expose(self, channel: str, *, instance: str | None = None) -> ExposeFrame | None:
        """Pull the latest frame for `channel` on demand (no subscription
        needed). Decodes the plugin's base64 `frame_b64` with the SAME XEX1
        decoder used for the pushed frames. Returns None if the channel has
        never been written (`found:false`)."""
        rsp = self.exchange_instance(
            instance or self.EXPOSE_INSTANCE,
            {"command": "get", "channel": channel},
        )
        if not isinstance(rsp, dict) or not rsp.get("found"):
            return None
        return decode_xex1(base64.b64decode(rsp["frame_b64"]))

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

    def run(self, frame_path: str | None = None, timeout: float | None = None,
            settle: float = 0.25) -> RunResult:
        """Run one inspect() and collect the latest `expose` frame per channel.

        Output now arrives as atomic XEX1 binary frames pushed by the `expose`
        plugin — one per *subscribed* channel, stamped with `seq == run_id`.
        So **subscribe first** (`client.subscribe([...])`); a run whose
        channels have no subscriber yields an empty `RunResult.frames`.

        Synchronisation: stale frames are drained, `cmd:run` is issued, then
        frames carrying this run's `seq` are collected, keyed by channel
        (latest wins). Because there is no per-run "done" marker for the push
        path (frames just broadcast), collection ends after `settle` seconds
        with no further matching frame. Events that landed during the run are
        scooped non-blocking into `RunResult.events`.

        Stale frames (`seq < run_id`) are dropped; `_inbox_events` is left
        intact so earlier events (e.g. `state_dropped`) remain readable.
        """
        self._drain(self._inbox_frames)
        # Note: _inbox_events deliberately left intact — see docstring.

        args = {"frame_path": frame_path} if frame_path else {}
        data = self.call("run", args, timeout=timeout or self.timeout)
        run_id = data["run_id"]

        # Collect the XEX1 frames broadcast for this run. Key by channel,
        # keeping the frame whose seq matches this run_id (latest wins).
        frames: dict[str, ExposeFrame] = {}
        while True:
            try:
                fr: ExposeFrame = self._inbox_frames.get(timeout=settle)
            except Empty:
                break
            if fr.seq < run_id:
                continue           # stale frame from a prior run
            if fr.seq > run_id:
                # A frame from a later run arrived (shouldn't happen in the
                # blocking single-run flow) — push it back and stop.
                self._inbox_frames.put(fr)
                break
            frames[fr.channel] = fr

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
            frames=frames,
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
            # Signal callers waiting on call() / run() that the
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
        # Every binary WS message is an atomic XEX1 `expose` frame (broadcast;
        # we filter by channel). A frame that fails the magic/version gate or
        # is otherwise malformed is dropped rather than killing the read loop.
        try:
            frame = decode_xex1(data)
        except Exception:
            return
        self._inbox_frames.put(frame)
