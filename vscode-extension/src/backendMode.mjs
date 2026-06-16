// backendMode.mjs — pure backend-ownership decision for the FE/BE split.
//
// Kept dependency-free (no `vscode` import) so it is the single source of truth
// for both extension.ts and a node --test unit (test/backend_mode.test.mjs).
// See docs/internals/fe-be.md "managed vs attach".
//
// managed : the extension owns the backend (spawns + respawns it).
// attach  : a supervisor (xinsp-fe.exe) owns it; connect read-only, never spawn.
// auto    : attach if a backend is already listening on the port, else managed.

/**
 * @param {string} configMode  the `xinsp2.backendMode` setting ('auto'|'managed'|'attach')
 * @param {boolean} portAlreadyOpen  whether something is already listening on the WS port
 * @returns {'managed'|'attach'}  the resolved ownership mode
 */
export function resolveBackendMode(configMode, portAlreadyOpen) {
    if (configMode === 'attach') return 'attach';
    if (configMode === 'managed') return 'managed';
    // 'auto' (and any unknown value — 'auto' is the default) decides by probe.
    return portAlreadyOpen ? 'attach' : 'managed';
}
