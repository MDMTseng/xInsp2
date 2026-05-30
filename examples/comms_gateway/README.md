# comms_gateway — the out-of-process comms gateway (Increment 1)

Exercises `xinsp-comms.exe` in isolation: it owns the PLC connection and relays
newline-JSON between one loopback client (the backend, later) and the PLC. See
[`../../docs/design/comms-gateway.md`](../../docs/design/comms-gateway.md).

`driver.py` stands up a UDP "PLC simulator", runs the gateway pointed at it,
connects a loopback client, and round-trips both ways:
```
python driver.py
```

## Protocol (newline-delimited JSON; gateway is schema-agnostic about the PLC payload)
```
client -> gateway : {"id":N,"op":"send","line":"<verbatim to PLC>"}
                    {"id":N,"op":"set_deadman","line":"<emergency line>"}  (fire on crash-drop)
                    {"id":N,"op":"bye"}                                    (clean shutdown; disarm)
                    {"id":N,"op":"ping"}
gateway -> client : {"id":N,"ok":true} / {"id":N,"ok":false,"err":"..."}
                    {"event":"plc_in","line":"<verbatim from PLC>"}    (async)
                    {"event":"plc_up","up":true|false}                 (PLC link state)
```

## Dead-man safety
The backend registers an emergency payload via `set_deadman`. If its connection
to the gateway drops **without a `bye`** (i.e. the backend crashed), the gateway
sends that payload to the PLC immediately so it can do emergency handling — the
gateway owns the PLC link, so it's the natural place to fire it. (If the gateway
itself dies, the PLC sees its TCP connection drop and dead-mans on its own.)
The test covers both: fires on crash-drop, silent after a clean `bye`.

## Run the gateway directly
```
xinsp-comms --plc=udp:PLC_HOST:PORT --listen=7900
xinsp-comms --plc=tcp:PLC_HOST:PORT --listen=7900
```

Next increments (see the design doc): a backend-side `xi::io` handle the script
calls; the FE supervising the gateway as a sibling of the backend. (`gw.log` is
generated; gitignored.)
