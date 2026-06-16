# Data types at the boundary — Record, Image, typed I/O

> **Scope:** what crosses between script and plugin — `xi::Record` (path
> expressions, image bag), `xi::Image` (OpenCV interop, the RGB-not-BGR gotcha),
> and the nominal typed-I/O contract surface (types + NA + provenance) as seen
> by a wiring author.
> **Status:** SKELETON.
> <!-- source: docs/reference/image-io.md + Record/boundary parts of docs/architecture.md + the contract surface of docs/design/io-types-and-na.md -->

<!-- TODO P2: port + tighten. NOTE (open question #2 in PLAN.md): confirm this
  grouping isn't too big — split Image into its own file if it bloats. Keep the
  *mechanics* of typed I/O in internals/typed-io.md; keep only the contract here. -->
