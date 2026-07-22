// Health / state contract — TypeScript side (schema `xi.health/1`).
//
// Mirrors the core-owned health/state contract (docs/new_gen/04-health-contract.md,
// backend `xi_health.hpp`) so the VS Code extension surfaces the ONE canonical
// read of "is the system healthy, and if not, which part isn't" instead of
// inferring liveness from side channels (get_state / status / dispatch_stats).
// Pure data + no `vscode` import — the extension's message dispatch stays a thin
// wrapper and this parsing is unit-testable under `node --test`, exactly like
// `runOutcome.ts`.
//
// Forward-tolerant by construction: unknown state/health/reason strings pass
// through as their literal (never dropped), every additive field is optional,
// and hostile/missing input parses to safe defaults — a payload from an older or
// newer backend loads without throwing. Consumers feature-detect `get_health`
// (an "unknown command" rsp) rather than version-sniffing.

export const HEALTH_SCHEMA = 'xi.health/1';

// Top-level state-machine values (backend `xi::sys_state_name`). Kept as string
// constants — not a TS enum — so an unknown state from a newer backend passes
// through as its literal rather than being coerced or dropped.
export const STATE_BOOT = 'boot';
export const STATE_PROJECT_LOADED = 'project_loaded';
export const STATE_RUNNING = 'running';
export const STATE_DEGRADED = 'degraded';
export const STATE_DRAINING = 'draining';
export const STATE_FAULT = 'fault';

// Component health values (backend `xi::comp_health_name`).
export const HEALTH_OK = 'ok';
export const HEALTH_DEGRADED = 'degraded';
export const HEALTH_FAILED = 'failed';

// Reason codes (backend `kReason*`). Additive: a new code is a doc + a constant,
// never a wire break — so this list is a convenience, not an exhaustive gate.
export const REASON_PLUGIN_FAULT = 'plugin_fault';
export const REASON_PREPARE_FAILED = 'prepare_failed';
export const REASON_COMPILE_ERROR = 'compile_error';
export const REASON_WATCHDOG_TRIP = 'watchdog_trip';

// The two states that mean "a human should look": `degraded` (running but a
// runtime fault is live) and `fault` (unrecoverable, backend exits for respawn).
// Every other value is a normal lifecycle state.
const PROBLEM_STATES = new Set<string>([STATE_DEGRADED, STATE_FAULT]);

export function isProblemState(state: string | undefined): boolean {
    return state != null && PROBLEM_STATES.has(state);
}

// True when the machine ENTERS a problem state — used to fire a one-time warning
// on the transition, not on every subsequent `health_changed` while it stays
// there. `running → degraded` and the `degraded → fault` escalation both fire
// (the target changed); a component-only change within `degraded` does not.
export function enteredProblem(prev: string | undefined, next: string | undefined): boolean {
    return isProblemState(next) && next !== prev;
}

export interface HealthComponent {
    kind: string;          // script | instance | group | source
    name: string;
    health: string;        // ok | degraded | failed
    reason_code: string;   // '' when ok
    since_ms?: number;
    // additive derived extras, present per kind (all optional + tolerant so a
    // consumer need not also poll get_state / dispatch_stats for the common case)
    crash_count?: number;      // instance
    queue_now?: number;        // group
    running?: number;          // group
    dropped?: number;          // group
    last_emit_age_ms?: number; // source
    raw: Record<string, unknown>;
}

export interface HealthSnapshot {
    schema?: string;
    state: string;
    since_ms?: number;
    boot_id?: string;
    station_id?: string;
    components: HealthComponent[];
    // The single changed component on a `health_changed` event; absent on a pure
    // state transition and on a `get_health` snapshot (which lists all in
    // `components`).
    component?: HealthComponent;
    ts_ms?: number;
    raw: Record<string, unknown>;
}

function asData(ev: unknown): Record<string, unknown> {
    if (ev && typeof ev === 'object') {
        const o = ev as Record<string, unknown>;
        const d = 'data' in o ? o.data : o;
        if (d && typeof d === 'object') return d as Record<string, unknown>;
    }
    return {};
}

const num = (v: unknown): number | undefined => (typeof v === 'number' ? v : undefined);
const str = (v: unknown): string | undefined => (typeof v === 'string' ? v : undefined);

function parseComponent(v: unknown): HealthComponent | undefined {
    if (!v || typeof v !== 'object') return undefined;
    const d = v as Record<string, unknown>;
    return {
        kind: str(d.kind) ?? '',
        name: str(d.name) ?? '',
        health: str(d.health) ?? HEALTH_OK,
        reason_code: str(d.reason_code) ?? '',
        since_ms: num(d.since_ms),
        crash_count: num(d.crash_count),
        queue_now: num(d.queue_now),
        running: num(d.running),
        dropped: num(d.dropped),
        last_emit_age_ms: num(d.last_emit_age_ms),
        raw: d,
    };
}

// Parse either a `get_health` reply dict (`{state, components:[…]}`) or a
// `health_changed` event (`{name,data:{state, component?}}` or a bare data dict).
// A missing/garbage `state` defaults to `boot` so the caller always has a state.
export function parseHealth(ev: unknown): HealthSnapshot {
    const d = asData(ev);
    const comps: HealthComponent[] = [];
    if (Array.isArray(d.components)) {
        for (const c of d.components) {
            const pc = parseComponent(c);
            if (pc) comps.push(pc);
        }
    }
    return {
        schema: str(d.schema),
        state: str(d.state) ?? STATE_BOOT,
        since_ms: num(d.since_ms),
        boot_id: str(d.boot_id),
        station_id: str(d.station_id),
        components: comps,
        component: parseComponent(d.component),
        ts_ms: num(d.ts_ms),
        raw: d,
    };
}

// Fold a `health_changed` event onto the last full `get_health` snapshot. The
// event carries the new top-level state plus (optionally) the ONE component that
// changed, not the whole list — so for an accurate "which components are failing"
// tooltip we keep the previous snapshot's component set and upsert the changed
// one (matched by kind+name). Recovery (an `ok` component) upserts too, so it
// drops out of the failing set. With no prior snapshot the event stands alone.
export function mergeHealthEvent(prev: HealthSnapshot | undefined, ev: HealthSnapshot): HealthSnapshot {
    const components = prev ? prev.components.slice() : [];
    if (ev.component) {
        const c = ev.component;
        const i = components.findIndex((x) => x.kind === c.kind && x.name === c.name);
        if (i >= 0) components[i] = c; else components.push(c);
    }
    return {
        schema: ev.schema ?? prev?.schema,
        state: ev.state,
        since_ms: ev.since_ms,
        boot_id: prev?.boot_id,
        station_id: prev?.station_id,
        components,
        component: ev.component,
        ts_ms: ev.ts_ms,
        raw: ev.raw,
    };
}

// Components that are not `ok` — the failing set a degraded/fault state points at.
// Ordered failed-first so the most severe surfaces at the top of a tooltip.
export function failingComponents(snap: HealthSnapshot): HealthComponent[] {
    const bad = snap.components.filter((c) => c.health !== HEALTH_OK);
    return bad.sort((a, b) =>
        (a.health === HEALTH_FAILED ? 0 : 1) - (b.health === HEALTH_FAILED ? 0 : 1));
}

// A short, human line for one component, e.g. `instance "cam0": degraded (plugin_fault)`.
// Used verbatim in the extension tooltip / warning and the HMI text.
export function componentSummary(c: HealthComponent): string {
    const reason = c.reason_code ? ` (${c.reason_code})` : '';
    return `${c.kind} "${c.name}": ${c.health}${reason}`;
}

// One-line summary of every failing component, or '' when all are ok. Callers use
// the emptiness to decide whether to show failing-component detail at all.
export function summarizeFailing(snap: HealthSnapshot): string {
    return failingComponents(snap).map(componentSummary).join(', ');
}
