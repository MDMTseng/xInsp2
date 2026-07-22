// Codegen probe: proves the generated RunOutcome interface compiles under
// `strict` AND accepts a real run_result payload (the run_result.json fixture's
// data). If a field type drifted from the wire this would fail tsc. Committed as
// evidence; `tsc --noEmit -p .` from this dir type-checks it.
import type { RunOutcome } from './run_outcome.generated';

// Shape lifted verbatim from protocol/fixtures/run_result.json .data:
const ok: RunOutcome = {
  code: 1,
  msg: 'ok',
  run_id: 17,
  ms: 12,
  source: '@script',
  group: 'main',
  trigger_id: '9f3c1a2b4d5e6f708192a3b4c5d6e7f8',
  boot_id: '01234567-89ab-cdef-0123-456789abcdef',
  station_id: 'cell-A',
  inspection_id: 'cell-A/01234567-89ab-cdef-0123-456789abcdef/17',
  schema: 'xi.run-outcome/1',
  class: 'ok',
  reason_code: '',
  script_generation: 4,
};

// The dropped-trigger frame: run_id/ms absent (both optional) — must still type.
const dropped: RunOutcome = { code: -999001, schema: 'xi.run-outcome/1', class: 'dropped' };

// The discriminator constant is a literal type, so a wrong schema is a compile error:
// @ts-expect-error schema must be the exact xi.run-outcome/1 constant
const badSchema: RunOutcome = { code: 0, schema: 'xi.run-outcome/2' };

export const _probe = { ok, dropped, badSchema };
