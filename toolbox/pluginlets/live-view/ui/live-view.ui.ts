// live_view.ui.ts — the UI half of the `live-view` pluginlet (doc 37).
//
// A self-contained widget the webui bundles at build time (the "header
// equivalent" of the native .hpp — compile-time binding, no runtime UI-plugin
// loader). The host webui, seeing a plugin declare `"pluginlets": ["live-view"]`,
// mounts this bound to that plugin's channel. The author wires nothing.
//
// It owns the browser end of the pluginlet's private protocol (contract.ts):
//   * receives pre-encoded JPEG frames on the channel and draws them,
//   * tracks the on-screen viewport (pan/zoom) and sends it UPSTREAM, so the
//     native half can crop+downsample to exactly what's visible — the viewport
//     feedback that had no home before the pluginlet (doc 37).
//
// Transport is injected (LiveViewTransport) so this widget is testable headless
// and agnostic to how the webui speaks to expose (WS today).

import {
  Viewport,
  ViewportMessage,
  viewportMessage,
} from "../contract";

/** How the widget reaches expose. The webui supplies one backed by its WS. */
export interface LiveViewTransport {
  /** Subscribe to a channel's frames. Returns an unsubscribe fn. */
  onFrame(channel: string, cb: (jpeg: Uint8Array) => void): () => void;
  /** Send the widget's latest viewport upstream (contract.ts ViewportMessage). */
  send(msg: ViewportMessage): void;
}

export interface LiveViewOptions {
  /** Coalesce viewport sends to at most one per this many ms (default 100). */
  viewportThrottleMs?: number;
}

/** Mount a live-view widget into `el`, bound to `channel`. Returns a disposer. */
export function mountLiveView(
  el: HTMLElement,
  channel: string,
  transport: LiveViewTransport,
  opts: LiveViewOptions = {},
): () => void {
  const throttleMs = opts.viewportThrottleMs ?? 100;

  const canvas = document.createElement("canvas");
  canvas.style.width = "100%";
  canvas.style.height = "100%";
  canvas.style.display = "block";
  el.appendChild(canvas);
  const ctx = canvas.getContext("2d")!;

  // Full-image dimensions, learned from the first decoded frame.
  let imgW = 0;
  let imgH = 0;
  // Current viewport in FULL-IMAGE pixels (what the native half crops to).
  let vp: Viewport = { x: 0, y: 0, w: 0, h: 0 };

  let lastSent = 0;
  let pending: number | null = null;
  const sendViewport = () => {
    pending = null;
    if (vp.w <= 0 || vp.h <= 0) return;
    lastSent = now();
    transport.send(viewportMessage(channel, vp));
  };
  const scheduleViewport = () => {
    if (pending !== null) return;
    const wait = Math.max(0, throttleMs - (now() - lastSent));
    pending = window.setTimeout(sendViewport, wait);
  };

  // Draw a decoded JPEG frame. The native half already cropped+scaled to the
  // viewport, so the frame IS the viewport — stretch it to fill the canvas.
  const drawFrame = (jpeg: Uint8Array) => {
    const blob = new Blob([jpeg], { type: "image/jpeg" });
    const url = URL.createObjectURL(blob);
    const img = new Image();
    img.onload = () => {
      // First frame with no viewport yet => the frame is the whole image.
      if (imgW === 0) {
        imgW = img.naturalWidth;
        imgH = img.naturalHeight;
        if (vp.w === 0) vp = { x: 0, y: 0, w: imgW, h: imgH };
      }
      canvas.width = img.naturalWidth;
      canvas.height = img.naturalHeight;
      ctx.drawImage(img, 0, 0);
      URL.revokeObjectURL(url);
    };
    img.src = url;
  };

  // Pan/zoom input -> update viewport (full-image px) -> throttled upstream send.
  const zoomAt = (cx: number, cy: number, factor: number) => {
    if (imgW === 0) return;
    const nw = clamp(vp.w * factor, 16, imgW);
    const nh = clamp(vp.h * factor, 16, imgH);
    // keep the point under the cursor stable
    const rx = cx / canvas.clientWidth;
    const ry = cy / canvas.clientHeight;
    vp = {
      x: clamp(vp.x + (vp.w - nw) * rx, 0, imgW - nw),
      y: clamp(vp.y + (vp.h - nh) * ry, 0, imgH - nh),
      w: nw,
      h: nh,
    };
    scheduleViewport();
  };

  const onWheel = (e: WheelEvent) => {
    e.preventDefault();
    zoomAt(e.offsetX, e.offsetY, e.deltaY > 0 ? 1.1 : 1 / 1.1);
  };
  let drag: { x: number; y: number } | null = null;
  const onDown = (e: MouseEvent) => (drag = { x: e.clientX, y: e.clientY });
  const onUp = () => (drag = null);
  const onMove = (e: MouseEvent) => {
    if (!drag || imgW === 0) return;
    const dxPx = ((e.clientX - drag.x) / canvas.clientWidth) * vp.w;
    const dyPx = ((e.clientY - drag.y) / canvas.clientHeight) * vp.h;
    vp = {
      ...vp,
      x: clamp(vp.x - dxPx, 0, imgW - vp.w),
      y: clamp(vp.y - dyPx, 0, imgH - vp.h),
    };
    drag = { x: e.clientX, y: e.clientY };
    scheduleViewport();
  };

  canvas.addEventListener("wheel", onWheel, { passive: false });
  canvas.addEventListener("mousedown", onDown);
  window.addEventListener("mouseup", onUp);
  window.addEventListener("mousemove", onMove);
  const unsub = transport.onFrame(channel, drawFrame);

  return () => {
    if (pending !== null) window.clearTimeout(pending);
    canvas.removeEventListener("wheel", onWheel);
    canvas.removeEventListener("mousedown", onDown);
    window.removeEventListener("mouseup", onUp);
    window.removeEventListener("mousemove", onMove);
    unsub();
    el.removeChild(canvas);
  };
}

function clamp(v: number, lo: number, hi: number): number {
  return v < lo ? lo : v > hi ? hi : v;
}
function now(): number {
  return typeof performance !== "undefined" ? performance.now() : Date.now();
}
