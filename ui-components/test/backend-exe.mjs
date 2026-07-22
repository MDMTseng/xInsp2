// backend-exe.mjs — preconditions for the tests that drive a REAL backend over WS.
//
// Two things those tests need, and neither is guaranteed in a fresh checkout:
//
//  1. A built backend executable. The layout differs per generator: the
//     Windows/MSVC multi-config build writes backend/build/<Config>/xinsp-backend.exe,
//     the Linux Ninja single-config build writes backend/build/xinsp-backend.
//     Mirrors tests/fuzz/_common.py's _find_backend_exe (same XINSP_BACKEND_EXE
//     override, same Release-then-Debug preference) so the Python and Node
//     suites share one convention.
//  2. A WebSocket implementation. src/ws-client.mjs is the BROWSER shim — it uses
//     the global WebSocket and takes no dependency on the `ws` package. Node
//     exposes that global from v22; on v20 the shim throws "no WebSocket
//     implementation".
//
// Either missing → skip with a reason, rather than fail: `npm test` must stay
// runnable in a checkout with no C++ build, and on the node the repo pins.
import { existsSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const REPO_ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const EXE = process.platform === "win32" ? "xinsp-backend.exe" : "xinsp-backend";

export function findBackendExe() {
  const override = process.env.XINSP_BACKEND_EXE;
  if (override) return existsSync(override) ? override : null;

  const base = resolve(REPO_ROOT, "backend", "build");
  for (const cfg of ["Release", "Debug", ""]) {
    const cand = resolve(base, cfg, EXE);
    if (existsSync(cand)) return cand;
  }
  return null;
}

export const backendExe = findBackendExe();

// node:test takes { skip: <string> } — a truthy reason skips the test.
export const skipLiveBackendTest =
  (!backendExe &&
    `no backend built (looked for backend/build/[Release|Debug/]${EXE}; set XINSP_BACKEND_EXE to override)`) ||
  (typeof WebSocket === "undefined" &&
    `no global WebSocket (node ${process.versions.node}; the browser shim needs node >= 22)`) ||
  false;
