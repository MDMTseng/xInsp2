# qa_soak — full-stack stability soak (Phase G, #92)

The fault tests prove the supervisor reacts correctly to crashes. This proves the
inverse — just as important on a line: under sustained **normal** operation the
FE must **not** trip a false safe-state, the backend must keep serving (heartbeat
advancing), and the comms link must stay up for the whole soak, then shut down
clean with no orphan.

`driver.py` brings up `xinsp-fe.exe --comms-plc=udp:<sim> --autostart-fps=N` (FE +
backend + gateway) running a deliberately boring healthy script (one PLC send per
frame), soaks for `QA_SOAK_S` seconds (default 15), and asserts:

- the FE announced the backend healthy and the link up,
- **no** `ENTER SAFE STATE` of any reason, **no** `respawning` (BE or gateway),
- the backend heartbeat advanced steadily (~1 Hz serving-loop beat — not wedged),
- the PLC sim received sends in both an early and a late window (link stayed up),
- backend + exactly one gateway still up, FE alive at the end; no orphan after shutdown.

```
python driver.py             # 15s soak, VERDICT: PASS
QA_SOAK_S=60 python driver.py  # longer soak
```

Windows-only today (process spawn / Job Object). Skips on non-`nt`. See
[`docs/design/fe-be-split-test-plan.md`](../../docs/design/fe-be-split-test-plan.md)
"Phase G".
