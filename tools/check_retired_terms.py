#!/usr/bin/env python3
"""check_retired_terms.py — a freeze guard for RETIRED API vocabulary in docs.

WHY THIS EXISTS
---------------
The developer-facing surface keeps copy-paste examples: guides, reference pages,
the README, and the GENERATED-PROJECT-TEMPLATE the VS Code extension emits for a
new project. Readers copy those verbatim. When the SDK RETIRES a concept — a
macro deleted from the headers, an entry signature demoted to a compat fallback —
the examples do NOT stop compiling loudly; they just teach an API that no longer
exists. That is exactly how a generated `inspect.cpp` template kept teaching the
removed `VAR`/`EMIT` macros and the legacy `void xi_inspect_entry(int)` entry long
after the headers moved to `XI_INSPECT_ENTRY(t, frame)` — nothing FAILED, so only
manual review caught it.

This is the analogue of check_doc_coverage, inverted: doc_coverage asserts every
LIVE public symbol IS taught; this asserts no RETIRED token is taught on a BLESSED
copy-paste surface. Each enforced term is validated below against the actual SDK
headers (backend/include/xi) so the list cannot silently go stale — a term is only
enforced while it is genuinely gone / demoted; if a term comes back as a live
macro the guard tells you to stop enforcing it.

A legitimate mention of a retired term (a compatibility appendix that documents
the legacy path ON PURPOSE, an archived page, or runtime logic in the extension
that PARSES user `VAR(...)` tokens for a graph view) is silenced by an entry in
tools/.retired-terms-allow — an EXPLICIT, reasoned opt-out, never a silent skip.

Run standalone (exits non-zero on a hit):  python tools/check_retired_terms.py
Optional repo-root override:               python tools/check_retired_terms.py <root>
Also wired as the `retired_terms` ctest.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
XI_INC = ROOT / "backend" / "include" / "xi"
# THE CUT (v12) retired five WS commands. Headers say nothing about the WS
# command surface — the dispatch truth is service_main.cpp's g_cmd_table — so
# the validator reads that file too (commands only; API terms stay header-checked).
SERVICE_MAIN = ROOT / "backend" / "src" / "service_main.cpp"
ALLOW_FILE = ROOT / "tools" / ".retired-terms-allow"

# --- retired vocabulary -----------------------------------------------------
#
# Each entry: token-key -> (compiled regex, human "why retired").  The regex is
# what we grep for on a blessed surface; `validate` (below) re-checks the CLAIM
# against the headers so an enforced term can't silently un-retire.
#
# ENFORCED (verified genuinely retired against backend/include/xi):
#   VAR(            — the VAR macro is GONE from the headers (no #define VAR).
#                     Docs/STUDY call it a "removed no-op"; a copied VAR(x, expr)
#                     no longer publishes anything and the macro itself is absent.
#   EMIT(           — same: no #define EMIT anywhere in the SDK headers.
#   xi_inspect_entry(int
#                   — the LEGACY free-function entry. The current entry is the
#                     XI_INSPECT_ENTRY(t, frame) macro (xi_use.hpp), which exports
#                     xi_inspect_entry_tv(const xi_trigger_view*, int). The bare
#                     `void xi_inspect_entry(int)` form is the demoted compat
#                     fallback — not what a new script should be taught to type.
#
# REJECTED (checked and NOT retired — deliberately NOT enforced):
#   XI_SCRIPT_EXPORT   — still #define'd in xi_script.hpp and actively emitted by
#                        the live XI_INSPECT_ENTRY macro expansion + used across
#                        xi_script_support.hpp. It is current SDK plumbing.
#   xi::current_trigger( — still a real callable (inline Trigger current_trigger()
#                        in xi_use.hpp). It is the LEGACY-v1 ambient path, but it
#                        exists and is documented as a live (if not preferred) API,
#                        so flagging it would be wrong.
RETIRED = {
    "VAR(":  (re.compile(r"\bVAR\s*\("),  "the VAR macro was removed from the SDK headers (no #define VAR); a copied VAR(...) no longer compiles/publishes"),
    "EMIT(": (re.compile(r"\bEMIT\s*\("), "the EMIT macro was removed from the SDK headers (no #define EMIT)"),
    "xi_inspect_entry(int": (re.compile(r"\bxi_inspect_entry\s*\(\s*int"), "legacy free-function entry; the current entry is the XI_INSPECT_ENTRY(t, frame) macro"),
    # --- THE CUT (v12) — the Record data plane and its satellites ------------
    "xi::Record": (re.compile(r"\bxi::Record\b"), "the Record data plane was deleted at THE CUT (v12); scripts read t.pack() (ScriptPack) and build with xi::ScriptPackBuilder, plugins speak the xi.pack@1 door (PackIn/PackOut)"),
    "xi::state(": (re.compile(r"xi::state\s*\("), "xi::state() was deleted at THE CUT (v12, docs/new_gen/16); xi::kv() is the only cross-frame state channel"),
    "xi::imread(": (re.compile(r"xi::imread\s*\("), "xi::imread was deleted with the read_image_file host slot (v12); decode is the xi.image.decode capability (imgcodec provider)"),
    "emit_record": (re.compile(r"\bemit_record\b"), "the emit_record dispatch verb was deleted at v12; a source seals a pack and emits it via xi_pack_v1::emit_pack (SDK: Plugin::new_pack() + emit(std::move(pack)))"),
    "t.meta(": (re.compile(r"\bt\.meta\s*\("), "the Record meta plane (t.meta()) was deleted at v12; event metadata rides the pack as ordinary entries (t.pack().get_*)"),
}

# THE CUT (v12) also retired five WS commands (docs/new_gen/06 §1.7 — all five
# confirmed by the app team, removed from g_cmd_table). On a blessed surface we
# flag the COMMAND-token forms a reader would copy into a client (`cmd`,
# "quoted", `backticked`) — bare prose like "the watchdog status" is fine, and
# an INTERNALS mention of e.g. xi::script::unload_script() never carries the
# marker prefix, so it cannot false-hit even if internals ever joined the scan.
RETIRED_WS_COMMANDS = ("watchdog_status", "unload_script", "unquarantine_plugin",
                       "load_plugin", "get_project")
for _c in RETIRED_WS_COMMANDS:
    RETIRED[f"cmd:{_c}"] = (
        re.compile(r'(?:cmd:\s*|["`])' + _c + r"\b"),
        f'the WS command "{_c}" was retired at THE CUT (v12); it is gone from the g_cmd_table dispatch table',
    )

# Validation: (token-key, predicate(headers_text) -> is_still_retired). If a
# predicate returns False the token has come BACK as live surface and the guard
# refuses to run until the RETIRED table is corrected — the mirror of
# doc_coverage's "stale curated macro" check.
def _validate_retired(headers_text: str, service_text: str) -> list[str]:
    problems: list[str] = []
    if re.search(r"#\s*define\s+VAR\b", headers_text):
        problems.append("VAR( — a #define VAR now exists in the headers; VAR is live again, stop enforcing it")
    if re.search(r"#\s*define\s+EMIT\b", headers_text):
        problems.append("EMIT( — a #define EMIT now exists in the headers; EMIT is live again, stop enforcing it")
    # The current macro must still be XI_INSPECT_ENTRY for the legacy signature to
    # be the "retired" one; if that macro vanished our claim is stale.
    if "#define XI_INSPECT_ENTRY" not in headers_text:
        problems.append("xi_inspect_entry(int — XI_INSPECT_ENTRY macro no longer defined; re-check what the current entry is")

    # --- THE CUT (v12) claims. Each predicate is STRUCTURAL (a declaration /
    # definition / dispatch-table shape), never a bare substring, because the
    # headers deliberately keep tombstone COMMENTS naming the deleted APIs.
    if re.search(r"\bclass\s+Record\b", headers_text):
        problems.append("xi::Record — a `class Record` is defined in the headers again; the Record plane is live, stop enforcing it")
    if (XI_INC / "xi_state.hpp").exists():
        problems.append("xi::state( — backend/include/xi/xi_state.hpp exists again; the state channel is back, stop enforcing it")
    if re.search(r"\bImage\s+imread\s*\(", headers_text):
        problems.append("xi::imread( — an `Image imread(...)` definition is back in the headers; stop enforcing it")
    if re.search(r"\(\s*\*\s*emit_record\s*\)", headers_text) or \
       re.search(r"\binline\s+\S+\s+emit_record\s*\(", headers_text):
        problems.append("emit_record — an emit_record host slot / SDK helper is declared in the headers again; stop enforcing it")
    if re.search(r"\busing\s+TriggerMetaFn\b", headers_text) or \
       re.search(r"\bmeta\s*\(\s*\)\s*const", headers_text):
        problems.append("t.meta( — a meta() accessor / TriggerMetaFn is declared in the headers again; stop enforcing it")

    # The five retired WS commands: validated against the ACTUAL dispatch table
    # (g_cmd_table in service_main.cpp), the same source of truth
    # check_doc_coverage.py extracts the live command set from.
    m = re.search(r"g_cmd_table\s*=\s*\{", service_text)
    if not m:
        problems.append("cmd:* — could not find `g_cmd_table = {` in backend/src/service_main.cpp; "
                        "the retired-command claims can no longer be validated — update "
                        "_validate_retired() to match the new dispatch-table shape")
    else:
        body = service_text[m.end():]
        end = body.find("};")
        if end != -1:
            body = body[:end]
        live_cmds = set(re.findall(r'\{\s*"([a-z_]+)"\s*,', body))
        for c in RETIRED_WS_COMMANDS:
            if c in live_cmds:
                problems.append(f'cmd:{c} — the command is back in g_cmd_table; it is live again, stop enforcing it')
    return problems

# --- blessed surfaces -------------------------------------------------------
#
# Files/globs whose CONTENT readers copy. A retired token here is a copy-paste
# hazard. We scan every .md under docs/guides + docs/reference, plus the two
# top-level onboarding pages, plus the extension source (relying on the allow-list
# to exclude the VAR-chip graph-parser region — runtime logic, not an example).
def blessed_files() -> list[Path]:
    out: list[Path] = []
    for d in (ROOT / "docs" / "guides", ROOT / "docs" / "reference"):
        out.extend(sorted(d.rglob("*.md")))
    for f in (ROOT / "docs" / "overview.md", ROOT / "README.md",
              ROOT / "vscode-extension" / "src" / "extension.ts"):
        if f.exists():
            out.append(f)
    return out

# --- allow-list -------------------------------------------------------------
#
# Format (mirrors docs/.doc-coverage-allow's `key: reason`):
#   <relpath>[::<substr>]: reason
# A bare <relpath> silences the whole file. <relpath>::<substr> silences only
# hits on lines CONTAINING <substr> (specific, not a blanket skip). Reason
# required — it is the point.
class AllowRule:
    __slots__ = ("path", "substr", "reason", "raw", "used")
    def __init__(self, path: str, substr: str | None, reason: str, raw: str):
        self.path, self.substr, self.reason, self.raw = path, substr, reason, raw
        self.used = False

def load_allow() -> list[AllowRule]:
    rules: list[AllowRule] = []
    if not ALLOW_FILE.exists():
        return rules
    for line in ALLOW_FILE.read_text(encoding="utf-8", errors="replace").splitlines():
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        # Split key from reason on the FIRST ": " (colon-space). The KEY may
        # itself contain "::" (path::substr) and single ":" chars, so we cannot
        # partition on a bare ":"; the reason is always introduced by ": ".
        key, sep_r, reason = s.partition(": ")
        if not sep_r:  # tolerate a trailing-colon form "<key>:" with empty reason
            key, _, reason = s.partition(":")
        key = key.strip()
        path, sep, substr = key.partition("::")
        rules.append(AllowRule(path.strip().replace("\\", "/"),
                               substr.strip() if sep else None,
                               reason.strip(), s))
    return rules

def allowed(rules: list[AllowRule], rel: str, line_text: str) -> bool:
    hit = False
    for r in rules:
        if r.path != rel:
            continue
        if r.substr is None or r.substr in line_text:
            r.used = True
            hit = True
    return hit

# --- scan -------------------------------------------------------------------

def main() -> int:
    # Flagged doc lines are quoted verbatim and often carry non-ASCII prose; on
    # a narrow Windows console codepage (e.g. cp950) that must degrade to
    # replacement characters, not a UnicodeEncodeError that masks the report.
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    # *.hpp + *.h: the C ABI header (xi_abi.h) carries the host slots, so a
    # resurrected (*emit_record) field must be visible to the validator.
    headers_text = "\n".join(
        f.read_text(encoding="utf-8", errors="replace")
        for pat in ("*.hpp", "*.h") for f in sorted(XI_INC.rglob(pat))
    )
    service_text = (SERVICE_MAIN.read_text(encoding="utf-8", errors="replace")
                    if SERVICE_MAIN.exists() else "")
    stale_claims = _validate_retired(headers_text, service_text)

    rules = load_allow()
    files = blessed_files()

    hits: list[tuple[str, int, str, str, str]] = []  # (rel, lineno, term, why, line)
    for f in files:
        rel = f.relative_to(ROOT).as_posix()
        for i, line in enumerate(f.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            for term, (rx, why) in RETIRED.items():
                if rx.search(line) and not allowed(rules, rel, line):
                    hits.append((rel, i, term, why, line.strip()))

    stale_allow = [r.raw for r in rules if not r.used]

    print(f"[retired-terms] scanned {len(files)} blessed file(s) for "
          f"{len(RETIRED)} retired term(s): {', '.join(RETIRED)}")
    print(f"[retired-terms] {len(rules)} allow-list rule(s)")

    if stale_claims:
        print("\nSTALE retired-term claim(s) (a term this guard enforces is LIVE "
              "again in the headers — fix RETIRED in check_retired_terms.py):")
        for p in stale_claims:
            print(f"  - {p}")

    if stale_allow:
        print("\nSTALE allow-list rule(s) (matched nothing this run — remove them "
              "from tools/.retired-terms-allow):")
        for r in stale_allow:
            print(f"  - {r}")

    if hits:
        print(f"\nRETIRED terms on blessed copy-paste surfaces ({len(hits)}): each "
              "must be rewritten to the current API,")
        print("or, if it is an intentional legacy/archive/runtime mention, added to "
              "tools/.retired-terms-allow with a reason.\n")
        for rel, ln, term, why, text in hits:
            print(f"  {rel}:{ln}: `{term}` — {why}")
            print(f"      | {text}")
        print()

    if hits or stale_claims or stale_allow:
        return 1
    print("[retired-terms] OK — no retired API vocabulary on blessed surfaces.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
