// Extension <-> backend version-skew classification — TypeScript side.
//
// The backend announces itself once per connection in the `hello` event, whose
// data carries exactly three identity fields today (backend/src/service_main.cpp
// send_hello): `version` (the backend's SemVer string, e.g. "0.2.0"), `commit`
// (git sha), and `abi` (the WS *protocol* stamp — a hardcoded `2`, distinct from
// the C plugin-ABI struct version; see docs/reference/ws-protocol.md §version:
// "an informational stamp, not a live version gate"). This module turns that
// payload into an explicit compatibility verdict instead of a silent log line
// (external review 06 finding 3 / review 10 finding 3).
//
// Pure data + no `vscode` import, mirroring runOutcome.ts, so the dispatch in
// extension.ts stays a thin wrapper and this logic is unit-testable under
// `node --test`. Forward-tolerant by construction: a `hello` from an old backend
// that omits `version`/`abi` classifies as `unknown` (warn once, never block) —
// missing fields never throw.
//
// Compat model — anchored to README §"Versioning & compatibility":
//   * Packages version independently on their own SemVer tracks; the
//     known-compatible row pairs backend 0.2.x with extension 0.2.x at WS abi 2.
//   * Pre-1.0, "minor bumps may still carry breaking changes" (README), so the
//     compatibility unit is the backend MAJOR.MINOR, not just the major.
//   * The protocol "evolves additive-only" (ws-protocol.md), which makes skew
//     asymmetric: a NEWER backend is a superset an older extension mostly reads
//     (proceed with a notice); an OLDER backend lacks commands this extension
//     sends and answers them `ok:false` (a real problem → warn).
//   * The `abi` stamp is the strongest signal: if the backend reports an abi
//     that differs from the one this extension speaks, the wire contract itself
//     changed — incompatible regardless of the version string.

// The backend MAJOR.MINOR this extension release is developed and tested against
// (README known-compatible row: backend 0.2.x <-> extension 0.2.x).
export const EXPECTED_BACKEND_MAJOR = 0;
export const EXPECTED_BACKEND_MINOR = 2;

// The WS-protocol `abi` stamp this extension was built to speak. Raised to `2`
// at the ABI v11->v12 cut (the Record data plane was deleted, pack-door only),
// in lockstep with the backend's send_hello, so a mismatch becomes a real,
// catchable incompatibility.
export const EXPECTED_WS_ABI = 2;

export type SkewKind =
    | 'compatible'   // backend MAJOR.MINOR == expected, abi matches
    | 'newer'        // backend ahead of expected MAJOR.MINOR (additive superset)
    | 'older'        // backend behind expected MAJOR.MINOR (missing commands)
    | 'abi'          // backend WS abi stamp != EXPECTED_WS_ABI (wire break)
    | 'unknown';     // backend reported no parseable version (likely old backend)

// UX escalation level. `ok` = silence (the feature). `notice` = one
// output-channel line. `warn` = visible notification + persistent status chip.
export type SkewLevel = 'ok' | 'notice' | 'warn';

export interface SkewResult {
    kind: SkewKind;
    level: SkewLevel;
    backendVersion?: string;   // raw string as received (undefined if missing)
    backendAbi?: number;       // raw abi as received (undefined if missing)
    extensionVersion: string;  // the extension's own version, for messaging
    expectedBackend: string;   // human range, e.g. "0.2.x"
    expectedAbi: number;
    summary: string;           // one-line verdict naming both sides
    advice?: string;           // what to do (only for notice/warn)
}

interface SemVer { major: number; minor: number; patch: number; }

// Parse a leading `MAJOR.MINOR.PATCH` out of a version string, tolerating a
// pre-release/build suffix ("0.2.0-rc1", "0.2.0+sha"). Returns undefined for
// anything without at least a numeric major.minor.
export function parseSemVer(v: unknown): SemVer | undefined {
    if (typeof v !== 'string') return undefined;
    const m = v.trim().match(/^(\d+)\.(\d+)(?:\.(\d+))?/);
    if (!m) return undefined;
    return {
        major: Number(m[1]),
        minor: Number(m[2]),
        patch: m[3] != null ? Number(m[3]) : 0,
    };
}

function asData(ev: unknown): Record<string, unknown> {
    if (ev && typeof ev === 'object') {
        const o = ev as Record<string, unknown>;
        const d = 'data' in o ? o.data : o;
        if (d && typeof d === 'object') return d as Record<string, unknown>;
    }
    return {};
}

const EXPECTED_RANGE = `${EXPECTED_BACKEND_MAJOR}.${EXPECTED_BACKEND_MINOR}.x`;

// Classify a `hello` event (or its bare data dict) against this extension's
// declared expectations. `extensionVersion` is the extension's own package
// version, threaded in for the message (kept out of the module so the pure
// logic has no dependency on how the caller sources it).
export function classifyHello(hello: unknown, extensionVersion: string): SkewResult {
    const d = asData(hello);
    const backendVersion = typeof d.version === 'string' ? d.version : undefined;
    const backendAbi = typeof d.abi === 'number' ? d.abi : undefined;
    const base = {
        backendVersion,
        backendAbi,
        extensionVersion,
        expectedBackend: EXPECTED_RANGE,
        expectedAbi: EXPECTED_WS_ABI,
    };

    // Strongest signal first: an abi stamp that is present AND differs means the
    // wire contract itself changed. A missing abi (very old backend) is not, on
    // its own, a mismatch — the version string handles that case as `unknown`.
    if (backendAbi !== undefined && backendAbi !== EXPECTED_WS_ABI) {
        return {
            ...base,
            kind: 'abi',
            level: 'warn',
            summary: `Backend WS protocol abi ${backendAbi} differs from the abi `
                + `${EXPECTED_WS_ABI} this extension (v${extensionVersion}) speaks`
                + `${backendVersion ? ` (backend v${backendVersion})` : ''}.`,
            advice: 'The wire protocol changed. Update the extension and backend '
                + 'together to a matching release.',
        };
    }

    const sv = parseSemVer(backendVersion);
    if (!sv) {
        return {
            ...base,
            kind: 'unknown',
            level: 'notice',
            summary: 'Backend did not report a recognizable version — cannot '
                + `verify compatibility with this extension (v${extensionVersion}).`,
            advice: 'This is likely an older backend. Proceeding, but update it to '
                + `${EXPECTED_RANGE} if you hit unexpected behaviour.`,
        };
    }

    const sameMajorMinor =
        sv.major === EXPECTED_BACKEND_MAJOR && sv.minor === EXPECTED_BACKEND_MINOR;
    if (sameMajorMinor) {
        return {
            ...base,
            kind: 'compatible',
            level: 'ok',
            summary: `Backend v${backendVersion} (abi ${backendAbi ?? EXPECTED_WS_ABI}) `
                + `is compatible with this extension (v${extensionVersion}).`,
        };
    }

    const backendAhead =
        sv.major > EXPECTED_BACKEND_MAJOR ||
        (sv.major === EXPECTED_BACKEND_MAJOR && sv.minor > EXPECTED_BACKEND_MINOR);
    if (backendAhead) {
        return {
            ...base,
            kind: 'newer',
            level: 'notice',
            summary: `Backend v${backendVersion} is newer than the ${EXPECTED_RANGE} `
                + `this extension (v${extensionVersion}) was built against.`,
            advice: 'The protocol is additive, so this should work; newer backend '
                + 'features may not be surfaced. Update the extension when convenient.',
        };
    }

    // Backend is behind the expected MAJOR.MINOR — the asymmetric bad case:
    // commands this extension sends may be answered `ok:false`.
    return {
        ...base,
        kind: 'older',
        level: 'warn',
        summary: `Backend v${backendVersion} is older than the ${EXPECTED_RANGE} `
            + `this extension (v${extensionVersion}) requires.`,
        advice: `Update the backend to ${EXPECTED_RANGE}, or install the matching `
            + 'older extension. Commands this extension sends may be rejected.',
    };
}
