// Type declarations for backendMode.mjs (a plain-JS module shared with a node
// --test unit). Keeps `tsc --noEmit` clean without compiling the .mjs.
export function resolveBackendMode(
  configMode: string,
  portAlreadyOpen: boolean
): 'managed' | 'attach';
