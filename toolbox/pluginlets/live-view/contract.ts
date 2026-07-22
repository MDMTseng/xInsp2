// contract.ts — the SHARED contract of the `live-view` pluginlet (doc 37).
//
// The single source of truth both halves validate against: the C++ native half
// (live_view.hpp) and the TS UI half (live_view.ui.ts). It rides the same
// cross-language discipline as the canonical-msgpack golden fixtures
// (test_mp_fixtures keep C++/TS/Python codecs in lockstep) — this file is a new
// consumer of that rail, not a new mechanism.
//
// Keep these key strings identical to live_view_keys in live_view.hpp.

/** Reply keys expose returns from the xi.ui.sink subscription probe. */
export const LiveViewKeys = {
  /** i64: non-zero when at least one browser is subscribed to the channel. */
  subscribed: "subscribed",
  /** string "x,y,w,h" (i32 CSV): the browser's latest viewport for the channel,
   *  in FULL-IMAGE pixel coordinates. Absent => native ships the full frame. */
  viewport: "viewport",
} as const;

/** Full-image pixel rectangle the widget is currently showing. */
export interface Viewport {
  x: number;
  y: number;
  w: number;
  h: number;
}

/** Serialize a viewport to the "x,y,w,h" CSV the native half parses. */
export function encodeViewport(v: Viewport): string {
  return `${v.x | 0},${v.y | 0},${v.w | 0},${v.h | 0}`;
}

/** The message the UI widget sends UPSTREAM (widget -> expose -> native demand).
 *  The webui transport delivers it as expose's `viewport` exchange command
 *  ({command:"viewport",channel,x,y,w,h}); expose stores the latest per channel
 *  (only while subscribed) and returns it as LiveViewKeys.viewport in the next
 *  subscription probe reply. */
export interface ViewportMessage {
  type: "viewport";
  channel: string;
  x: number;
  y: number;
  w: number;
  h: number;
}

export function viewportMessage(channel: string, v: Viewport): ViewportMessage {
  return { type: "viewport", channel, x: v.x | 0, y: v.y | 0, w: v.w | 0, h: v.h | 0 };
}
