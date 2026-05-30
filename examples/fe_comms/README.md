# fe_comms — FE supervises the comms gateway (Increment 3)

The comms gateway (`xinsp-comms`) is an out-of-process PLC I/O plugin: it owns the
PLC socket so its hang/crash risk is isolated from the in-process backend compute
core. Increment 1 built the gateway, Increment 2 the backend-side `xi::comms`
client. **This example proves Increment 3: `xinsp-fe.exe` spawns and supervises
the gateway as a sibling of the backend.**

`driver.py`:

1. Stands up a fake UDP "PLC" sim.
2. Launches `xinsp-fe.exe --comms-plc=udp:<sim> --autostart-fps=…`. The FE brings
   up BOTH the gateway and the backend, and passes the backend `--comms-port`.
3. Asserts the script's per-frame `xi::comms::send()` reaches the PLC sim
   (script → gateway → PLC).
4. Kills **only** the gateway (name-guarded: must be `xinsp-comms.exe`, matched by
   our unique `--listen` port) and asserts the FE log shows
   `comms gateway exited` → `ENTER SAFE STATE reason=CommsLost` →
   `respawning comms gateway` → `comms gateway back up`.
5. Asserts the **backend survived** the gateway death, a fresh gateway is running,
   and sends reach the PLC sim again (link restored).
6. Stops the FE and asserts no orphans.

```
python driver.py     # VERDICT: PASS
```

Windows-only today (process spawn / Job Object). Skips on non-`nt`. See
[`docs/design/comms-gateway.md`](../../docs/design/comms-gateway.md) and
[`docs/design/fe-be-split.md`](../../docs/design/fe-be-split.md).

The dead-man path (backend crash → gateway fires the registered payload to the
PLC) is covered by [`examples/comms_gateway/`](../comms_gateway/); the script↔PLC
round trip without the FE by [`examples/comms_script/`](../comms_script/).
