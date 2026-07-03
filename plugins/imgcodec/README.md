# xi.imgcodec — the first official lib plugin (capability plane pilot)

This is a **lib plugin** (docs/new_gen/14): a capability provider, **not** a
pipeline stage. It has **no data plane** — it never emits, nothing routes to
it, and it publishes no pack door. On create it registers two capabilities
with the host; consumers never see this DLL or its vtable — they resolve
`host->get_interface("xi.cap", 1)` and **call by capability name** through the
host forwarding funnel (SEH-gated, fault charged to *this* instance, per-thread
reentrancy refused with -5).

## Capabilities

Registry is **name-only**; versioning rides *inside* the request pack:
`$v` (i64, default 1), `$probe: true` (bool) answers `$versions` with no work,
an unsupported `$v` answers a normal sealed `$fault` pack naming the range.

### `xi.jpeg.encode` (versions: 1)

| direction | entries |
|---|---|
| in | image `image` (1/3/4-channel 8-bit), i64 `quality` (optional, 1..100, default = config `quality`), `$v`/`$probe` |
| out | bin `jpeg`, i64 `cache_hit` (1 = served from cache), i64 `encodes` (lifetime encode count), i64 `hits` |

**Dedup memo cache** — keyed by the image's *content* identity (FNV-1a over
pixels + dims; sealed-pack pool-handle identity is not exposed across the ABI
pre-v12) + quality. The same sealed image requested by N consumers is encoded
**once**; everyone gets byte-identical bytes. `exchange {"command":"stats"}`
exposes `encodes` vs `hits` so tests can *prove* dedup.

### `xi.image.decode` (versions: 1)

| direction | entries |
|---|---|
| in | bin `data` — PNG / JPEG / BMP / TGA / GIF / PSD / HDR / PIC (stb_image) |
| out | image `image`, i64 `w` / `h` / `c` |

This mirrors `host_api->read_image_file`'s format set: the decode capability is
that host field's **designated v12 eviction target** (net-zero ABI bill). The
host field itself is untouched until the authorized break.

## Encoder choice

stb_image_write (vendored, compiled in via `backend/src/stb_impl.cpp` — the
record_save pattern). Deterministic bytes, zero external dependencies.
libjpeg-turbo is *not* actually deployed beside the backend today
(`XINSP2_HAS_TURBOJPEG` is an opt-in cmake switch expecting an external
install); swapping this plugin's encoder later is invisible to consumers —
that is the point of the capability boundary.

## Contracts

- **Thread-safe by contract**: the funnel does not serialize handler calls.
- **Fail-loud**: missing/mis-typed inputs answer a sealed `$fault` pack
  (`missing_input` / `wrong_type` / `encode_failed` / `decode_failed` /
  `unsupported_version`); `XI_PACK_NULL` only on hard internal failure.
- **`on_fault: refuse`**: a faulted codec quarantines (consumers see -3)
  rather than serving from possibly-corrupt state; re-commit config to
  re-enable.
- Unregisters on destroy; the host's owner sweep backstops it regardless.

## plugin.json marker

`"lib": true` is informational (pre-v12): create **one** instance; don't wire
it into any pipeline. Tests: `plugins/build` ctest `cap_imgcodec_test` (dedup +
round-trip against the real DLL) and `examples/qa_cap_imgcodec` (live service).
