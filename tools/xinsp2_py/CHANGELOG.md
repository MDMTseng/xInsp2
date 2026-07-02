# Changelog

## 0.2.0 — WS run-outcome contract catch-up

Brings the client up to date with wire changes already on the backend
(`master`). Additive and backward-tolerant: parsing older events still works
(missing optional fields resolve to `None`/defaults).

### Added
- `RunOutcome` — typed parse of the `run_result` event (schema
  `xi.run-outcome/1`). Exposes the additive identity/outcome fields:
  `trigger_id`, `boot_id`, `station_id`, `inspection_id`, `schema`,
  `verdict_class` (the JSON `class` key, renamed because `class` is a Python
  keyword), `reason_code`, `script_generation`, plus `is_ok/is_ng/is_na/
  is_crashed/is_no_verdict/is_dropped` helpers.
- `RunFinished` — typed parse of `run_finished`, exposing the added
  `inspect_compute_us` (µs-precision inspect COMPUTE time) alongside `ms`.
- `RunResult.outcome` / `RunResult.finished` typed accessors that pull the
  per-run `run_result` / `run_finished` events out of `RunResult.events`.
- System result-code constants `XI_SYS_CRASHED = -999002`,
  `XI_SYS_NO_VERDICT = -999005`, `XI_SYS_DROPPED = -999001`; class-string
  constants `CLASS_OK/NG/NA/NO_VERDICT/CRASHED/DROPPED`;
  `outcome_class_for_code()`; and `RUN_RESULT_SCHEMA`.
- `Client.commit_group()` and `Client.metrics()` helpers, plus
  `Client.metrics_inspect_compute()` which tolerates the `latency_ms` ->
  `inspect_compute_ms` metrics-key rename.
- `PartialStatusError` (subclass of `ProtocolError`).
- `snapshot.dump_run` now records the typed `outcome` block and
  `inspect_compute_us` in `report.json`.
- Test suite `tests/test_run_outcome.py` parsing the protocol golden fixtures.

### Changed / wire-compat notes
- **BREAKING (backend, reflected here):** a caught inspect error now emits
  `code = -999002` (was `0`) and a run that set no result emits `code =
  -999005` (was `0`). An explicit `xi::result(0)` is still `0` (`na`). The
  client can now distinguish crash / no-verdict / na on the numeric channel.
- **`load_project`** now returns `status` ("ok"|"partial"|"rejected") and comes
  back `ok:false` on partial/rejected. `Client.load_project()` re-raises these
  as `PartialStatusError` — a partial/rejected load is NOT treated as success.
- **`commit_group`** gained `status` ("committed"|"partial"); after a partial
  commit the backend no longer auto-resumes dispatch.
  `Client.commit_group()` raises `PartialStatusError` on a non-"committed"
  status.

## 0.1.0
- Initial synchronous WS client.
