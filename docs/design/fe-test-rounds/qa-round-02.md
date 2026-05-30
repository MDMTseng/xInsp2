# QA team loop — Round 2 (countdown 9 → 8) · targeted: safe-state timestamp

Targeted round (per "targeted rounds till dry"): close the `ts_ms` gap flagged
in solo round 1.

## Gap
`SafeStateEvent` carried `ts_ms` (epoch-ms the FE went safe) but
`LoggingSafeStateSink` never logged it — a safety-transition log with no
timestamp is poor forensics.

## Fix + test
- `xi_safe_state.hpp`: `enter_safe_state` now appends `ts=<epoch-ms>` to the
  line. Placed at the END so the `ENTER SAFE STATE reason=...` prefix that the
  drivers scrape stays stable.
- `test_safe_state.cpp` SS-U8: assert the line carries `ts=<value>`.

## Result
ctest 11/11; the FE example drivers (which grep the `reason=` prefix) unaffected.

## Next (note for round 3)
The biggest remaining concrete gap: a backend that hangs during boot is
invisible to the connect-only probe (port bound == "up"). → boot-readiness gate.
