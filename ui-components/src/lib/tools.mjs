//
// tools.mjs — pull-model teach tools (task #77). Pure interaction state machines
// that turn pointer events (in IMAGE coordinates) into a result. The editor
// component feeds them screen→image points (via viewport.mjs) and asks them to
// draw an overlay; the result rides back to the plugin via exchange_instance.
// No backend/ABI change — the webui drives (see plugin-caveats.md "UI patterns").
//
// A tool: { type, onDown(p), onMove(p), onUp(p), onDbl(p), done(), result(),
//           draw(ctx, toScreen) }. State is closed over per instance.
//

// point — single click picks one image point. result: {x,y}
export function pointTool() {
  let pt = null;
  return {
    type: "point",
    onDown(p) { pt = { x: Math.round(p.x), y: Math.round(p.y) }; },
    onMove() {}, onUp() {}, onDbl() {},
    done() { return !!pt; },
    result() { return pt ? { ...pt } : null; },
    draw(ctx, toScreen) {
      if (!pt) return;
      const s = toScreen(pt);
      ctx.fillStyle = "#f59e0b";
      ctx.beginPath(); ctx.arc(s.x, s.y, 4, 0, Math.PI * 2); ctx.fill();
    },
  };
}

// rect — drag a rectangle. result: normalized {x,y,w,h} in image space.
export function rectTool() {
  let a = null, b = null, dragging = false;
  const norm = () => ({
    x: Math.round(Math.min(a.x, b.x)), y: Math.round(Math.min(a.y, b.y)),
    w: Math.round(Math.abs(a.x - b.x)), h: Math.round(Math.abs(a.y - b.y)),
  });
  return {
    type: "rect",
    onDown(p) { a = { ...p }; b = { ...p }; dragging = true; },
    onMove(p) { if (dragging) b = { ...p }; },
    onUp(p) { if (dragging) { b = { ...p }; dragging = false; } },
    onDbl() {},
    done() { return !!(a && b) && (norm().w > 0 || norm().h > 0); },
    result() { return a && b ? norm() : null; },
    draw(ctx, toScreen) {
      if (!a || !b) return;
      const s0 = toScreen(a), s1 = toScreen(b);
      ctx.strokeStyle = "#f59e0b"; ctx.lineWidth = 1.5;
      ctx.strokeRect(Math.min(s0.x, s1.x), Math.min(s0.y, s1.y), Math.abs(s1.x - s0.x), Math.abs(s1.y - s0.y));
    },
  };
}

// polygon — click to add vertices, double-click (or onDbl) closes. result: {points:[[x,y]…]}
export function polygonTool() {
  let pts = [], closed = false;
  return {
    type: "polygon",
    onDown(p) { if (!closed) pts.push([Math.round(p.x), Math.round(p.y)]); },
    onMove() {}, onUp() {},
    onDbl() { if (pts.length >= 3) closed = true; },
    done() { return closed && pts.length >= 3; },
    result() { return pts.length >= 3 ? { points: pts.map((p) => [...p]), closed } : null; },
    draw(ctx, toScreen) {
      if (!pts.length) return;
      ctx.strokeStyle = "#f59e0b"; ctx.fillStyle = "#f59e0b"; ctx.lineWidth = 1.5;
      ctx.beginPath();
      pts.forEach((p, i) => { const s = toScreen({ x: p[0], y: p[1] }); i ? ctx.lineTo(s.x, s.y) : ctx.moveTo(s.x, s.y); });
      if (closed) ctx.closePath();
      ctx.stroke();
      for (const p of pts) { const s = toScreen({ x: p[0], y: p[1] }); ctx.beginPath(); ctx.arc(s.x, s.y, 3, 0, Math.PI * 2); ctx.fill(); }
    },
  };
}

export const TOOLS = { point: pointTool, rect: rectTool, polygon: polygonTool };

// Register a custom tool factory (plugins can ship their own).
export function registerTool(id, factory) { TOOLS[id] = factory; }
export function makeTool(id) { return TOOLS[id] ? TOOLS[id]() : null; }
