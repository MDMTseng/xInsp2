# IPC + SHM (cross-process isolation)

> **Status: shipping; opt-in.** The full mesh (worker / runner / SHM /
> respawn) is merged from `shm-process-isolation` and exercised by 9
> backend tests. Activation is per-instance: set
> `"isolation": "process"` in `instance.json`, or call
> `cmd:script_isolated_run` for the script side. Multi-image
> input/output is symmetric (was first-image-only). Default-on is
> tracked work — fail-loud worker-dead policy and heap-pool
> input-direction auto-convert still missing.

---

## Why

Same-process plugins are protected by `_set_se_translator`: a segfault
in `process()` becomes a recoverable C++ exception. But several
failure modes leak through the in-proc model:

- Stack overflow (OS guard fault, not catchable).
- Heap corruption (silent, may surface unrelated to the cause).
- Hung `process()` (watchdog kills inspect but the plugin's threads
  may be in unknown state).
- Third-party plugins you don't fully trust.

An instance can opt in via `instance.json: "isolation": "process"`:
the plugin runs in a separate `xinsp-worker.exe`. User scripts have
the analogous `xinsp-script-runner.exe` available via
`cmd:script_isolated_run`.

Pixel data still flows zero-copy via shared memory (`CreateFileMapping`
+ refcount in mapped pages); only handles + small JSON ride the
named-pipe RPC.

---

## Components

```
                 ┌───────────────────────────────────────┐
                 │         Shared Memory region          │
                 │   xinsp2-shm-<backend-pid>            │
                 │   (mmap'd by every attaching proc)    │
                 │                                       │
                 │   [hdr | block hdr | pixels]          │
                 │         [block hdr | pixels]          │
                 │         ...                           │
                 └────▲──────────────▲──────────────▲────┘
                      │              │              │
                      │ host->image_*│              │ same in worker /
                      │ resolves     │              │ runner processes
                      │ via singleton│              │
                ┌─────┴──┐    ┌──────┴───────┐    ┌─┴──────────────┐
                │backend │    │xinsp-worker  │    │xinsp-script-   │
                │        │    │.exe          │    │runner.exe      │
                │        │    │ (1 plugin)   │    │ (1 script)     │
                │        │◄──►│ named pipe   │    │                │
                │        │    │ RPC          │    │                │
                │        │◄──►│              │    │ named pipe     │
                │        │    └──────────────┘    │ RPC (bidir)    │
                │        │                        │                │
                │        │◄══════════════════════►│                │
                │        │   Session::Handler     │                │
                │        │   routes use_*         │                │
                │        │   callbacks back here  │                │
                └────────┘                        └────────────────┘
```

Three new artifacts:
- `xinsp-worker.exe` — hosts ONE plugin in a separate process.
- `xinsp-script-runner.exe` — hosts ONE inspection script.
- SHM region — created by the backend at startup, attached by both
  worker types.

---

## Code map

| File | Role |
|---|---|
| `backend/include/xi/xi_shm.hpp` | `ShmRegion` — `CreateFileMapping` wrapper, bump allocator, cross-process atomic refcount. Handles are 8-bit tag + 56-bit offset. |
| `backend/include/xi/xi_ipc.hpp` | `Pipe` (named-pipe RAII) + frame format + `Session` (bidirectional RPC mux) |
| `backend/include/xi/xi_process_instance.hpp` | `ProcessInstanceAdapter` — `InstanceBase` impl that proxies methods to a worker over the pipe |
| `backend/include/xi/xi_script_process_adapter.hpp` | `ScriptProcessAdapter` — same shape for the script runner |
| `backend/src/worker_main.cpp` | `xinsp-worker.exe` entry |
| `backend/src/script_runner_main.cpp` | `xinsp-script-runner.exe` entry |

---

## SHM region

```cpp
auto rgn = xi::ShmRegion::create("xinsp2-shm-<pid>", 512 << 20);  // 512 MB
xi::ImagePool::set_shm_region(&rgn);                              // wire singleton

uint64_t h = rgn.alloc_image(640, 480, 1);   // top byte = 0xA5 region tag
uint8_t* px = rgn.data(h);                    // pointer in this proc's mapping
rgn.addref(h); rgn.release(h);                // cross-proc atomic
```

- One region per backend session, named after the backend PID so
  attaching workers can find it.
- Bump allocator: never reclaims (deliberate; production would add a
  free-list / size-class arena).
- Handles validate via tag + magic + bounds before deref → bogus
  handles return null safely.
- Refcount is `std::atomic<int32_t>` on a cache-line-aligned header;
  Windows + x86-64 honour atomic across processes mapping the same
  page.

`host_api` extensions (binary-compatible append):

```c
xi_image_handle (*shm_create_image)(int32_t w, int32_t h, int32_t channels);
xi_image_handle (*shm_alloc_buffer)(int32_t size_bytes);
void            (*shm_addref)(xi_image_handle h);
void            (*shm_release)(xi_image_handle h);
int32_t         (*shm_is_shm_handle)(xi_image_handle h);
```

`image_data` / `image_addref` / `image_release` / etc. **dispatch by
the handle's top byte**: `0xA5` → SHM region, otherwise heap pool.
Plugins can mix freely (allocate one image via `image_create`, another
via `shm_create_image`, hand both to the same `xi::Record`).

---

## Named pipe IPC

Frame format (16-byte header + payload):

```
[ magic(u32)='XIPI' | seq(u32) | type(u32) | len(u32) ][ payload(len) ]
```

- `seq` — incrementing per request; reply echoes it.
- `type` — `RpcType` enum (8=USE_PROCESS, etc.); high bit set in
  reply.
- `len` — payload bytes, capped at 16 MB to bound memory.

Reader / Writer helpers handle binary-packed payloads (image handles
as raw `u64`, JSON as length-prefixed bytes).

`Session` adds bidirectional RPC: while `call()` waits for its reply,
incoming requests from the other side are dispatched through a
registered handler. Used by the script runner — when the script calls
`xi::use(...).process(...)`, the runner sends `RPC_USE_PROCESS` to the
backend mid-`RPC_SCRIPT_RUN` reply-await.

RPC types covered:

| Type | Initiator | Purpose |
|---|---|---|
| `CREATE` / `DESTROY` | backend → worker | plugin lifecycle |
| `PROCESS` / `EXCHANGE` / `GET_DEF` / `SET_DEF` | backend → worker | proxy plugin methods |
| `SCRIPT_RUN` | backend → script-runner | reset + inspect_entry + snapshot |
| `USE_PROCESS` / `USE_EXCHANGE` / `USE_GRAB` | script-runner → backend | mid-call callbacks back to backend's instance registry |
| `TEST_SET_INPUT` | tests only | drive a test handle into the script |
| `EMIT_TRIGGER` | worker → backend, **one-way** (`seq=0`, no reply) | source plugins running in isolated workers push `host->emit_trigger` calls back to the backend's `TriggerBus`. Image handles are SHM-tagged before sending (worker promotes any heap-pool handles via `shm_create_image` first). |

---

## Adapter behaviour

`ProcessInstanceAdapter`:
- Spawns `xinsp-worker.exe` with `--pipe`, `--shm`, `--plugin-dll`,
  `--instance` args.
- Caches the `set_def` JSON last accepted; replayed on respawn so the
  fresh worker isn't blank.
- **Always-on reader thread** owns the pipe's read side. Replies for
  in-flight `do_call_locked_()` calls fulfil a per-seq
  `std::promise<Frame>`; `seq=0` async frames (currently only
  `RPC_EMIT_TRIGGER` from source plugins) dispatch straight to
  `TriggerBus::instance().emit()` regardless of whether any RPC is
  in flight. This is what lets a process-isolated source plugin
  emit triggers continuously when the backend is silent (Task #74 /
  PR #19 follow-up). See the comment block above
  `start_reader_()` in `xi_process_instance.hpp` for the
  broken-pipe history that motivated this design.
- Pipe handles use `FILE_FLAG_OVERLAPPED` with synchronous wrappers
  (`OVERLAPPED` + `WaitForSingleObject`) inside `Pipe::read_exact` /
  `Pipe::write_all`. Synchronous I/O on a non-overlapped handle
  serialises concurrent R+W in the kernel I/O manager — fatal for
  the reader-thread design — but every caller still sees plain
  blocking semantics.
- Per-call timeout: `do_call_locked_()` `wait_for`s the promise. On
  timeout it pops the inflight entry and `CancelIoEx`'s the pipe
  handle; the cancelled `ReadFile` aborts the reader, the throw
  surfaces, `call_()` catches and respawns. `call_timeout_ms`
  defaults to 30s.
- Auto-respawn on pipe failure: stop reader, tear down old, spawn
  new (with `-r<count>` pipe name suffix to avoid collision), start
  fresh reader, re-CREATE, reapply saved_def, retry once. Capped at
  3 respawns per 60s rolling window — over the cap the adapter
  goes dead and returns safe defaults.

`ScriptProcessAdapter`:
- Same lifecycle (spawn / accept / CREATE-equivalent / RPC).
- `set_handler` cached so the freshly spawned `Session` re-installs
  it (otherwise the script's use_*/exchange/grab callbacks would have
  no destination after a respawn).
- Same 3-respawn-per-60s cap.

---

## Test surface

On the spike branch:

```
test_xi_shm                  cross-process refcount + byte-match parent/child
test_worker                  end-to-end zero-copy via xinsp-worker.exe
test_isolated_instance       open_project with isolation:"process" instance
test_worker_respawn          kill from outside → next call succeeds
test_worker_timeout          hung plugin → CancelIoEx + respawn cap
test_script_runner           xinsp-script-runner.exe + bidir RPC
test_script_process_adapter  ScriptProcessAdapter lifecycle + handler
test_script_runner_respawn   script runner auto-respawn
test_shm_ipc_edges           DoS / OOM / bogus handle / attach error edges
```

All 9 green at branch tip.

Plus end-to-end driver coverage in `examples/`:

```
examples/cross_proc_trigger/    isolated source emits at 60 Hz; driver
                                  observes a 1-s silent window with no
                                  backend->worker RPCs and asserts >=50
                                  triggers reach the bus (proves the
                                  always-on reader is live)
examples/multi_source_surge/    3 isolated/in-proc sources + 2 detectors
                                  + 2 surges — exercises the full mesh
examples/burst_pipeline/        bursty source + parallel dispatch
```

---

## Open questions before merge to master

- **Default isolation policy.** Currently opt-in per instance. Should
  be configurable per project? Per machine?
- **Operator UX for "worker dead".** When the rate limit caps, the
  adapter goes silent — the script still calls into it but gets safe
  defaults. Should we surface a more obvious "your plugin is broken,
  fix it" toast? Crash report integration?
- **SHM size policy.** 512 MB hardcoded. Should be projects'
  responsibility to tune based on max image footprint.
- **Script isolation default.** `cmd:script_isolated_run` is the only
  current entry point; `cmd:run` still uses the in-proc path. Plumbing
  the existing run path to optionally use `ScriptProcessAdapter` is
  ~50 lines but interacts with history / triggers / breakpoints in
  ways that need a dedicated review.

---

## See also

- [`status.md`](../status.md) — current spike state.
- [`guides/debugging.md`](../guides/debugging.md) — the in-proc crash
  classes that motivated this spike.
- `backend/tests/test_xi_shm.cpp` — the simplest end-to-end demo of
  the SHM model.
