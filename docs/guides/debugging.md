# Debugging guide

The most common failure modes in xInsp2 and how to chase them down. The
backend is built to keep running through plugin / script crashes, so
"the backend died" is news worth investigating.

---

## Levels of crash isolation (in-proc, master)

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

For the strictest isolation (separate process), see the
`shm-process-isolation` spike branch — `instance.json: "isolation":
"process"` opts an instance into its own process.

---

## When the script crashes

### Symptom: `cmd:run` returns `error: script crashed: ACCESS_VIOLATION`

1. Check the **Output panel** in VS Code for the full error line:
   ```
   [xinsp2] inspect crashed: 0xC0000005 (ACCESS_VIOLATION)
   ```
2. Open the file → look for unguarded dereferences or out-of-range
   indexing on the line you most recently changed.
3. If it's reproducible: set `xi::breakpoint("right-before-crash")` to
   pause and step in the VS Code C++ debugger (attach to
   `xinsp-backend.exe`).

### Symptom: backend just disappears

Most likely **stack overflow** (unbounded recursion in the script) or
**heap corruption**. SEH won't catch these.

- Check the auto-respawn — extension restarts the backend within ~2s.
- Check `crash_reports/` for a JSON report + minidump:
  - `crash_reports/<timestamp>.json` — exception code, faulting module,
    last cmd, last script.
  - `crash_reports/<timestamp>.dmp` — minidump for WinDbg / VS.

The extension toasts a "Backend recovered after crash in `<module>`"
message naming the offending DLL.

### Recovering state

After a backend crash + auto-respawn:
- The extension calls `cmd:open_project` with `lastProjectFolder` to
  reload your project.
- `xi::state()` JSON is persisted before crash if the script exited
  cleanly; **not** if the crash was during inspect. To make a script
  crash-resilient, write to `xi::state()` between safe checkpoints.

---

## When a plugin crashes

### Symptom: `use_process('det0') crashed: 0xC0000005`

- Open the plugin's source. Check the bounds on inputs you read from
  `xi::Record` (often a missing image or wrong dimensions).
- The host doesn't unload the plugin on crash — you can keep retrying
  after a fix and `cmd:rescan_plugins` reloads it.

### Symptom: plugin's `process()` runs forever (UI freezes)

The host's **watchdog** kills runaway inspects after `watchdog_ms` (set
via `cmd:set_watchdog_ms` or per-project). Default 10000 ms.

Fix by:
- Adding loop bounds in your plugin / op.
- Bumping the watchdog if the operation legitimately takes longer.
- (`shm-process-isolation` spike) Marking the instance
  `"isolation": "process"` so a hang only kills the worker process.

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
3. `Shift+Ctrl+P` → **xInsp2: Compile Script** to force a manual
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
| `cmd:watchdog_status` | Get current watchdog timeout |
| `cmd:set_watchdog_ms` | Change watchdog timeout at runtime |
| Status bar at bottom-left | Backend connection state + WORK indicator |

In the extension's command palette:

- **xInsp2: Show Project Warnings** — surface any instances that were
  skipped on `open_project` (bad plugin / missing DLL / etc.).
- **xInsp2: Restart Backend** — manual respawn if auto-respawn rate
  limit was hit.

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

For SHM spike workers: attach to `xinsp-worker.exe` or
`xinsp-script-runner.exe` instead — they run as separate processes.
