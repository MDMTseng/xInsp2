"""Synchronous WS client for xInsp2.

One client = one connection. Not thread-safe; intended for scripts driven
by an AI agent (or a human at a REPL). Spec: docs/protocol.md.
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
    pass


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
        self._on_log: Callable[[dict], None] | None = None

    # ---- lifecycle ----------------------------------------------------

    def connect(self) -> dict:
        self._ws = websocket.create_connection(self.url, timeout=self.timeout)
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
        cid = self._next_id
        self._next_id += 1
        q: Queue = Queue()
        self._rsp_waiters[cid] = q
        self._ws.send(json.dumps({"type": "cmd", "id": cid, "name": name, "args": args or {}}))
        try:
            rsp = q.get(timeout=timeout or self.timeout)
        finally:
            self._rsp_waiters.pop(cid, None)
        if not rsp.get("ok"):
            raise ProtocolError(f"cmd {name!r} failed: {rsp.get('error')}")
        return rsp.get("data")

    # ---- high-level ---------------------------------------------------

    def ping(self) -> dict:
        return self.call("ping")

    def version(self) -> dict:
        return self.call("version")

    def compile_and_load(self, path: str) -> dict:
        return self.call("compile_and_load", {"path": path}, timeout=180)

    def load_project(self, path: str) -> None:
        # Cold opens compile every project-local plugin under cl.exe; the
        # 30 s default is too short for any non-trivial project.
        self.call("load_project", {"path": path}, timeout=180)

    def open_project(self, path: str) -> None:
        # Alias for `load_project`; some tooling uses the `open_project`
        # cmd name. Behaviour is identical from the SDK's perspective.
        self.call("open_project", {"path": path}, timeout=180)

    def recompile_project_plugin(self, plugin: str) -> dict:
        """Hot-rebuild one project-local plugin. Linchpin for live tuning.

        Returns `{plugin, diagnostics, reattached}` on success. On compile
        failure raises `ProtocolError` whose message contains the build
        log; the previous DLL stays loaded so the inspection keeps working.
        """
        return self.call("recompile_project_plugin", {"plugin": plugin}, timeout=120)

    def set_param(self, name: str, value) -> None:
        self.call("set_param", {"name": name, "value": value})

    # ---- discovery / introspection -----------------------------------

    def list_plugins(self) -> list[dict]:
        """Every registered plugin (global + project-local).

        Each entry: `{name, description, folder, has_ui, loaded, origin,
        cert, manifest?}`. `manifest` is the optional structured
        `params / inputs / outputs / exchange` block from the plugin's
        `plugin.json` (see docs/reference/plugin-abi.md).
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

        Returns whatever the plugin chose to send back (typically a
        JSON object parsed into a dict).
        """
        return self.call("exchange_instance", {"name": name, "cmd": cmd_obj})

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
        """Run one inspect() and collect the resulting vars + previews.

        Drains vars + binary frames until either run_finished event arrives or
        the timeout elapses.
        """
        # Drain stale messages from any prior async activity.
        self._drain(self._inbox_vars)
        self._drain(self._inbox_previews)
        self._drain(self._inbox_events)

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
