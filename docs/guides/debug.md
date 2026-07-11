# Debugging guide

The most common failure modes in xInsp2 and how to chase them down. The
backend is built to keep running through plugin / script crashes, so
"the backend died" is news worth investigating.

---

## Levels of crash isolation

```
   user script DLL                     plugin DLL
   ────────────────                    ─────────────
   xi_inspect_entry( )                 process( )
        │                                   │
        │ SEH translator                    │ SEH translator
        │ → seh_exception                    │ → seh_exception
        │                                   │
        ▼                                   ▼
   ────────── catch in service_main ────────────
        │
        ▼
   error reply / log / crash report (still running!)
```

What's caught:
- Null pointer deref (`ACCESS_VIOLATION`)
- Integer divide by zero
- Array bounds exceeded (when triggered by `_RTC*` checks)
- Illegal instruction
- C++ `throw` from any layer

What's NOT caught:
- **Stack overflow** — the OS guard page faults; backend dies.
- **Heap corruption** — write past buffer end may corrupt host memory
  silently; eventually crashes somewhere unrelated.
- **Plugin worker thread without `xi::spawn_worker`** — translator is
  per-thread; a raw `std::thread` segfault is uncaught.

---

## When the script crashes

### Symptom: `cmd:run` returns `error: script crashed: ACCESS_VIOLATION`

1. Check the **Output panel** in VS Code for the full error line:
   ```
   [xinsp2] inspect crashed: 0xC0000005 (ACCESS_VIOLATION)
   ```
2. Open the file → look for unguarded dereferences or out-of-range
   indexing on the line you most recently changed.
3. If it's reproducible: attach the VS Code C++ debugger to
   `xinsp-backend.exe` and set a native breakpoint on the suspect line.

### Symptom: backend just disappears

Most likely **stack overflow** (unbounded recursion in the script) or
**heap corruption**. SEH won't catch these.

- Check the auto-respawn — extension restarts the backend within ~2s.
- Check `crash_reports/` for a JSON report + minidump:
  - `crash_reports/<timestamp>.json` — exception code, faulting module,
    and a `context` block (the faulting thread's `last_cmd` /
    `last_script` / `last_instance` / `last_plugin` / `last_phase` /
    `last_run_id` / `last_frame`).
  - A `threads` array lists EVERY dispatch thread's breadcrumb at
    crash time (not just the faulting one), each tagged
    `faulting: true|false`. Under `dispatch_threads > 1` this tells
    you which of the N concurrent inspects actually faulted and what
    the others were doing — the single-global breadcrumb used to
    blame whichever thread wrote last.
  - `last_phase` ∈ {`reset`, `inspect`, `done`} pinpoints the inspect
    lifecycle stage.
  - `crash_reports/<timestamp>.dmp` — minidump for WinDbg / VS.
    Captured with DataSegs + ThreadInfo + IndirectlyReferenced +
    UnloadedModules so the dump is self-contained (globals like the
    breadcrumb table and `recent_errors` ring are inside it; a
    just-FreeLibrary'd plugin still appears in the module list).

**Symbolicating the script frame.** Inspect scripts compile with
`/O2 /Z7` + linker `/DEBUG`, emitting a versioned `inspect_vN.pdb`
beside the DLL in `%TEMP%/xinsp2/script_build/`. WinDbg / VS resolve
the script's stack frames to file:line as long as that PDB is still
present (the `_vN` retention keeps the PDB paired with the live DLL).

The extension toasts a "Backend recovered after crash in `<module>`"
message naming the offending DLL.

### Recovering state

After a backend crash + auto-respawn:
- The extension calls `cmd:open_project` with `lastProjectFolder` to
  reload your project.
- `xi::kv()` state is **in-memory only** (by design — doc 16): the host
  carries it across hot reloads, but a backend crash + respawn starts it
  empty. A script that needs crash-durable values must persist them
  itself (e.g. a file under its project folder) at safe checkpoints.

---

## When a plugin crashes

### Symptom: `use_process('det0') crashed: 0xC0000005`

- Open the plugin's source. Check the bounds on inputs you read from
  the pack door's `xi::PackIn` (often a missing image entry or wrong
  dimensions — prefer the optional-returning reads and `fault()` over
  assuming a key is present).
- The host doesn't unload the plugin on crash — you can keep retrying
  after a fix and `cmd:rescan_plugins` reloads it.

### Symptom: plugin's `process()` runs forever (UI freezes)

The host's **watchdog** is **off by default** (`watchdog_ms` = 0). Enable it with
`--watchdog=MS`, `cmd:set_watchdog_ms`, or per-project. It has one phase:
overrun → 1 s grace → hard exit. A frame that returns during the grace yields a
normal verdict; if the inspect is still wedged after the grace, the backend
**exits** (`_Exit`) for the FE supervisor to respawn — it does not kill the
worker thread, and there is no cooperative soft-cancel phase.

Fix by:
- Adding loop bounds in your plugin / op.
- Bumping the watchdog if the operation legitimately takes longer.

---

## When the build fails

`cmd:compile_and_load` errors flow into the **Problems panel** as
proper VS Code diagnostics. Each `cl.exe` / `link.exe` line gets parsed
into `{ file, line, col, severity, code, message }` (see
`xi::script::parse_diagnostics`).

If the Problems panel doesn't update:
1. Make sure the file is in-buffer (not just modified on disk). The
   extension hooks `onDidSaveTextDocument`; an external edit + save
   has to come through the editor.
2. Check the Output panel for the raw cl.exe text.
3. `Ctrl+Shift+P` → **xInsp2: Compile Script** to force a manual
   rebuild.

---

## When tests fail

See [`docs/testing.md`](../testing.md) for the full test surface.

Useful patterns when a test is unhappy:

- **C++ unit test failed** → run the binary directly:
  `backend\build\Release\test_<name>.exe` — first-failing assertion's
  file/line is on stderr.
- **Node integration test failed** → `node --test
  vscode-extension\test\<name>.test.mjs` — same; tests write a backend
  log to `vscode-extension\test\<name>.log`.
- **E2E test failed** → re-run the launcher, check
  `screenshot/journey_*.png` for the UI state at each step.

---

## Useful diagnostics commands

| Command | Purpose |
|---|---|
| `cmd:crash_reports` | List recent crash reports as JSON |
| `cmd:clear_crash_reports` | Wipe the crash report directory |
| `cmd:set_watchdog_ms` | Change watchdog timeout at runtime — the reply echoes the `{ms, trips}` snapshot |
| Status bar at bottom-left | Backend connection state + WORK indicator |

In the extension's command palette:

- **xInsp2: Restart Backend** — manual respawn if auto-respawn rate
  limit was hit.
- **xInsp2: Rescan Plugins** — re-discover plugins after fixing a bad
  build / missing DLL.

Instances skipped on `open_project` (bad plugin / missing DLL / etc.) surface
as a `warn` log in the Output panel (the backend also answers the
`open_project_warnings` command with the same list).

---

## Attaching a debugger

The script + plugin DLLs are compiled with `/Zi` (PDBs alongside the
DLL). To break in your code:

1. Open the relevant `.cpp` in VS Code.
2. **Run and Debug** → **C++ Attach** → pick `xinsp-backend.exe`.
3. Set a breakpoint.
4. Trigger via `cmd:run` (extension's Run button).

Plugins built in-project use `CompileMode::PluginDev` (`/Od /Zi
/RTC1`), so debug symbols are accurate.
