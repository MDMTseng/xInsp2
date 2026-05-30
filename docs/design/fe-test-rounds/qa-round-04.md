# QA team loop — Round 4 (countdown 7 → 6) · extension decision unit + DRYNESS

Targeted round: the one area QA round 1's four viewpoints never touched — the VS
Code `backendMode` (managed/attach) logic had no automated test.

## Gap
The extension must not spawn/respawn a backend the `xinsp-fe` supervisor owns.
That decision lived inline in `extension.ts` (untestable without the VS Code
extension host, which the `vscode` import requires).

## Fix + test
- Extracted the decision into `vscode-extension/src/backendMode.mjs` —
  dependency-free, single source of truth; `extension.ts` now imports
  `resolveBackendMode()`.
- `test/backend_mode.test.mjs` (node --test): managed/attach ignore the probe;
  auto attaches iff a backend is already listening; unknown/empty → default
  (auto). **4/4 pass.** esbuild build clean with the `.mjs` import.
- Windows ESM note: used a static relative import (dynamic `import()` of an
  absolute `C:\` path throws `ERR_UNSUPPORTED_ESM_URL_SCHEME`).

## Result
No new **product** bug — round 4 added coverage for a known gap. ctest 11/11;
all FE/QA drivers + the new node test green.

## Dryness declaration
After 4 targeted rounds the loop is **dry for quick-fix work**: rounds now yield
coverage, not bugs. The remaining items are infrastructure-level, not targeted
fixes, and are tracked as their own follow-ups:

1. **Serve-time wedge deep heartbeat** (FE-E4b) — detect a backend that's bound +
   accepting TCP but stalled on commands. Needs a real WS handshake/ping client
   in the FE (there is none today). Phase 2.
2. **Extension-host e2e** — drive the actual spawn-guard / status-bar / crash
   toast in attach mode via `@vscode/test-electron`. The decision logic is now
   unit-tested; the UI wiring is e2e-scoped.
3. **Boot-timeout default (60 s)** — may be tight for a cold first compile on a
   slow machine; consider raising or making it adaptive. Minor.
4. Carried: `--be-arg` doesn't quote args with spaces (verbatim passthrough).

Per "targeted rounds till dry," stopping here (countdown 6) rather than grinding
to 0 on convergent re-sweeps. The countdown file + these notes make it resumable
if the Phase-2 items are picked up.

## Loop tally (product issues fixed across the QA loop)
1. cp950 em-dash decode failure (FE output → ASCII).
2. "port-up ≠ ready" → `autostart: ready` marker.
3. dead open-only path + wrong relative-`--script` base.
4. healthy-liveness signal (announce once per instance).
5. `ts=` timestamp in the safe-state log.
6. boot-readiness gate (`BootTimeout`) — boot-hang detection.
+ test bug fixed (qa_fault cap-loop bound); extension decision extracted + tested.
