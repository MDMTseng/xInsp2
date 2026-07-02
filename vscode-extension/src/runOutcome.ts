// Run-outcome contract — TypeScript side (schema `xi.run-outcome/1`).
//
// Mirrors the `RunOutcome` / `RunFinished` models in the Python SDK
// (`tools/xinsp2_py/xinsp2/client.py`, the reference consumer per external
// review 10) so the VS Code extension classifies verdicts the same way. Pure
// data + no `vscode` import, so the extension's message dispatch stays a thin
// wrapper and this logic is unit-testable under `node --test`.
//
// Forward-tolerant by construction: every additive field is optional and an
// unknown/missing key parses to undefined, so an outcome from an older backend
// still loads and a newer one never throws.

export const RUN_RESULT_SCHEMA = 'xi.run-outcome/1';

// Reserved system result codes (backend `XI_SYS_*`). These ride on the numeric
// `code` channel of a `run_result`: a caught inspect error emits XI_SYS_CRASHED,
// a run that set no result emits XI_SYS_NO_VERDICT, a run dropped before inspect
// (backpressure) emits XI_SYS_DROPPED. An explicit `xi::result(0)` is still `0`.
export const XI_SYS_DROPPED = -999001;
export const XI_SYS_CRASHED = -999002;
export const XI_SYS_NO_VERDICT = -999005;

// Derived outcome `class` strings (the wire `class` key; kept out of a TS enum
// so an unknown class from a newer backend passes through as its literal).
export const CLASS_OK = 'ok';
export const CLASS_NG = 'ng';
export const CLASS_NA = 'na';
export const CLASS_NO_VERDICT = 'no_verdict';
export const CLASS_CRASHED = 'crashed';
export const CLASS_DROPPED = 'dropped';

const SYS_CODE_CLASS: Record<number, string> = {
    [XI_SYS_DROPPED]: CLASS_DROPPED,
    [XI_SYS_CRASHED]: CLASS_CRASHED,
    [XI_SYS_NO_VERDICT]: CLASS_NO_VERDICT,
};

// Best-effort class derivation from a numeric result code — mirrors the backend
// `outcome_class_for_code` so an older event missing the additive `class` field
// can still be classified. Prefer the event's own `class` (see parseRunOutcome).
export function outcomeClassForCode(code: number): string {
    if (code in SYS_CODE_CLASS) return SYS_CODE_CLASS[code];
    if (code > 0) return CLASS_OK;
    if (code === 0) return CLASS_NA;
    // Valid ng band is <0 and above the reserved system band (< -990000).
    if (code > -990000) return CLASS_NG;
    return CLASS_NA;
}

export interface RunOutcome {
    code: number;
    msg: string;
    run_id?: number;
    ms?: number;
    source?: string;
    group?: string;
    // additive identity / outcome fields (schema xi.run-outcome/1)
    trigger_id?: string;
    boot_id?: string;
    station_id?: string;
    inspection_id?: string;
    schema?: string;
    verdictClass?: string;   // wire key `class`
    reason_code?: string;
    script_generation?: number;
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

// Parse a `run_result` event dict (`{name,data,...}`) or a bare data dict.
export function parseRunOutcome(ev: unknown): RunOutcome {
    const d = asData(ev);
    const num = (v: unknown): number | undefined =>
        typeof v === 'number' ? v : undefined;
    const str = (v: unknown): string | undefined =>
        typeof v === 'string' ? v : undefined;
    return {
        code: typeof d.code === 'number' ? d.code : 0,
        msg: typeof d.msg === 'string' ? d.msg : '',
        run_id: num(d.run_id),
        ms: num(d.ms),
        source: str(d.source),
        group: str(d.group),
        trigger_id: str(d.trigger_id),
        boot_id: str(d.boot_id),
        station_id: str(d.station_id),
        inspection_id: str(d.inspection_id),
        schema: str(d.schema),
        // `class` is a reserved word — expose it as `verdictClass`.
        verdictClass: str(d['class']),
        reason_code: str(d.reason_code),
        script_generation: num(d.script_generation),
        raw: d,
    };
}

// The outcome class: the event's own `class` when present, else derived from the
// numeric code (older events omit `class`).
export function outcomeClass(o: RunOutcome): string {
    return o.verdictClass || outcomeClassForCode(o.code);
}

export interface RunFinished {
    run_id?: number;
    ms?: number;
    inspect_compute_us?: number;
    raw: Record<string, unknown>;
}

export function parseRunFinished(ev: unknown): RunFinished {
    const d = asData(ev);
    const num = (v: unknown): number | undefined =>
        typeof v === 'number' ? v : undefined;
    return {
        run_id: num(d.run_id),
        ms: num(d.ms),
        inspect_compute_us: num(d.inspect_compute_us),
        raw: d,
    };
}

// Rolling verdict tally for the status bar — the ok/ng/na/crashed counts plus
// the last verdict seen. `record` returns the class it counted so the caller can
// react (colour the status bar, log the line) without re-deriving it.
export class VerdictTally {
    ok = 0;
    ng = 0;
    na = 0;
    no_verdict = 0;
    crashed = 0;
    dropped = 0;
    other = 0;
    total = 0;
    last?: RunOutcome;
    lastClass?: string;

    reset(): void {
        this.ok = this.ng = this.na = this.no_verdict = 0;
        this.crashed = this.dropped = this.other = this.total = 0;
        this.last = undefined;
        this.lastClass = undefined;
    }

    record(o: RunOutcome): string {
        const cls = outcomeClass(o);
        this.total += 1;
        this.last = o;
        this.lastClass = cls;
        switch (cls) {
            case CLASS_OK: this.ok += 1; break;
            case CLASS_NG: this.ng += 1; break;
            case CLASS_NA: this.na += 1; break;
            case CLASS_NO_VERDICT: this.no_verdict += 1; break;
            case CLASS_CRASHED: this.crashed += 1; break;
            case CLASS_DROPPED: this.dropped += 1; break;
            default: this.other += 1; break;
        }
        return cls;
    }
}
