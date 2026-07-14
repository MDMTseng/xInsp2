# Roadmap — stream control markers (EOS / segment / flush)

Status: DESIGN SETTLED, IMPLEMENTATION UNSCHEDULED (2026-07-15, CT + review).
Core change required: **zero**. Pull this page when a real streaming
application (line-scan, continuous profile) arrives.

## What markers are for

Boundary events between data: "this group is complete" (calib set, HDR
stack, end of a roll), "the source ended" (replay EOF — aggregators must
flush, else results never emit), "discard in-flight" (abort without mixing
batches), and the ordered-sink deadlock antidote ("seq 7 is never coming").

## The settled design (CT 2026-07-15)

1. **Markers are emitted by SOURCE plugins, in-band.** The source is the one
   party that knows the boundary truth (file list done, camera stopped, batch
   of N issued). A marker is an ordinary pack with only `$` keys
   (`$eof`/`$segment` + `$stream` + `$count`), emitted on the same path as
   the frames — so it sits BEHIND its data in every hop's FIFO. Per-hop
   ordering is free; the multi-hop straggler problem never exists
   (the GStreamer serial-event property, bought with same-path instead of
   declared topology).
2. **Router scripts have ONE duty**: wherever they routed stream X's data,
   they route X's markers. This is the single leak point (CT's if/else
   concern) and it is fenced: an SDK `is_marker()` helper + a canonical
   router idiom in the docs + a qa example whose failure mode (aggregator
   never finalizes) is recognizable.
3. **Consumers reconcile by count**: `$eof` carries `$count:N`; an aggregator
   holding N-1 frames waits for the straggler (short timeout as backstop).
   Stateless plugins ignore markers entirely — default no-op.
4. **Strict flush (abort / anti-mixing) does NOT race markers**: it rides the
   existing drain-barrier machinery (quiesce → flush marker → release), the
   same mechanism config-swap already trusts.
5. Global-vs-lane ruling: delivery scope is wherever the data path carries
   the marker; ordering guarantees are per-lane only (lanes are independent
   by design — no cross-lane global order exists to promise).

## Existing embryo

`$eof`/`$part`/`$seq`/`$stream` are already in propagate_fault's copied-key
set; emit is an existing verb; a marker is just a pack. What's missing is only
the SDK helper, the router idiom doc, and the qa suite pinning: marker
never overtakes same-path data; ordered sink releases on `$eof`; count
reconciliation; flush-under-drain.
