# plc_safe_state — FE drives a PLC over TCP/UDP on backend death

Proves the `SafeStateSink` PLC transport end to end: when a plugin crashes the
backend, the `xinsp-fe` supervisor sends a newline-JSON **safe-state command** to
the PLC (carrying the crash forensics), so a real line would be told to go safe.

`driver.py` stands up a tiny **UDP "PLC simulator"**, runs the FE on the crashing
project (`examples/fe_supervisor`) with `--safe-state=udp:127.0.0.1:<sim>`, and
asserts the sim receives a `state:"enter"` command with `reason`/`module`/`phase`.

```
python driver.py
```

## Using it for real
```
xinsp-fe --project=<your project> --autostart-fps=N --safe-state=udp:PLC_HOST:PORT
xinsp-fe ... --safe-state=tcp:PLC_HOST:PORT
```
Message (newline-delimited JSON; adapt `build_enter()/build_clear()` in
`backend/include/xi/xi_safe_state_plc.hpp` to your PLC's keys):
```json
{"src":"xinsp-fe","event":"safe_state","state":"enter","reason":"BackendExit","rc":"0xC0000005","module":"plugin_v0.dll","phase":"inspect","ts_ms":1780...}
{"src":"xinsp-fe","event":"safe_state","state":"clear","ts_ms":1780...}
```
UDP repeats the `enter` command (loss tolerance); TCP does a short-timeout
per-message connect. MessagePack framing is a planned follow-up.

(`fe.log` / `be.log` are generated; gitignored.)
