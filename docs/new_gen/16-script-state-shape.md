# Script State After Record — the U2 Decision (`xi::kv()`)

| Field | Value |
|---|---|
| **Date** | 2026-07-03 |
| **Status** | DECIDED + LANDED BILINGUAL on `polaris2/u2-state-shape` (this branch). U2 cut-gate resolution for doc 10 §Record removal schedule; closes doc 12 row G3 / §Unscheduled U2 |
| **Scope** | The post-Record shape of cross-frame script-local state (`xi::state()` today). NOT in scope: `t.meta()`'s Record return (doc 12 names it "same family" — it is a *payload-plane read surface*, owned by the pack migration, not by state) |

## The decision in one paragraph

Cross-frame script state becomes **`xi::kv()` — a flat, typed, mutable
key-value store (`xi::Kv`)** that lives purely SDK-side (header-only,
zero-dependency beyond `xi_mp.hpp`), serializes as **one canonical max-width
msgpack map with sorted keys (byte-deterministic)**, and crosses the host
boundary through **four new length-carrying byte exports**
(`xi_script_kv_get/set/schema_version/change`) that mirror the Record state
choreography exactly (capture pre-swap → restore post-swap → schema-gate →
opt-in migrate → drop). JSON survives on this path **only until the cut and
only on the old channel**: the host converts nothing between the two channels;
a script that ports self-seeds `xi::kv()` from the restored `xi::state()`
during the bilingual window (both channels are live in the same DLL). At THE
CUT the Record channel is deleted wholesale and `xi::kv()` is simply what
remains.

## Why this shape (and what was rejected)

**Why a flat typed KV, not a pack.** Packs are sealed/immutable — the write
model of accumulating per-frame state ("read previous, compute, overwrite")
is the exact opposite. Forcing state into a pack means reseal-per-frame plus
host-side registry traffic for data that never legitimately crosses the ABI.
State is script-local by design; it deserves a plain mutable container.

**Why flat-with-nested-mp, not a mutable tree DOM.** A nested mutable
document container is Record by another name — rebuilding it without yyjson
just re-creates the thing the cut deletes. In-tree state usage (blob_tracker's
`prev_centroids`, trend_monitor's `window`, hot_reload_run2's `count`) is
"a few scalars + one rebuilt-each-frame structure". So: typed scalar slots
(`i64 / f64 / bool / str / bin`) plus an **`mp` slot** holding one complete
canonical msgpack subtree, written with `xi::mp::Writer` and read with
`xi::mp::Reader` — the exact idiom `ScriptPackBuilder::add_mp` already
established, including the same canonical gate (`xi::mp::canonicalize`,
reject-all-ext) so a script cannot store non-canonical or ext-bearing bytes.

**Why canonical msgpack, not JSON (the one-codec ruling).** Doc 07's ruling
is canonical mp everywhere on the data plane, JSON only at the human edges.
State bytes are a host-opaque machine boundary — nothing human reads them in
transit (verified: the host stores them verbatim, never parses; see
§Findings). Keeping JSON here would preserve a second codec, a yyjson
dependency, and Record's non-finite-double sentinel warts for zero benefit.
Canonical mp gets byte-determinism (sorted keys + max-width profile: equal
state ⇒ equal bytes), NaN normalization for free, and one decoder story.
Human-readable dumps remain available as a *view*, not a currency:
`Kv::debug_text()` renders JSON-ish text for logs/debugging only.

**Why new exports, not the old ones.** The design-forcing finding: the whole
Record state chain is typed `const char*` + `strlen` (`xi_script_set_state`,
`xi_script_code_change`'s `old_json`) — msgpack bytes contain NULs and cannot
ride it. Base64-over-`char*` (two stacked encodings) was rejected as dishonest.
New optional symbols with explicit lengths are additive, keep the Record path
byte-for-byte untouched, and make the bilingual window a physical fact: two
independent channels, two schema versions, one deletion at the cut.

**Why the name `xi::kv()`.** It names the shape honestly (keyed typed
values, not a document) and survives the cut without a rename — `state2()`
would demand churn later; re-using `state()` is impossible while both live.
Post-cut docs teach "cross-frame state lives in `xi::kv()`". Disambiguation:
doc 14's RED-FLAGGED `xi.kv` lib-plugin idea is an unrelated cross-instance
blackboard on the plugin capability plane — doc 14 now carries a naming note
telling any revival to pick a different door name.

## API surface (landed)

Header: `backend/include/xi/xi_kv.hpp` (include via `<xi/xi.hpp>` umbrella).

```cpp
xi::Kv& kv();                 // the store (static, script-DLL-local)
std::mutex& kv_mutex();       // same discipline as state_mutex(): lock in xi::async tasks

// container
bool set_i64 (key, int64_t);      int64_t     get_i64 (key, def=0);
bool set_f64 (key, double);       double      get_f64 (key, def=0.0);
bool set_bool(key, bool);         bool        get_bool(key, def=false);
bool set_str (key, string_view);  std::string get_str (key, def="");
bool set_bin (key, ptr, n);       const Bytes* get_bin(key);           // null if absent/other type
bool set_mp  (key, xi::mp::Writer|bytes);  const Bytes* get_mp(key);   // canonical-gated on set
bool has(key);  Kv::Type type_of(key);  bool erase(key);  void clear();
size_t size(); bool empty();  std::vector<std::string> keys();
xi::mp::Bytes serialize() const;          // canonical map, sorted keys, deterministic
bool parse(const uint8_t*, size_t);       // canonicalize-then-adopt; false = refused, store untouched
std::string debug_text() const;           // human view only — never a boundary format

// hot-reload machinery (mirrors the Record channel's)
int  kv_schema_version();  void set_kv_schema_version(int);
XI_KV_SCHEMA(N);                          // file-scope macro, static-init registration
using KvMigrateFn = std::function<std::optional<Kv>(const Kv& old, int from, int to)>;
void set_kv_migrate(KvMigrateFn);         // typed code_change: return nullopt to decline
```

Typed getters return the default on absent/wrong-type (Record's `get_*`
precedent); `type_of`/`has` give the strict path. `set_mp` refuses (returns
false) malformed / ext-bearing / duplicate-keyed / non-string-keyed bytes —
identical policy to `ScriptPackBuilder::add_mp`. `parse` adopts all-or-nothing.

## Boundary contract (the four exports)

All optional symbols; grow-and-retry convention carried over from
`xi_script_get_state` but with **byte lengths, no NUL termination**:

```c
int xi_script_kv_get(uint8_t* buf, int buflen);
    // >0 = bytes written; -N = buffer too small, need N; 0 = store EMPTY (host stores nothing)
int xi_script_kv_set(const uint8_t* bytes, int len);
    // 0 = adopted; -1 = refused (invalid bytes; store left untouched — fail-loud, no partial fill)
int xi_script_kv_schema_version(void);      // 0 = unversioned (legacy best-effort restore)
int xi_script_kv_change(const uint8_t* old_bytes, int old_len,
                        int old_schema, int new_schema,
                        uint8_t* buf, int buflen);
    // 0 = decline (host drops), >0/-N = migrated bytes with grow-and-retry
```

The thunk bodies live in `xi_kv.hpp` as `xi::detail::kv_*_thunk` inline
functions; `xi_script_support.hpp`'s exports are one-line wrappers. Test
fixtures export the *same inline bodies*, so the loader tests exercise the
real logic without force-including the whole script world.

## Host choreography (mirrors the Record legs, byte-untouched beside them)

`service_internal.hpp`: `std::string persistent_kv_bytes; int
persistent_kv_schema = 0;` (a `std::string` holds NULs fine; captured with
explicit lengths). `service_cmd_lifecycle.cpp` compile_and_load:

1. **Capture** (next to the Record capture): `get_kv` grow-and-retry off the
   OLD DLL; `0` ⇒ empty ⇒ nothing stored. Schema stamped alongside.
2. **Restore** (next to the Record restore): same drop predicate
   (`new_schema != 0 && old != new`, including the 0→N adoption case); on
   mismatch consult `migrate_kv()` (loader helper, sibling of
   `migrate_state()`); migrated ⇒ `set_kv` under the same SEH/exception
   guards; declined ⇒ drop. Events reuse the existing names with an added
   discriminator: `state_migrated` / `state_dropped` with `"store":"kv"` in
   `data` — the Record channel's events remain byte-identical (no new field),
   and after the cut the names live on unchanged.
3. **Reset**: `persistent_kv_bytes`/`_schema` reset at exactly the two
   project-boundary points that reset the Record channel (open/close) —
   `qa_param_state_isolation`'s no-leak guarantee holds for kv by the same
   mechanism.

kv state, like Record state, is **in-memory only**: it rides hot reloads, not
service restarts, and is never written to disk or project files.

## Migration story (JSON-era state)

**The host converts nothing.** Rationale: state's lifetime is already bounded
by the process (it dies on every service restart), so the worst case of a
missed carry is one counter reset on one reload — not data loss of record.
Cross-codec host logic (parse JSON it never parsed before, guess shapes) would
violate the opacity that makes this boundary safe.

- **During the bilingual window**: both channels are live in one DLL. A script
  porting from `xi::state()` to `xi::kv()` self-seeds on first inspect:

  ```cpp
  if (xi::kv().empty() && xi::state().has("count"))
      xi::kv().set_i64("count", xi::state()["count"].as_int(0));
  ```

  The host restores the Record channel exactly as before, the seed runs once,
  and from then on the kv channel carries. Proven by `test_kv_migrate`
  SECTION E (loader-level) — no new host machinery needed, which is the point.
- **Old JSON blobs are never refused** — they simply keep riding the Record
  channel until that channel is deleted.
- **At the cut**: a script still on `xi::state()` fails loud at compile
  (header gone). Its state resets once at the porting reload. Accepted.

## What THE CUT deletes (the exact edits, so the cut is mechanical)

1. `backend/include/xi/xi_state.hpp` — delete file; drop its include from
   `xi.hpp` and `xi_script_support.hpp`.
2. `xi_script_support.hpp` — delete the four Record thunks
   (`xi_script_get_state`, `xi_script_set_state`,
   `xi_script_state_schema_version`, `xi_script_code_change`).
3. `xi_script_loader.hpp` — delete `GetStateFn/SetStateFn/
   StateSchemaVersionFn/CodeChangeFn` typedefs + fields + the four
   GetProcAddress lines + `migrate_state()`.
4. `service_internal.hpp` — delete `persistent_state_json` /
   `persistent_state_schema`; `service_cmd_lifecycle.cpp` — delete the Record
   capture block and the whole Record restore/migrate/drop block (the kv
   blocks beside them remain; the `"store":"kv"` field on the events stays).
5. Tests: delete `test_state_migrate.cpp` + `migrate_probe.cpp` (+ their
   CMake stanza); `test_kv` / `test_kv_migrate` / `qa_kv_reload` are the
   surviving coverage.
6. Examples: port `blob_tracker`, `trend_monitor`, `hot_reload_run2`,
   `use_demo` (`multi_file_script`'s comment) to `xi::kv()` —
   `qa_kv_reload` is the reference port.
7. Docs: `write-a-script.md` state section becomes kv-only;
   `ws-protocol.md` state-event note keeps the names, documents `"store"`;
   `XI_STATE_SCHEMA` leaves `check_doc_coverage.py`'s macro list
   (`XI_KV_SCHEMA` already in).

## Findings surfaced by this work

- **Host opacity confirmed** (the task's verify-this claim): the host never
  parses state bytes — every touch of `persistent_state_json` is
  assign/size/c_str/reset. The one soft format assumption found:
  `size() > 2` as the "non-empty" heuristic (it encodes knowledge that empty
  Record state serializes as `"{}"`). The kv channel replaces that with an
  explicit contract: `get` returns `0` ⇒ empty ⇒ nothing captured.
- **The `const char*` boundary was the real blocker**: any "keep the old
  exports, switch bytes to mp" plan dies on embedded NULs; this is why U2
  needed a decided shape rather than a codec swap.
- **`t.meta()` returns Record** (doc 12 flags it "same family"): NOT resolved
  here — it is a payload-plane read surface and belongs to the pack
  migration's read-side work, not to script-local state. Named so the cut
  planner doesn't assume U2 covered it.

## Evidence (what proves this)

- `backend/tests/test_kv.cpp` — container unit: typed round-trips, sorted-key
  byte-determinism, canonical gate refusals (ext / dup keys / non-string
  keys / malformed), foreign-width canonicalization, NaN normalization,
  all-or-nothing parse.
- `backend/tests/test_kv_migrate.cpp` + `kv_probe.cpp` (one source, three
  DLLs, real loader + real `migrate_kv`): A round-trip through the host
  boundary bytes; B hook-present migrate on schema mismatch; C hook-absent
  decline ⇒ drop; D grow-and-retry on a >64 KiB store; E JSON-era self-seed
  (Record channel restored → script seeds kv → kv carries).
- `examples/qa_kv_reload` — live-service QA: v1 counts frames in `xi::kv()`
  through real hot reloads, v2 bumps `XI_KV_SCHEMA` + migrates via
  `set_kv_migrate`, driver asserts carry + migration + the `"store":"kv"`
  event off the live WS wire.
