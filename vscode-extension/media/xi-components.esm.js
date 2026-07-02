var ks = Object.defineProperty;
var Br = (e) => {
  throw TypeError(e);
};
var Ts = (e, t, n) => t in e ? ks(e, t, { enumerable: !0, configurable: !0, writable: !0, value: n }) : e[t] = n;
var W = (e, t, n) => Ts(e, typeof t != "symbol" ? t + "" : t, n), Gn = (e, t, n) => t.has(e) || Br("Cannot " + n);
var f = (e, t, n) => (Gn(e, t, "read from private field"), n ? n.call(e) : t.get(e)), S = (e, t, n) => t.has(e) ? Br("Cannot add the same private member more than once") : t instanceof WeakSet ? t.add(e) : t.set(e, n), k = (e, t, n, r) => (Gn(e, t, "write to private field"), r ? r.call(e, n) : t.set(e, n), n), R = (e, t, n) => (Gn(e, t, "access private method"), n);
var oi;
typeof window < "u" && ((oi = window.__svelte ?? (window.__svelte = {})).v ?? (oi.v = /* @__PURE__ */ new Set())).add("5");
const Ss = 1, Cs = 2, ci = 4, Ms = 8, As = 16, Os = 1, Ns = 4, Rs = 8, Is = 16, Ds = 2, di = "[", _r = "[!", qr = "[?", br = "]", Xt = {}, U = Symbol("uninitialized"), Ls = "http://www.w3.org/1999/xhtml", hi = !1;
var yr = Array.isArray, Ps = Array.prototype.indexOf, On = Array.prototype.includes, zn = Array.from, Nn = Object.keys, Rn = Object.defineProperty, $t = Object.getOwnPropertyDescriptor, js = Object.getOwnPropertyDescriptors, Hs = Object.prototype, Fs = Array.prototype, vi = Object.getPrototypeOf, Xr = Object.isExtensible;
const Ws = () => {
};
function Ys(e) {
  for (var t = 0; t < e.length; t++)
    e[t]();
}
function pi() {
  var e, t, n = new Promise((r, i) => {
    e = r, t = i;
  });
  return { promise: n, resolve: e, reject: t };
}
const ne = 2, Vt = 4, Bn = 8, gi = 1 << 24, Re = 16, De = 32, it = 64, er = 128, Ee = 512, G = 1024, Z = 2048, qe = 4096, ie = 8192, pe = 16384, Ot = 32768, tr = 1 << 25, Ut = 65536, In = 1 << 17, zs = 1 << 18, Nt = 1 << 19, Bs = 1 << 20, We = 1 << 25, Mt = 65536, Dn = 1 << 21, jt = 1 << 22, ct = 1 << 23, kt = Symbol("$state"), mi = Symbol("legacy props"), qs = Symbol(""), kn = Symbol("attributes"), nr = Symbol("class"), Xs = Symbol("style"), en = Symbol("text"), _i = Symbol("form reset"), qn = new class extends Error {
  constructor() {
    super(...arguments);
    W(this, "name", "StaleReactionError");
    W(this, "message", "The reaction that called `getAbortSignal()` was re-run or destroyed");
  }
}();
var ui;
const bi = (
  // We gotta write it like this because after downleveling the pure comment may end up in the wrong location
  !!((ui = globalThis.document) != null && ui.contentType) && /* @__PURE__ */ globalThis.document.contentType.includes("xml")
), xr = 3, _n = 8;
function Vs() {
  throw new Error("https://svelte.dev/e/async_derived_orphan");
}
function Us(e, t, n) {
  throw new Error("https://svelte.dev/e/each_key_duplicate");
}
function Gs(e) {
  throw new Error("https://svelte.dev/e/effect_in_teardown");
}
function Ks() {
  throw new Error("https://svelte.dev/e/effect_in_unowned_derived");
}
function Js(e) {
  throw new Error("https://svelte.dev/e/effect_orphan");
}
function Zs() {
  throw new Error("https://svelte.dev/e/effect_update_depth_exceeded");
}
function Qs() {
  throw new Error("https://svelte.dev/e/hydration_failed");
}
function el(e) {
  throw new Error("https://svelte.dev/e/props_invalid_value");
}
function tl() {
  throw new Error("https://svelte.dev/e/state_descriptors_fixed");
}
function nl() {
  throw new Error("https://svelte.dev/e/state_prototype_fixed");
}
function rl() {
  throw new Error("https://svelte.dev/e/state_unsafe_mutation");
}
function il() {
  throw new Error("https://svelte.dev/e/svelte_boundary_reset_onerror");
}
function sl() {
  console.warn("https://svelte.dev/e/derived_inert");
}
function Xn(e) {
  console.warn("https://svelte.dev/e/hydration_mismatch");
}
function ll() {
  console.warn("https://svelte.dev/e/select_multiple_invalid_value");
}
function al() {
  console.warn("https://svelte.dev/e/svelte_boundary_reset_noop");
}
let D = !1;
function nt(e) {
  D = e;
}
let j;
function ge(e) {
  if (e === null)
    throw Xn(), Xt;
  return j = e;
}
function bn() {
  return ge(/* @__PURE__ */ lt(j));
}
function Y(e) {
  if (D) {
    if (/* @__PURE__ */ lt(j) !== null)
      throw Xn(), Xt;
    j = e;
  }
}
function ol(e = 1) {
  if (D) {
    for (var t = e, n = j; t--; )
      n = /** @type {TemplateNode} */
      /* @__PURE__ */ lt(n);
    j = n;
  }
}
function Ln(e = !0) {
  for (var t = 0, n = j; ; ) {
    if (n.nodeType === _n) {
      var r = (
        /** @type {Comment} */
        n.data
      );
      if (r === br) {
        if (t === 0) return n;
        t -= 1;
      } else (r === di || r === _r || // "[1", "[2", etc. for if blocks
      r[0] === "[" && !isNaN(Number(r.slice(1)))) && (t += 1);
    }
    var i = (
      /** @type {TemplateNode} */
      /* @__PURE__ */ lt(n)
    );
    e && n.remove(), n = i;
  }
}
function yi(e) {
  if (!e || e.nodeType !== _n)
    throw Xn(), Xt;
  return (
    /** @type {Comment} */
    e.data
  );
}
function xi(e) {
  return e === this.v;
}
function ul(e, t) {
  return e != e ? t == t : e !== t || e !== null && typeof e == "object" || typeof e == "function";
}
function wi(e) {
  return !ul(e, this.v);
}
let fl = !1, se = null;
function Gt(e) {
  se = e;
}
function Se(e, t = !1, n) {
  se = {
    p: se,
    i: !1,
    c: null,
    e: null,
    s: e,
    x: null,
    r: (
      /** @type {Effect} */
      C
    ),
    l: null
  };
}
function Ce(e) {
  var t = (
    /** @type {ComponentContext} */
    se
  ), n = t.e;
  if (n !== null) {
    t.e = null;
    for (var r of n)
      Gi(r);
  }
  return e !== void 0 && (t.x = e), t.i = !0, se = t.p, e ?? /** @type {T} */
  {};
}
function Ei() {
  return !0;
}
let pt = [];
function $i() {
  var e = pt;
  pt = [], Ys(e);
}
function rt(e) {
  if (pt.length === 0 && !an) {
    var t = pt;
    queueMicrotask(() => {
      t === pt && $i();
    });
  }
  pt.push(e);
}
function cl() {
  for (; pt.length > 0; )
    $i();
}
function ki(e) {
  var t = C;
  if (t === null)
    return M.f |= ct, e;
  if ((t.f & Ot) === 0 && (t.f & Vt) === 0)
    throw e;
  ft(e, t);
}
function ft(e, t) {
  if (!(t !== null && (t.f & pe) !== 0)) {
    for (; t !== null; ) {
      if ((t.f & er) !== 0) {
        if ((t.f & Ot) === 0)
          throw e;
        try {
          t.b.error(e);
          return;
        } catch (n) {
          e = n;
        }
      }
      t = t.parent;
    }
    throw e;
  }
}
const dl = -7169;
function X(e, t) {
  e.f = e.f & dl | t;
}
function wr(e) {
  (e.f & Ee) !== 0 || e.deps === null ? X(e, G) : X(e, qe);
}
function Ti(e) {
  if (e !== null)
    for (const t of e)
      (t.f & ne) === 0 || (t.f & Mt) === 0 || (t.f ^= Mt, Ti(
        /** @type {Derived} */
        t.deps
      ));
}
function Si(e, t, n) {
  (e.f & Z) !== 0 ? t.add(e) : (e.f & qe) !== 0 && n.add(e), Ti(e.deps), X(e, G);
}
let En = !1;
function hl(e) {
  var t = En;
  try {
    return En = !1, [e(), En];
  } finally {
    En = t;
  }
}
function vl(e) {
  let t = 0, n = At(0), r;
  return () => {
    Mr() && (O(n), Nr(() => (t === 0 && (r = Lr(() => e(() => on(n)))), t += 1, () => {
      rt(() => {
        t -= 1, t === 0 && (r == null || r(), r = void 0, on(n));
      });
    })));
  };
}
var pl = Ut | Nt;
function gl(e, t, n, r) {
  new ml(e, t, n, r);
}
var de, dn, be, bt, oe, ye, re, he, Je, yt, ot, Ht, hn, vn, Ze, Fn, F, Ci, Mi, Ai, rr, Tn, Sn, ir, sr;
class ml {
  /**
   * @param {TemplateNode} node
   * @param {BoundaryProps} props
   * @param {((anchor: Node) => void)} children
   * @param {((error: unknown) => unknown) | undefined} [transform_error]
   */
  constructor(t, n, r, i) {
    S(this, F);
    /** @type {Boundary | null} */
    W(this, "parent");
    W(this, "is_pending", !1);
    /**
     * API-level transformError transform function. Transforms errors before they reach the `failed` snippet.
     * Inherited from parent boundary, or defaults to identity.
     * @type {(error: unknown) => unknown}
     */
    W(this, "transform_error");
    /** @type {TemplateNode} */
    S(this, de);
    /** @type {TemplateNode | null} */
    S(this, dn, D ? j : null);
    /** @type {BoundaryProps} */
    S(this, be);
    /** @type {((anchor: Node) => void)} */
    S(this, bt);
    /** @type {Effect} */
    S(this, oe);
    /** @type {Effect | null} */
    S(this, ye, null);
    /** @type {Effect | null} */
    S(this, re, null);
    /** @type {Effect | null} */
    S(this, he, null);
    /** @type {DocumentFragment | null} */
    S(this, Je, null);
    S(this, yt, 0);
    S(this, ot, 0);
    S(this, Ht, !1);
    /** @type {Set<Effect>} */
    S(this, hn, /* @__PURE__ */ new Set());
    /** @type {Set<Effect>} */
    S(this, vn, /* @__PURE__ */ new Set());
    /**
     * A source containing the number of pending async deriveds/expressions.
     * Only created if `$effect.pending()` is used inside the boundary,
     * otherwise updating the source results in needless `Batch.ensure()`
     * calls followed by no-op flushes
     * @type {Source<number> | null}
     */
    S(this, Ze, null);
    S(this, Fn, vl(() => (k(this, Ze, At(f(this, yt))), () => {
      k(this, Ze, null);
    })));
    var s;
    k(this, de, t), k(this, be, n), k(this, bt, (l) => {
      var a = (
        /** @type {Effect} */
        C
      );
      a.b = this, a.f |= er, r(l);
    }), this.parent = /** @type {Effect} */
    C.b, this.transform_error = i ?? ((s = this.parent) == null ? void 0 : s.transform_error) ?? ((l) => l), k(this, oe, Rr(() => {
      if (D) {
        const l = (
          /** @type {Comment} */
          f(this, dn)
        );
        bn();
        const a = l.data === _r;
        if (l.data.startsWith(qr)) {
          const u = JSON.parse(l.data.slice(qr.length));
          R(this, F, Mi).call(this, u);
        } else a ? R(this, F, Ai).call(this) : R(this, F, Ci).call(this);
      } else
        R(this, F, rr).call(this);
    }, pl)), D && k(this, de, j);
  }
  /**
   * Defer an effect inside a pending boundary until the boundary resolves
   * @param {Effect} effect
   */
  defer_effect(t) {
    Si(t, f(this, hn), f(this, vn));
  }
  /**
   * Returns `false` if the effect exists inside a boundary whose pending snippet is shown
   * @returns {boolean}
   */
  is_rendered() {
    return !this.is_pending && (!this.parent || this.parent.is_rendered());
  }
  has_pending_snippet() {
    return !!f(this, be).pending;
  }
  /**
   * Update the source that powers `$effect.pending()` inside this boundary,
   * and controls when the current `pending` snippet (if any) is removed.
   * Do not call from inside the class
   * @param {1 | -1} d
   * @param {Batch} batch
   */
  update_pending_count(t, n) {
    R(this, F, ir).call(this, t, n), k(this, yt, f(this, yt) + t), !(!f(this, Ze) || f(this, Ht)) && (k(this, Ht, !0), rt(() => {
      k(this, Ht, !1), f(this, Ze) && Kt(f(this, Ze), f(this, yt));
    }));
  }
  get_effect_pending() {
    return f(this, Fn).call(this), O(
      /** @type {Source<number>} */
      f(this, Ze)
    );
  }
  /** @param {unknown} error */
  error(t) {
    if (!f(this, be).onerror && !f(this, be).failed)
      throw t;
    T != null && T.is_fork ? (f(this, ye) && T.skip_effect(f(this, ye)), f(this, re) && T.skip_effect(f(this, re)), f(this, he) && T.skip_effect(f(this, he)), T.oncommit(() => {
      R(this, F, sr).call(this, t);
    })) : R(this, F, sr).call(this, t);
  }
}
de = new WeakMap(), dn = new WeakMap(), be = new WeakMap(), bt = new WeakMap(), oe = new WeakMap(), ye = new WeakMap(), re = new WeakMap(), he = new WeakMap(), Je = new WeakMap(), yt = new WeakMap(), ot = new WeakMap(), Ht = new WeakMap(), hn = new WeakMap(), vn = new WeakMap(), Ze = new WeakMap(), Fn = new WeakMap(), F = new WeakSet(), Ci = function() {
  try {
    k(this, ye, we(() => f(this, bt).call(this, f(this, de))));
  } catch (t) {
    this.error(t);
  }
}, /**
 * @param {unknown} error The deserialized error from the server's hydration comment
 */
Mi = function(t) {
  const n = f(this, be).failed;
  n && k(this, he, we(() => {
    n(
      f(this, de),
      () => t,
      () => () => {
      }
    );
  }));
}, Ai = function() {
  const t = f(this, be).pending;
  t && (this.is_pending = !0, k(this, re, we(() => t(f(this, de)))), rt(() => {
    var n = k(this, Je, document.createDocumentFragment()), r = Ye();
    n.append(r), k(this, ye, R(this, F, Sn).call(this, () => we(() => f(this, bt).call(this, r)))), f(this, ot) === 0 && (f(this, de).before(n), k(this, Je, null), St(
      /** @type {Effect} */
      f(this, re),
      () => {
        k(this, re, null);
      }
    ), R(this, F, Tn).call(
      this,
      /** @type {Batch} */
      T
    ));
  }));
}, rr = function() {
  try {
    if (this.is_pending = this.has_pending_snippet(), k(this, ot, 0), k(this, yt, 0), k(this, ye, we(() => {
      f(this, bt).call(this, f(this, de));
    })), f(this, ot) > 0) {
      var t = k(this, Je, document.createDocumentFragment());
      Dr(f(this, ye), t);
      const n = (
        /** @type {(anchor: Node) => void} */
        f(this, be).pending
      );
      k(this, re, we(() => n(f(this, de))));
    } else
      R(this, F, Tn).call(
        this,
        /** @type {Batch} */
        T
      );
  } catch (n) {
    this.error(n);
  }
}, /**
 * @param {Batch} batch
 */
Tn = function(t) {
  this.is_pending = !1, t.transfer_effects(f(this, hn), f(this, vn));
}, /**
 * @template T
 * @param {() => T} fn
 */
Sn = function(t) {
  var n = C, r = M, i = se;
  Xe(f(this, oe)), $e(f(this, oe)), Gt(f(this, oe).ctx);
  try {
    return dt.ensure(), t();
  } catch (s) {
    return ki(s), null;
  } finally {
    Xe(n), $e(r), Gt(i);
  }
}, /**
 * Updates the pending count associated with the currently visible pending snippet,
 * if any, such that we can replace the snippet with content once work is done
 * @param {1 | -1} d
 * @param {Batch} batch
 */
ir = function(t, n) {
  var r;
  if (!this.has_pending_snippet()) {
    this.parent && R(r = this.parent, F, ir).call(r, t, n);
    return;
  }
  k(this, ot, f(this, ot) + t), f(this, ot) === 0 && (R(this, F, Tn).call(this, n), f(this, re) && St(f(this, re), () => {
    k(this, re, null);
  }), f(this, Je) && (f(this, de).before(f(this, Je)), k(this, Je, null)));
}, /**
 * @param {unknown} error
 */
sr = function(t) {
  f(this, ye) && (le(f(this, ye)), k(this, ye, null)), f(this, re) && (le(f(this, re)), k(this, re, null)), f(this, he) && (le(f(this, he)), k(this, he, null)), D && (ge(
    /** @type {TemplateNode} */
    f(this, dn)
  ), ol(), ge(Ln()));
  var n = f(this, be).onerror;
  let r = f(this, be).failed;
  var i = !1, s = !1;
  const l = () => {
    if (i) {
      al();
      return;
    }
    i = !0, s && il(), f(this, he) !== null && St(f(this, he), () => {
      k(this, he, null);
    }), R(this, F, Sn).call(this, () => {
      R(this, F, rr).call(this);
    });
  }, a = (o) => {
    try {
      s = !0, n == null || n(o, l), s = !1;
    } catch (u) {
      ft(u, f(this, oe) && f(this, oe).parent);
    }
    r && k(this, he, R(this, F, Sn).call(this, () => {
      try {
        return we(() => {
          var u = (
            /** @type {Effect} */
            C
          );
          u.b = this, u.f |= er, r(
            f(this, de),
            () => o,
            () => l
          );
        });
      } catch (u) {
        return ft(
          u,
          /** @type {Effect} */
          f(this, oe).parent
        ), null;
      }
    }));
  };
  rt(() => {
    var o;
    try {
      o = this.transform_error(t);
    } catch (u) {
      ft(u, f(this, oe) && f(this, oe).parent);
      return;
    }
    o !== null && typeof o == "object" && typeof /** @type {any} */
    o.then == "function" ? o.then(
      a,
      /** @param {unknown} e */
      (u) => ft(u, f(this, oe) && f(this, oe).parent)
    ) : a(o);
  });
};
function _l(e, t, n, r) {
  const i = un;
  var s = e.filter((v) => !v.settled), l = t.map(i);
  if (n.length === 0 && s.length === 0) {
    r(l);
    return;
  }
  var a = (
    /** @type {Effect} */
    C
  ), o = bl(), u = s.length === 1 ? s[0].promise : s.length > 1 ? Promise.all(s.map((v) => v.promise)) : null;
  function d(v) {
    if ((a.f & pe) === 0) {
      o();
      try {
        r([...l, ...v]);
      } catch (m) {
        ft(m, a);
      }
      Pn();
    }
  }
  var p = Oi();
  if (n.length === 0) {
    u.then(() => d([])).finally(p);
    return;
  }
  function h() {
    Promise.all(n.map((v) => /* @__PURE__ */ yl(v))).then(d).catch((v) => ft(v, a)).finally(p);
  }
  u ? u.then(() => {
    o(), h(), Pn();
  }) : h();
}
function bl() {
  var e = (
    /** @type {Effect} */
    C
  ), t = M, n = se, r = (
    /** @type {Batch} */
    T
  );
  return function(s = !0) {
    Xe(e), $e(t), Gt(n), s && (e.f & pe) === 0 && (r == null || r.activate(), r == null || r.apply());
  };
}
function Pn(e = !0) {
  Xe(null), $e(null), Gt(null), e && (T == null || T.deactivate());
}
function Oi() {
  var e = (
    /** @type {Effect} */
    C
  ), t = e.b, n = (
    /** @type {Batch} */
    T
  ), r = !!(t != null && t.is_rendered());
  return t == null || t.update_pending_count(1, n), n.increment(r, e), () => {
    t == null || t.update_pending_count(-1, n), n.decrement(r, e);
  };
}
// @__NO_SIDE_EFFECTS__
function un(e) {
  var t = ne | Z;
  return C !== null && (C.f |= Nt), {
    ctx: se,
    deps: null,
    effects: null,
    equals: xi,
    f: t,
    fn: e,
    reactions: null,
    rv: 0,
    v: (
      /** @type {V} */
      U
    ),
    wv: 0,
    parent: C,
    ac: null
  };
}
const tn = Symbol("obsolete");
// @__NO_SIDE_EFFECTS__
function yl(e, t, n) {
  let r = (
    /** @type {Effect | null} */
    C
  );
  r === null && Vs();
  var i = (
    /** @type {Promise<V>} */
    /** @type {unknown} */
    void 0
  ), s = At(
    /** @type {V} */
    U
  ), l = !M, a = /* @__PURE__ */ new Set();
  return Rl(() => {
    var v, m;
    var o = (
      /** @type {Effect} */
      C
    ), u = pi();
    i = u.promise;
    try {
      Promise.resolve(e()).then(u.resolve, (g) => {
        g !== qn && u.reject(g);
      }).finally(Pn);
    } catch (g) {
      u.reject(g), Pn();
    }
    var d = (
      /** @type {Batch} */
      T
    );
    if (l) {
      if ((o.f & Ot) !== 0)
        var p = Oi();
      if (
        // boundary can be null if the async derived is inside an $effect.root not connected to the component render tree
        (v = r.b) != null && v.is_rendered()
      )
        (m = d.async_deriveds.get(o)) == null || m.reject(tn);
      else
        for (const g of a.values())
          g.reject(tn);
      a.add(u), d.async_deriveds.set(o, u);
    }
    const h = (g, c = void 0) => {
      p == null || p(), a.delete(u), c !== tn && (d.activate(), c ? (s.f |= ct, Kt(s, c)) : ((s.f & ct) !== 0 && (s.f ^= ct), Kt(s, g)), d.deactivate());
    };
    u.promise.then(h, (g) => h(null, g || "unknown"));
  }), Ar(() => {
    for (const o of a)
      o.reject(tn);
  }), new Promise((o) => {
    function u(d) {
      function p() {
        d === i ? o(s) : u(i);
      }
      d.then(p, p);
    }
    u(i);
  });
}
// @__NO_SIDE_EFFECTS__
function Er(e) {
  const t = /* @__PURE__ */ un(e);
  return ts(t), t;
}
// @__NO_SIDE_EFFECTS__
function Ni(e) {
  const t = /* @__PURE__ */ un(e);
  return t.equals = wi, t;
}
function xl(e) {
  var t = e.effects;
  if (t !== null) {
    e.effects = null;
    for (var n = 0; n < t.length; n += 1)
      le(
        /** @type {Effect} */
        t[n]
      );
  }
}
function $r(e) {
  var t, n = C, r = e.parent;
  if (!st && r !== null && e.v !== U && // if it was never evaluated before, it's guaranteed to fail downstream, so we try to execute instead
  (r.f & (pe | ie)) !== 0)
    return sl(), e.v;
  Xe(r);
  try {
    e.f &= ~Mt, xl(e), t = ss(e);
  } finally {
    Xe(n);
  }
  return t;
}
function Ri(e) {
  var t = $r(e);
  if (!e.equals(t) && (e.wv = rs(), (!(T != null && T.is_fork) || e.deps === null) && (T !== null ? (T.capture(e, t, !0), ln == null || ln.capture(e, t, !0)) : e.v = t, e.deps === null))) {
    X(e, G);
    return;
  }
  st || (ee !== null ? (Mr() || T != null && T.is_fork) && ee.set(e, t) : wr(e));
}
function wl(e) {
  var t, n;
  if (e.effects !== null)
    for (const r of e.effects)
      (r.teardown || r.ac) && ((t = r.teardown) == null || t.call(r), (n = r.ac) == null || n.abort(qn), r.fn !== null && (r.teardown = Ws), r.ac = null, cn(r, 0), Ir(r));
}
function Ii(e) {
  if (e.effects !== null)
    for (const t of e.effects)
      t.teardown && t.fn !== null && Jt(t);
}
let Kn = null, Dt = null, T = null, ln = null, ee = null, lr = null, an = !1, Jn = !1, Pt = null, Cn = null;
var Vr = 0;
let El = 1;
var Ft, ut, xt, Wt, Yt, zt, Qe, Bt, ue, pn, et, Ae, He, qt, wt, L, ar, nn, or, Di, Li, Lt, $l, rn;
const Wn = class Wn {
  constructor() {
    S(this, L);
    W(this, "id", El++);
    /** True as soon as `#process` was called */
    S(this, Ft, !1);
    W(this, "linked", !0);
    /** @type {Batch | null} */
    S(this, ut, null);
    /** @type {Batch | null} */
    S(this, xt, null);
    /** @type {Map<Effect, ReturnType<typeof deferred<any>>>} */
    W(this, "async_deriveds", /* @__PURE__ */ new Map());
    /**
     * The current values of any signals that are updated in this batch.
     * Tuple format: [value, is_derived] (note: is_derived is false for deriveds, too, if they were overridden via assignment)
     * They keys of this map are identical to `this.#previous`
     * @type {Map<Value, [any, boolean]>}
     */
    W(this, "current", /* @__PURE__ */ new Map());
    /**
     * The values of any signals (sources and deriveds) that are updated in this batch _before_ those updates took place.
     * They keys of this map are identical to `this.#current`
     * @type {Map<Value, any>}
     */
    W(this, "previous", /* @__PURE__ */ new Map());
    /**
     * When the batch is committed (and the DOM is updated), we need to remove old branches
     * and append new ones by calling the functions added inside (if/each/key/etc) blocks
     * @type {Set<(batch: Batch) => void>}
     */
    S(this, Wt, /* @__PURE__ */ new Set());
    /**
     * If a fork is discarded, we need to destroy any effects that are no longer needed
     * @type {Set<(batch: Batch) => void>}
     */
    S(this, Yt, /* @__PURE__ */ new Set());
    /**
     * The number of async effects that are currently in flight
     */
    S(this, zt, 0);
    /**
     * Async effects that are currently in flight, _not_ inside a pending boundary
     * @type {Map<Effect, number>}
     */
    S(this, Qe, /* @__PURE__ */ new Map());
    /**
     * A deferred that resolves when the batch is committed, used with `settled()`
     * TODO replace with Promise.withResolvers once supported widely enough
     * @type {{ promise: Promise<void>, resolve: (value?: any) => void, reject: (reason: unknown) => void } | null}
     */
    S(this, Bt, null);
    /**
     * The root effects that need to be flushed
     * @type {Effect[]}
     */
    S(this, ue, []);
    /**
     * Effects created while this batch was active.
     * @type {Effect[]}
     */
    S(this, pn, []);
    /**
     * Deferred effects (which run after async work has completed) that are DIRTY
     * @type {Set<Effect>}
     */
    S(this, et, /* @__PURE__ */ new Set());
    /**
     * Deferred effects that are MAYBE_DIRTY
     * @type {Set<Effect>}
     */
    S(this, Ae, /* @__PURE__ */ new Set());
    /**
     * A map of branches that still exist, but will be destroyed when this batch
     * is committed — we skip over these during `process`.
     * The value contains child effects that were dirty/maybe_dirty before being reset,
     * so they can be rescheduled if the branch survives.
     * @type {Map<Effect, { d: Effect[], m: Effect[] }>}
     */
    S(this, He, /* @__PURE__ */ new Map());
    /**
     * Inverse of #skipped_branches which we need to tell prior batches to unskip them when committing
     * @type {Set<Effect>}
     */
    S(this, qt, /* @__PURE__ */ new Set());
    W(this, "is_fork", !1);
    S(this, wt, !1);
    Dt === null ? Kn = Dt = this : (k(Dt, xt, this), k(this, ut, Dt)), Dt = this;
  }
  /**
   * Add an effect to the #skipped_branches map and reset its children
   * @param {Effect} effect
   */
  skip_effect(t) {
    f(this, He).has(t) || f(this, He).set(t, { d: [], m: [] }), f(this, qt).delete(t);
  }
  /**
   * Remove an effect from the #skipped_branches map and reschedule
   * any tracked dirty/maybe_dirty child effects
   * @param {Effect} effect
   * @param {(e: Effect) => void} callback
   */
  unskip_effect(t, n = (r) => this.schedule(r)) {
    var r = f(this, He).get(t);
    if (r) {
      f(this, He).delete(t);
      for (var i of r.d)
        X(i, Z), n(i);
      for (i of r.m)
        X(i, qe), n(i);
    }
    f(this, qt).add(t);
  }
  /**
   * Associate a change to a given source with the current
   * batch, noting its previous and current values
   * @param {Value} source
   * @param {any} value
   * @param {boolean} [is_derived]
   */
  capture(t, n, r = !1) {
    t.v !== U && !this.previous.has(t) && this.previous.set(t, t.v), (t.f & ct) === 0 && (this.current.set(t, [n, r]), ee == null || ee.set(t, n)), this.is_fork || (t.v = n);
  }
  activate() {
    T = this;
  }
  deactivate() {
    T = null, ee = null;
  }
  flush() {
    try {
      Jn = !0, T = this, R(this, L, nn).call(this);
    } finally {
      Vr = 0, lr = null, Pt = null, Cn = null, Jn = !1, T = null, ee = null, Tt.clear();
    }
  }
  discard() {
    var t;
    for (const n of f(this, Yt)) n(this);
    f(this, Yt).clear();
    for (const n of this.async_deriveds.values())
      n.reject(tn);
    R(this, L, rn).call(this), (t = f(this, Bt)) == null || t.resolve();
  }
  /**
   * @param {Effect} effect
   */
  register_created_effect(t) {
    f(this, pn).push(t);
  }
  /**
   * @param {boolean} blocking
   * @param {Effect} effect
   */
  increment(t, n) {
    if (k(this, zt, f(this, zt) + 1), t) {
      let r = f(this, Qe).get(n) ?? 0;
      f(this, Qe).set(n, r + 1);
    }
  }
  /**
   * @param {boolean} blocking
   * @param {Effect} effect
   */
  decrement(t, n) {
    if (k(this, zt, f(this, zt) - 1), t) {
      let r = f(this, Qe).get(n) ?? 0;
      r === 1 ? f(this, Qe).delete(n) : f(this, Qe).set(n, r - 1);
    }
    f(this, wt) || (k(this, wt, !0), rt(() => {
      k(this, wt, !1), this.linked && this.flush();
    }));
  }
  /**
   * @param {Set<Effect>} dirty_effects
   * @param {Set<Effect>} maybe_dirty_effects
   */
  transfer_effects(t, n) {
    for (const r of t)
      f(this, et).add(r);
    for (const r of n)
      f(this, Ae).add(r);
    t.clear(), n.clear();
  }
  /** @param {(batch: Batch) => void} fn */
  oncommit(t) {
    f(this, Wt).add(t);
  }
  /** @param {(batch: Batch) => void} fn */
  ondiscard(t) {
    f(this, Yt).add(t);
  }
  settled() {
    return (f(this, Bt) ?? k(this, Bt, pi())).promise;
  }
  static ensure() {
    if (T === null) {
      const t = T = new Wn();
      !Jn && !an && rt(() => {
        f(t, Ft) || t.flush();
      });
    }
    return T;
  }
  apply() {
    {
      ee = null;
      return;
    }
  }
  /**
   *
   * @param {Effect} effect
   */
  schedule(t) {
    var i;
    if (lr = t, (i = t.b) != null && i.is_pending && (t.f & (Vt | Bn | gi)) !== 0 && (t.f & Ot) === 0) {
      t.b.defer_effect(t);
      return;
    }
    for (var n = t; n.parent !== null; ) {
      n = n.parent;
      var r = n.f;
      if (Pt !== null && n === C && (M === null || (M.f & ne) === 0))
        return;
      if ((r & (it | De)) !== 0) {
        if ((r & G) === 0)
          return;
        n.f ^= G;
      }
    }
    f(this, ue).push(n);
  }
};
Ft = new WeakMap(), ut = new WeakMap(), xt = new WeakMap(), Wt = new WeakMap(), Yt = new WeakMap(), zt = new WeakMap(), Qe = new WeakMap(), Bt = new WeakMap(), ue = new WeakMap(), pn = new WeakMap(), et = new WeakMap(), Ae = new WeakMap(), He = new WeakMap(), qt = new WeakMap(), wt = new WeakMap(), L = new WeakSet(), ar = function() {
  if (this.is_fork) return !0;
  for (const r of f(this, Qe).keys()) {
    for (var t = r, n = !1; t.parent !== null; ) {
      if (f(this, He).has(t)) {
        n = !0;
        break;
      }
      t = t.parent;
    }
    if (!n)
      return !0;
  }
  return !1;
}, nn = function() {
  var o, u, d, p;
  k(this, Ft, !0), Vr++ > 1e3 && (R(this, L, rn).call(this), kl());
  for (const h of f(this, et))
    f(this, Ae).delete(h), X(h, Z), this.schedule(h);
  for (const h of f(this, Ae))
    X(h, qe), this.schedule(h);
  const t = f(this, ue);
  k(this, ue, []), this.apply();
  var n = Pt = [], r = [], i = Cn = [];
  for (const h of t)
    try {
      R(this, L, or).call(this, h, n, r);
    } catch (v) {
      throw Hi(h), R(this, L, ar).call(this) || this.discard(), v;
    }
  if (T = null, i.length > 0) {
    var s = Wn.ensure();
    for (const h of i)
      s.schedule(h);
  }
  if (Pt = null, Cn = null, R(this, L, ar).call(this)) {
    R(this, L, Lt).call(this, r), R(this, L, Lt).call(this, n);
    for (const [h, v] of f(this, He))
      ji(h, v);
    i.length > 0 && /** @type {unknown} */
    R(o = T, L, nn).call(o);
    return;
  }
  const l = R(this, L, Di).call(this);
  if (l) {
    R(this, L, Lt).call(this, r), R(this, L, Lt).call(this, n), R(u = l, L, Li).call(u, this);
    return;
  }
  f(this, et).clear(), f(this, Ae).clear();
  for (const h of f(this, Wt)) h(this);
  f(this, Wt).clear(), ln = this, Ur(r), Ur(n), ln = null, (d = f(this, Bt)) == null || d.resolve();
  var a = (
    /** @type {Batch | null} */
    /** @type {unknown} */
    T
  );
  if (f(this, zt) === 0 && (f(this, ue).length === 0 || a !== null) && R(this, L, rn).call(this), f(this, ue).length > 0)
    if (a !== null) {
      const h = a;
      f(h, ue).push(...f(this, ue).filter((v) => !f(h, ue).includes(v)));
    } else
      a = this;
  a !== null && R(p = a, L, nn).call(p);
}, /**
 * Traverse the effect tree, executing effects or stashing
 * them for later execution as appropriate
 * @param {Effect} root
 * @param {Effect[]} effects
 * @param {Effect[]} render_effects
 */
or = function(t, n, r) {
  t.f ^= G;
  for (var i = t.first; i !== null; ) {
    var s = i.f, l = (s & (De | it)) !== 0, a = l && (s & G) !== 0, o = a || (s & ie) !== 0 || f(this, He).has(i);
    if (!o && i.fn !== null) {
      l ? i.f ^= G : (s & Vt) !== 0 ? n.push(i) : yn(i) && ((s & Re) !== 0 && f(this, Ae).add(i), Jt(i));
      var u = i.first;
      if (u !== null) {
        i = u;
        continue;
      }
    }
    for (; i !== null; ) {
      var d = i.next;
      if (d !== null) {
        i = d;
        break;
      }
      i = i.parent;
    }
  }
}, Di = function() {
  for (var t = f(this, ut); t !== null; ) {
    if (!t.is_fork) {
      for (const [n, [, r]] of this.current)
        if (t.current.has(n) && !r)
          return t;
    }
    t = f(t, ut);
  }
  return null;
}, /**
 * @param {Batch} batch
 */
Li = function(t) {
  var r;
  for (const [i, s] of t.current)
    !this.previous.has(i) && t.previous.has(i) && this.previous.set(i, t.previous.get(i)), this.current.set(i, s);
  for (const [i, s] of t.async_deriveds) {
    const l = this.async_deriveds.get(i);
    l && s.promise.then(l.resolve).catch(l.reject);
  }
  t.async_deriveds.clear(), this.transfer_effects(f(t, et), f(t, Ae));
  const n = (i) => {
    var s = i.reactions;
    if (s !== null)
      for (const o of s) {
        var l = o.f;
        if ((l & ne) !== 0)
          n(
            /** @type {Derived} */
            o
          );
        else {
          var a = (
            /** @type {Effect} */
            o
          );
          l & (jt | Re) && !this.async_deriveds.has(a) && (f(this, Ae).delete(a), X(a, Z), this.schedule(a));
        }
      }
  };
  for (const i of this.current.keys())
    n(i);
  this.oncommit(() => t.discard()), R(r = t, L, rn).call(r), T = this, R(this, L, nn).call(this);
}, /**
 * @param {Effect[]} effects
 */
Lt = function(t) {
  for (var n = 0; n < t.length; n += 1)
    Si(t[n], f(this, et), f(this, Ae));
}, $l = function() {
  var p;
  for (let h = Kn; h !== null; h = f(h, xt)) {
    var t = h.id < this.id, n = [];
    for (const [v, [m, g]] of this.current) {
      if (h.current.has(v)) {
        var r = (
          /** @type {[any, boolean]} */
          h.current.get(v)[0]
        );
        if (t && m !== r)
          h.current.set(v, [m, g]);
        else
          continue;
      }
      n.push(v);
    }
    if (t)
      for (const [v, m] of this.async_deriveds) {
        const g = h.async_deriveds.get(v);
        g && m.promise.then(g.resolve).catch(g.reject);
      }
    var i = [...h.current.keys()].filter(
      (v) => !/** @type {[any, boolean]} */
      h.current.get(v)[1]
    );
    if (!(!f(h, Ft) || i.length === 0)) {
      var s = i.filter((v) => !this.current.has(v));
      if (s.length === 0)
        t && h.discard();
      else if (n.length > 0) {
        if (t)
          for (const v of f(this, qt))
            h.unskip_effect(v, (m) => {
              var g;
              (m.f & (Re | jt)) !== 0 ? h.schedule(m) : R(g = h, L, Lt).call(g, [m]);
            });
        h.activate();
        var l = /* @__PURE__ */ new Set(), a = /* @__PURE__ */ new Map();
        for (var o of n)
          Pi(o, s, l, a);
        a = /* @__PURE__ */ new Map();
        var u = [...h.current].filter(([v, m]) => {
          const g = this.current.get(v);
          return g ? g[0] !== m[0] || g[1] !== m[1] : !0;
        }).map(([v]) => v);
        if (u.length > 0)
          for (const v of f(this, pn))
            (v.f & (pe | ie | In)) === 0 && kr(v, u, a) && ((v.f & (jt | Re)) !== 0 ? (X(v, Z), h.schedule(v)) : f(h, et).add(v));
        if (f(h, ue).length > 0 && !f(h, wt)) {
          h.apply();
          for (var d of f(h, ue))
            R(p = h, L, or).call(p, d, [], []);
          k(h, ue, []);
        }
        h.deactivate();
      }
    }
  }
}, rn = function() {
  if (this.linked) {
    var t = f(this, ut), n = f(this, xt);
    t === null ? Kn = n : k(t, xt, n), n === null ? Dt = t : k(n, ut, t), this.linked = !1;
  }
};
let dt = Wn;
function A(e) {
  var t = an;
  an = !0;
  try {
    for (var n; ; ) {
      if (cl(), T === null)
        return (
          /** @type {T} */
          n
        );
      T.flush();
    }
  } finally {
    an = t;
  }
}
function kl() {
  try {
    Zs();
  } catch (e) {
    ft(e, lr);
  }
}
let Me = null;
function Ur(e) {
  var t = e.length;
  if (t !== 0) {
    for (var n = 0; n < t; ) {
      var r = e[n++];
      if ((r.f & (pe | ie)) === 0 && yn(r) && (Me = /* @__PURE__ */ new Set(), Jt(r), r.deps === null && r.first === null && r.nodes === null && r.teardown === null && r.ac === null && Zi(r), (Me == null ? void 0 : Me.size) > 0)) {
        Tt.clear();
        for (const i of Me) {
          if ((i.f & (pe | ie)) !== 0) continue;
          const s = [i];
          let l = i.parent;
          for (; l !== null; )
            Me.has(l) && (Me.delete(l), s.push(l)), l = l.parent;
          for (let a = s.length - 1; a >= 0; a--) {
            const o = s[a];
            (o.f & (pe | ie)) === 0 && Jt(o);
          }
        }
        Me.clear();
      }
    }
    Me = null;
  }
}
function Pi(e, t, n, r) {
  if (!n.has(e) && (n.add(e), e.reactions !== null))
    for (const i of e.reactions) {
      const s = i.f;
      (s & ne) !== 0 ? Pi(
        /** @type {Derived} */
        i,
        t,
        n,
        r
      ) : (s & (jt | Re)) !== 0 && (s & Z) === 0 && kr(i, t, r) && (X(i, Z), Tr(
        /** @type {Effect} */
        i
      ));
    }
}
function kr(e, t, n) {
  const r = n.get(e);
  if (r !== void 0) return r;
  if (e.deps !== null)
    for (const i of e.deps) {
      if (On.call(t, i))
        return !0;
      if ((i.f & ne) !== 0 && kr(
        /** @type {Derived} */
        i,
        t,
        n
      ))
        return n.set(
          /** @type {Derived} */
          i,
          !0
        ), !0;
    }
  return n.set(e, !1), !1;
}
function Tr(e) {
  T.schedule(e);
}
function ji(e, t) {
  if (!((e.f & De) !== 0 && (e.f & G) !== 0)) {
    (e.f & Z) !== 0 ? t.d.push(e) : (e.f & qe) !== 0 && t.m.push(e), X(e, G);
    for (var n = e.first; n !== null; )
      ji(n, t), n = n.next;
  }
}
function Hi(e) {
  X(e, G);
  for (var t = e.first; t !== null; )
    Hi(t), t = t.next;
}
let jn = /* @__PURE__ */ new Set();
const Tt = /* @__PURE__ */ new Map();
let Fi = !1;
function At(e, t) {
  var n = {
    f: 0,
    // TODO ideally we could skip this altogether, but it causes type errors
    v: e,
    reactions: null,
    equals: xi,
    rv: 0,
    wv: 0
  };
  return n;
}
// @__NO_SIDE_EFFECTS__
function je(e, t) {
  const n = At(e);
  return ts(n), n;
}
// @__NO_SIDE_EFFECTS__
function Wi(e, t = !1, n = !0) {
  const r = At(e);
  return t || (r.equals = wi), r;
}
function Ne(e, t, n = !1) {
  M !== null && // since we are untracking the function inside `$inspect.with` we need to add this check
  // to ensure we error if state is set inside an inspect effect
  (!Ie || (M.f & In) !== 0) && Ei() && (M.f & (ne | Re | jt | In)) !== 0 && (ze === null || !ze.has(e)) && rl();
  let r = n ? gt(t) : t;
  return Kt(e, r, Cn);
}
function Kt(e, t, n = null) {
  if (!e.equals(t)) {
    Tt.set(e, st ? t : e.v);
    var r = dt.ensure();
    if (r.capture(e, t), (e.f & ne) !== 0) {
      const i = (
        /** @type {Derived} */
        e
      );
      (e.f & Z) !== 0 && $r(i), ee === null && wr(i);
    }
    e.wv = rs(), Yi(e, Z, n), C !== null && (C.f & G) !== 0 && (C.f & (De | it)) === 0 && (_e === null ? Ll([e]) : _e.push(e)), !r.is_fork && jn.size > 0 && !Fi && Tl();
  }
  return t;
}
function Tl() {
  Fi = !1;
  for (const e of jn) {
    (e.f & G) !== 0 && X(e, qe);
    let t;
    try {
      t = yn(e);
    } catch {
      t = !0;
    }
    t && Jt(e);
  }
  jn.clear();
}
function on(e) {
  Ne(e, e.v + 1);
}
function Yi(e, t, n) {
  var r = e.reactions;
  if (r !== null)
    for (var i = r.length, s = 0; s < i; s++) {
      var l = r[s], a = l.f, o = (a & Z) === 0;
      if (o && X(l, t), (a & In) !== 0)
        jn.add(
          /** @type {Effect} */
          l
        );
      else if ((a & ne) !== 0) {
        var u = (
          /** @type {Derived} */
          l
        );
        ee == null || ee.delete(u), (a & Mt) === 0 && (a & Ee && (C === null || (C.f & Dn) === 0) && (l.f |= Mt), Yi(u, qe, n));
      } else if (o) {
        var d = (
          /** @type {Effect} */
          l
        );
        (a & Re) !== 0 && Me !== null && Me.add(d), n !== null ? n.push(d) : Tr(d);
      }
    }
}
function gt(e) {
  if (typeof e != "object" || e === null || kt in e)
    return e;
  const t = vi(e);
  if (t !== Hs && t !== Fs)
    return e;
  var n = /* @__PURE__ */ new Map(), r = yr(e), i = /* @__PURE__ */ je(0), s = Ct, l = (a) => {
    if (Ct === s)
      return a();
    var o = M, u = Ct;
    $e(null), Qr(s);
    var d = a();
    return $e(o), Qr(u), d;
  };
  return r && n.set("length", /* @__PURE__ */ je(
    /** @type {any[]} */
    e.length
  )), new Proxy(
    /** @type {any} */
    e,
    {
      defineProperty(a, o, u) {
        (!("value" in u) || u.configurable === !1 || u.enumerable === !1 || u.writable === !1) && tl();
        var d = n.get(o);
        return d === void 0 ? l(() => {
          var p = /* @__PURE__ */ je(u.value);
          return n.set(o, p), p;
        }) : Ne(d, u.value, !0), !0;
      },
      deleteProperty(a, o) {
        var u = n.get(o);
        if (u === void 0) {
          if (o in a) {
            const d = l(() => /* @__PURE__ */ je(U));
            n.set(o, d), on(i);
          }
        } else
          Ne(u, U), on(i);
        return !0;
      },
      get(a, o, u) {
        var v;
        if (o === kt)
          return e;
        var d = n.get(o), p = o in a;
        if (d === void 0 && (!p || (v = $t(a, o)) != null && v.writable) && (d = l(() => {
          var m = gt(p ? a[o] : U), g = /* @__PURE__ */ je(m);
          return g;
        }), n.set(o, d)), d !== void 0) {
          var h = O(d);
          return h === U ? void 0 : h;
        }
        return Reflect.get(a, o, u);
      },
      getOwnPropertyDescriptor(a, o) {
        var u = Reflect.getOwnPropertyDescriptor(a, o);
        if (u && "value" in u) {
          var d = n.get(o);
          d && (u.value = O(d));
        } else if (u === void 0) {
          var p = n.get(o), h = p == null ? void 0 : p.v;
          if (p !== void 0 && h !== U)
            return {
              enumerable: !0,
              configurable: !0,
              value: h,
              writable: !0
            };
        }
        return u;
      },
      has(a, o) {
        var h;
        if (o === kt)
          return !0;
        var u = n.get(o), d = u !== void 0 && u.v !== U || Reflect.has(a, o);
        if (u !== void 0 || C !== null && (!d || (h = $t(a, o)) != null && h.writable)) {
          u === void 0 && (u = l(() => {
            var v = d ? gt(a[o]) : U, m = /* @__PURE__ */ je(v);
            return m;
          }), n.set(o, u));
          var p = O(u);
          if (p === U)
            return !1;
        }
        return d;
      },
      set(a, o, u, d) {
        var _;
        var p = n.get(o), h = o in a;
        if (r && o === "length")
          for (var v = u; v < /** @type {Source<number>} */
          p.v; v += 1) {
            var m = n.get(v + "");
            m !== void 0 ? Ne(m, U) : v in a && (m = l(() => /* @__PURE__ */ je(U)), n.set(v + "", m));
          }
        if (p === void 0)
          (!h || (_ = $t(a, o)) != null && _.writable) && (p = l(() => /* @__PURE__ */ je(void 0)), Ne(p, gt(u)), n.set(o, p));
        else {
          h = p.v !== U;
          var g = l(() => gt(u));
          Ne(p, g);
        }
        var c = Reflect.getOwnPropertyDescriptor(a, o);
        if (c != null && c.set && c.set.call(d, u), !h) {
          if (r && typeof o == "string") {
            var b = (
              /** @type {Source<number>} */
              n.get("length")
            ), x = Number(o);
            Number.isInteger(x) && x >= b.v && Ne(b, x + 1);
          }
          on(i);
        }
        return !0;
      },
      ownKeys(a) {
        O(i);
        var o = Reflect.ownKeys(a).filter((p) => {
          var h = n.get(p);
          return h === void 0 || h.v !== U;
        });
        for (var [u, d] of n)
          d.v !== U && !(u in a) && o.push(u);
        return o;
      },
      setPrototypeOf() {
        nl();
      }
    }
  );
}
function Gr(e) {
  try {
    if (e !== null && typeof e == "object" && kt in e)
      return e[kt];
  } catch {
  }
  return e;
}
function Sl(e, t) {
  return Object.is(Gr(e), Gr(t));
}
var Kr, zi, Bi, qi;
function ur() {
  if (Kr === void 0) {
    Kr = window, zi = /Firefox/.test(navigator.userAgent);
    var e = Element.prototype, t = Node.prototype, n = Text.prototype;
    Bi = $t(t, "firstChild").get, qi = $t(t, "nextSibling").get, Xr(e) && (e[nr] = void 0, e[kn] = null, e[Xs] = void 0, e.__e = void 0), Xr(n) && (n[en] = void 0);
  }
}
function Ye(e = "") {
  return document.createTextNode(e);
}
// @__NO_SIDE_EFFECTS__
function fn(e) {
  return (
    /** @type {TemplateNode | null} */
    Bi.call(e)
  );
}
// @__NO_SIDE_EFFECTS__
function lt(e) {
  return (
    /** @type {TemplateNode | null} */
    qi.call(e)
  );
}
function B(e, t) {
  if (!D)
    return /* @__PURE__ */ fn(e);
  var n = /* @__PURE__ */ fn(j);
  if (n === null)
    n = j.appendChild(Ye());
  else if (t && n.nodeType !== xr) {
    var r = Ye();
    return n == null || n.before(r), ge(r), r;
  }
  return t && Ui(
    /** @type {Text} */
    n
  ), ge(n), n;
}
function me(e, t = 1, n = !1) {
  let r = D ? j : e;
  for (var i; t--; )
    i = r, r = /** @type {TemplateNode} */
    /* @__PURE__ */ lt(r);
  if (!D)
    return r;
  if (n) {
    if ((r == null ? void 0 : r.nodeType) !== xr) {
      var s = Ye();
      return r === null ? i == null || i.after(s) : r.before(s), ge(s), s;
    }
    Ui(
      /** @type {Text} */
      r
    );
  }
  return ge(r), r;
}
function Xi(e) {
  e.textContent = "";
}
function Vi() {
  return !1;
}
function Sr(e, t, n) {
  return (
    /** @type {T extends keyof HTMLElementTagNameMap ? HTMLElementTagNameMap[T] : Element} */
    n ? document.createElement(e, { is: n }) : document.createElement(e)
  );
}
function Ui(e) {
  if (
    /** @type {string} */
    e.nodeValue.length < 65536
  )
    return;
  let t = e.nextSibling;
  for (; t !== null && t.nodeType === xr; )
    t.remove(), e.nodeValue += /** @type {string} */
    t.nodeValue, t = e.nextSibling;
}
let Jr = !1;
function Cl() {
  Jr || (Jr = !0, document.addEventListener(
    "reset",
    (e) => {
      Promise.resolve().then(() => {
        var t;
        if (!e.defaultPrevented)
          for (
            const n of
            /**@type {HTMLFormElement} */
            e.target.elements
          )
            (t = n[_i]) == null || t.call(n);
      });
    },
    // In the capture phase to guarantee we get noticed of it (no possibility of stopPropagation)
    { capture: !0 }
  ));
}
function Cr(e) {
  var t = M, n = C;
  $e(null), Xe(null);
  try {
    return e();
  } finally {
    $e(t), Xe(n);
  }
}
function Ml(e) {
  C === null && (M === null && Js(), Ks()), st && Gs();
}
function Al(e, t) {
  var n = t.last;
  n === null ? t.last = t.first = e : (n.next = e, e.prev = n, t.last = e);
}
function Ve(e, t) {
  var n = C;
  n !== null && (n.f & ie) !== 0 && (e |= ie);
  var r = {
    ctx: se,
    deps: null,
    nodes: null,
    f: e | Z | Ee,
    first: null,
    fn: t,
    last: null,
    next: null,
    parent: n,
    b: n && n.b,
    prev: null,
    teardown: null,
    wv: 0,
    ac: null
  };
  T == null || T.register_created_effect(r);
  var i = r;
  if ((e & Vt) !== 0)
    Pt !== null ? Pt.push(r) : dt.ensure().schedule(r);
  else if (t !== null) {
    try {
      Jt(r);
    } catch (l) {
      throw le(r), l;
    }
    i.deps === null && i.teardown === null && i.nodes === null && i.first === i.last && // either `null`, or a singular child
    (i.f & Nt) === 0 && (i = i.first, (e & Re) !== 0 && (e & Ut) !== 0 && i !== null && (i.f |= Ut));
  }
  if (i !== null && (i.parent = n, n !== null && Al(i, n), M !== null && (M.f & ne) !== 0 && (e & it) === 0)) {
    var s = (
      /** @type {Derived} */
      M
    );
    (s.effects ?? (s.effects = [])).push(i);
  }
  return r;
}
function Mr() {
  return M !== null && !Ie;
}
function Ar(e) {
  const t = Ve(Bn, null);
  return X(t, G), t.teardown = e, t;
}
function Or(e) {
  Ml();
  var t = (
    /** @type {Effect} */
    C.f
  ), n = !M && (t & De) !== 0 && se !== null && !se.i;
  if (n) {
    var r = (
      /** @type {ComponentContext} */
      se
    );
    (r.e ?? (r.e = [])).push(e);
  } else
    return Gi(e);
}
function Gi(e) {
  return Ve(Vt | Bs, e);
}
function Ol(e) {
  dt.ensure();
  const t = Ve(it | Nt, e);
  return () => {
    le(t);
  };
}
function Nl(e) {
  dt.ensure();
  const t = Ve(it | Nt, e);
  return (n = {}) => new Promise((r) => {
    n.outro ? St(t, () => {
      le(t), r(void 0);
    }) : (le(t), r(void 0));
  });
}
function Ki(e) {
  return Ve(Vt, e);
}
function Rl(e) {
  return Ve(jt | Nt, e);
}
function Nr(e, t = 0) {
  return Ve(Bn | t, e);
}
function te(e, t = [], n = [], r = []) {
  _l(r, t, n, (i) => {
    Ve(Bn, () => {
      e(...i.map(O));
    });
  });
}
function Rr(e, t = 0) {
  var n = Ve(Re | t, e);
  return n;
}
function we(e) {
  return Ve(De | Nt, e);
}
function Ji(e) {
  var t = e.teardown;
  if (t !== null) {
    const n = st, r = M;
    Zr(!0), $e(null);
    try {
      t.call(null);
    } finally {
      Zr(n), $e(r);
    }
  }
}
function Ir(e, t = !1) {
  var n = e.first;
  for (e.first = e.last = null; n !== null; ) {
    const i = n.ac;
    i !== null && Cr(() => {
      i.abort(qn);
    });
    var r = n.next;
    (n.f & it) !== 0 ? n.parent = null : le(n, t), n = r;
  }
}
function Il(e) {
  for (var t = e.first; t !== null; ) {
    var n = t.next;
    (t.f & De) === 0 && le(t), t = n;
  }
}
function le(e, t = !0) {
  var n = !1;
  (t || (e.f & zs) !== 0) && e.nodes !== null && e.nodes.end !== null && (Dl(
    e.nodes.start,
    /** @type {TemplateNode} */
    e.nodes.end
  ), n = !0), e.f |= tr, Ir(e, t && !n), cn(e, 0);
  var r = e.nodes && e.nodes.t;
  if (r !== null)
    for (const s of r)
      s.stop();
  Ji(e), e.f ^= tr, e.f |= pe;
  var i = e.parent;
  i !== null && i.first !== null && Zi(e), e.next = e.prev = e.teardown = e.ctx = e.deps = e.fn = e.nodes = e.ac = e.b = null;
}
function Dl(e, t) {
  for (; e !== null; ) {
    var n = e === t ? null : /* @__PURE__ */ lt(e);
    e.remove(), e = n;
  }
}
function Zi(e) {
  var t = e.parent, n = e.prev, r = e.next;
  n !== null && (n.next = r), r !== null && (r.prev = n), t !== null && (t.first === e && (t.first = r), t.last === e && (t.last = n));
}
function St(e, t, n = !0) {
  var r = [];
  Qi(e, r, !0);
  var i = () => {
    n && le(e), t && t();
  }, s = r.length;
  if (s > 0) {
    var l = () => --s || i();
    for (var a of r)
      a.out(l);
  } else
    i();
}
function Qi(e, t, n) {
  if ((e.f & ie) === 0) {
    e.f ^= ie;
    var r = e.nodes && e.nodes.t;
    if (r !== null)
      for (const a of r)
        (a.is_global || n) && t.push(a);
    for (var i = e.first; i !== null; ) {
      var s = i.next;
      if ((i.f & it) === 0) {
        var l = (i.f & Ut) !== 0 || // If this is a branch effect without a block effect parent,
        // it means the parent block effect was pruned. In that case,
        // transparency information was transferred to the branch effect.
        (i.f & De) !== 0 && (e.f & Re) !== 0;
        Qi(i, t, l ? n : !1);
      }
      i = s;
    }
  }
}
function Hn(e) {
  es(e, !0);
}
function es(e, t) {
  if ((e.f & ie) !== 0) {
    e.f ^= ie, (e.f & G) === 0 && (X(e, Z), dt.ensure().schedule(e));
    for (var n = e.first; n !== null; ) {
      var r = n.next, i = (n.f & Ut) !== 0 || (n.f & De) !== 0;
      es(n, i ? t : !1), n = r;
    }
    var s = e.nodes && e.nodes.t;
    if (s !== null)
      for (const l of s)
        (l.is_global || t) && l.in();
  }
}
function Dr(e, t) {
  if (e.nodes)
    for (var n = e.nodes.start, r = e.nodes.end; n !== null; ) {
      var i = n === r ? null : /* @__PURE__ */ lt(n);
      t.append(n), n = i;
    }
}
let Mn = !1, st = !1;
function Zr(e) {
  st = e;
}
let M = null, Ie = !1;
function $e(e) {
  M = e;
}
let C = null;
function Xe(e) {
  C = e;
}
let ze = null;
function ts(e) {
  M !== null && (ze ?? (ze = /* @__PURE__ */ new Set())).add(e);
}
let fe = null, ce = 0, _e = null;
function Ll(e) {
  _e = e;
}
let ns = 1, mt = 0, Ct = mt;
function Qr(e) {
  Ct = e;
}
function rs() {
  return ++ns;
}
function yn(e) {
  var t = e.f;
  if ((t & Z) !== 0)
    return !0;
  if (t & ne && (e.f &= ~Mt), (t & qe) !== 0) {
    for (var n = (
      /** @type {Value[]} */
      e.deps
    ), r = n.length, i = 0; i < r; i++) {
      var s = n[i];
      if (yn(
        /** @type {Derived} */
        s
      ) && Ri(
        /** @type {Derived} */
        s
      ), s.wv > e.wv)
        return !0;
    }
    (t & Ee) !== 0 && // During time traveling we don't want to reset the status so that
    // traversal of the graph in the other batches still happens
    ee === null && X(e, G);
  }
  return !1;
}
function is(e, t, n = !0) {
  var r = e.reactions;
  if (r !== null && !(ze !== null && ze.has(e)))
    for (var i = 0; i < r.length; i++) {
      var s = r[i];
      (s.f & ne) !== 0 ? is(
        /** @type {Derived} */
        s,
        t,
        !1
      ) : t === s && (n ? X(s, Z) : (s.f & G) !== 0 && X(s, qe), Tr(
        /** @type {Effect} */
        s
      ));
    }
}
function ss(e) {
  var g;
  var t = fe, n = ce, r = _e, i = M, s = ze, l = se, a = Ie, o = Ct, u = e.f;
  fe = /** @type {null | Value[]} */
  null, ce = 0, _e = null, M = (u & (De | it)) === 0 ? e : null, ze = null, Gt(e.ctx), Ie = !1, Ct = ++mt, e.ac !== null && (Cr(() => {
    e.ac.abort(qn);
  }), e.ac = null);
  try {
    e.f |= Dn;
    var d = (
      /** @type {Function} */
      e.fn
    ), p = d();
    e.f |= Ot;
    var h = e.deps, v = T == null ? void 0 : T.is_fork;
    if (fe !== null) {
      var m;
      if (v || cn(e, ce), h !== null && ce > 0)
        for (h.length = ce + fe.length, m = 0; m < fe.length; m++)
          h[ce + m] = fe[m];
      else
        e.deps = h = fe;
      if (Mr() && (e.f & Ee) !== 0)
        for (m = ce; m < h.length; m++)
          ((g = h[m]).reactions ?? (g.reactions = [])).push(e);
    } else !v && h !== null && ce < h.length && (cn(e, ce), h.length = ce);
    if (Ei() && _e !== null && !Ie && h !== null && (e.f & (ne | qe | Z)) === 0)
      for (m = 0; m < /** @type {Source[]} */
      _e.length; m++)
        is(
          _e[m],
          /** @type {Effect} */
          e
        );
    if (i !== null && i !== e) {
      if (mt++, i.deps !== null)
        for (let c = 0; c < n; c += 1)
          i.deps[c].rv = mt;
      if (t !== null)
        for (const c of t)
          c.rv = mt;
      _e !== null && (r === null ? r = _e : r.push(.../** @type {Source[]} */
      _e));
    }
    return (e.f & ct) !== 0 && (e.f ^= ct), p;
  } catch (c) {
    return ki(c);
  } finally {
    e.f ^= Dn, fe = t, ce = n, _e = r, M = i, ze = s, Gt(l), Ie = a, Ct = o;
  }
}
function Pl(e, t) {
  let n = t.reactions;
  if (n !== null) {
    var r = Ps.call(n, e);
    if (r !== -1) {
      var i = n.length - 1;
      i === 0 ? n = t.reactions = null : (n[r] = n[i], n.pop());
    }
  }
  if (n === null && (t.f & ne) !== 0 && // Destroying a child effect while updating a parent effect can cause a dependency to appear
  // to be unused, when in fact it is used by the currently-updating parent. Checking `new_deps`
  // allows us to skip the expensive work of disconnecting and immediately reconnecting it
  (fe === null || !On.call(fe, t))) {
    var s = (
      /** @type {Derived} */
      t
    );
    (s.f & Ee) !== 0 && (s.f ^= Ee, s.f &= ~Mt), s.v !== U && wr(s), wl(s), cn(s, 0);
  }
}
function cn(e, t) {
  var n = e.deps;
  if (n !== null)
    for (var r = t; r < n.length; r++)
      Pl(e, n[r]);
}
function Jt(e) {
  var t = e.f;
  if ((t & pe) === 0) {
    X(e, G);
    var n = C, r = Mn;
    C = e, Mn = !0;
    try {
      (t & (Re | gi)) !== 0 ? Il(e) : Ir(e), Ji(e);
      var i = ss(e);
      e.teardown = typeof i == "function" ? i : null, e.wv = ns;
      var s;
      hi && fl && (e.f & Z) !== 0 && e.deps;
    } finally {
      Mn = r, C = n;
    }
  }
}
function O(e) {
  var t = e.f, n = (t & ne) !== 0;
  if (M !== null && !Ie) {
    var r = C !== null && (C.f & pe) !== 0;
    if (!r && (ze === null || !ze.has(e))) {
      var i = M.deps;
      if ((M.f & Dn) !== 0)
        e.rv < mt && (e.rv = mt, fe === null && i !== null && i[ce] === e ? ce++ : fe === null ? fe = [e] : fe.push(e));
      else {
        M.deps ?? (M.deps = []), On.call(M.deps, e) || M.deps.push(e);
        var s = e.reactions;
        s === null ? e.reactions = [M] : On.call(s, M) || s.push(M);
      }
    }
  }
  if (st && Tt.has(e))
    return Tt.get(e);
  if (n) {
    var l = (
      /** @type {Derived} */
      e
    );
    if (st) {
      var a = l.v;
      return ((l.f & G) === 0 && l.reactions !== null || as(l)) && (a = $r(l)), Tt.set(l, a), a;
    }
    var o = (l.f & Ee) === 0 && !Ie && M !== null && (Mn || (M.f & Ee) !== 0), u = (l.f & Ot) === 0;
    yn(l) && (o && (l.f |= Ee), Ri(l)), o && !u && (Ii(l), ls(l));
  }
  if (ee != null && ee.has(e))
    return ee.get(e);
  if ((e.f & ct) !== 0)
    throw e.v;
  return e.v;
}
function ls(e) {
  if (e.f |= Ee, e.deps !== null)
    for (const t of e.deps)
      (t.reactions ?? (t.reactions = [])).push(e), (t.f & ne) !== 0 && (t.f & Ee) === 0 && (Ii(
        /** @type {Derived} */
        t
      ), ls(
        /** @type {Derived} */
        t
      ));
}
function as(e) {
  if (e.v === U) return !0;
  if (e.deps === null) return !1;
  for (const t of e.deps)
    if (Tt.has(t) || (t.f & ne) !== 0 && as(
      /** @type {Derived} */
      t
    ))
      return !0;
  return !1;
}
function Lr(e) {
  var t = Ie;
  try {
    return Ie = !0, e();
  } finally {
    Ie = t;
  }
}
const _t = Symbol("events"), os = /* @__PURE__ */ new Set(), fr = /* @__PURE__ */ new Set();
function jl(e, t, n, r = {}) {
  function i(s) {
    if (r.capture || cr.call(t, s), !s.cancelBubble)
      return Cr(() => n == null ? void 0 : n.call(this, s));
  }
  return rt(() => {
    t.addEventListener(e, i, r);
  }), i;
}
function us(e, t, n, r, i) {
  var s = { capture: r, passive: i }, l = jl(e, t, n, s);
  (t === document.body || // @ts-ignore
  t === window || // @ts-ignore
  t === document || // Firefox has quirky behavior, it can happen that we still get "canplay" events when the element is already removed
  t instanceof HTMLMediaElement) && Ar(() => {
    t.removeEventListener(e, l, s);
  });
}
function J(e, t, n) {
  (t[_t] ?? (t[_t] = {}))[e] = n;
}
function ht(e) {
  for (var t = 0; t < e.length; t++)
    os.add(e[t]);
  for (var n of fr)
    n(e);
}
let ei = null;
function cr(e) {
  var g, c;
  var t = this, n = (
    /** @type {Node} */
    t.ownerDocument
  ), r = e.type, i = ((g = e.composedPath) == null ? void 0 : g.call(e)) || [], s = (
    /** @type {null | Element} */
    i[0] || e.target
  );
  ei = e;
  var l = 0, a = ei === e && e[_t];
  if (a) {
    var o = i.indexOf(a);
    if (o !== -1 && (t === document || t === /** @type {any} */
    window)) {
      e[_t] = t;
      return;
    }
    var u = i.indexOf(t);
    if (u === -1)
      return;
    o <= u && (l = o);
  }
  if (s = /** @type {Element} */
  i[l] || e.target, s !== t) {
    Rn(e, "currentTarget", {
      configurable: !0,
      get() {
        return s || n;
      }
    });
    var d = M, p = C;
    $e(null), Xe(null);
    try {
      for (var h, v = []; s !== null && s !== t; ) {
        try {
          var m = (c = s[_t]) == null ? void 0 : c[r];
          m != null && (!/** @type {any} */
          s.disabled || // DOM could've been updated already by the time this is reached, so we check this as well
          // -> the target could not have been disabled because it emits the event in the first place
          e.target === s) && m.call(s, e);
        } catch (b) {
          h ? v.push(b) : h = b;
        }
        if (e.cancelBubble) break;
        l++, s = l < i.length ? (
          /** @type {Element} */
          i[l]
        ) : null;
      }
      if (h) {
        for (let b of v)
          queueMicrotask(() => {
            throw b;
          });
        throw h;
      }
    } finally {
      e[_t] = t, delete e.currentTarget, $e(d), Xe(p);
    }
  }
}
var fi;
const Zn = (
  // We gotta write it like this because after downleveling the pure comment may end up in the wrong location
  ((fi = globalThis == null ? void 0 : globalThis.window) == null ? void 0 : fi.trustedTypes) && /* @__PURE__ */ globalThis.window.trustedTypes.createPolicy("svelte-trusted-html", {
    /** @param {string} html */
    createHTML: (e) => e
  })
);
function Hl(e) {
  return (
    /** @type {string} */
    (Zn == null ? void 0 : Zn.createHTML(e)) ?? e
  );
}
function Fl(e) {
  var t = Sr("template");
  return t.innerHTML = Hl(e.replaceAll("<!>", "<!---->")), t.content;
}
function dr(e, t) {
  var n = (
    /** @type {Effect} */
    C
  );
  n.nodes === null && (n.nodes = { start: e, end: t, a: null, t: null });
}
// @__NO_SIDE_EFFECTS__
function K(e, t) {
  var n = (t & Ds) !== 0, r, i = !e.startsWith("<!>");
  return () => {
    if (D)
      return dr(j, null), j;
    r === void 0 && (r = Fl(i ? e : "<!>" + e), r = /** @type {TemplateNode} */
    /* @__PURE__ */ fn(r));
    var s = (
      /** @type {TemplateNode} */
      n || zi ? document.importNode(r, !0) : r.cloneNode(!0)
    );
    return dr(s, s), s;
  };
}
function V(e, t) {
  if (D) {
    var n = (
      /** @type {Effect & { nodes: EffectNodes }} */
      C
    );
    ((n.f & Ot) === 0 || n.nodes.end === null) && (n.nodes.end = j), bn();
    return;
  }
  e !== null && e.before(
    /** @type {Node} */
    t
  );
}
const Wl = ["touchstart", "touchmove"];
function Yl(e) {
  return Wl.includes(e);
}
function ke(e, t) {
  var n = t == null ? "" : typeof t == "object" ? `${t}` : t;
  n !== /** @type {any} */
  (e[en] ?? (e[en] = e.nodeValue)) && (e[en] = n, e.nodeValue = `${n}`);
}
function fs(e, t) {
  return cs(e, t);
}
function zl(e, t) {
  ur(), t.intro = t.intro ?? !1;
  const n = t.target, r = D, i = j;
  try {
    for (var s = /* @__PURE__ */ fn(n); s && (s.nodeType !== _n || /** @type {Comment} */
    s.data !== di); )
      s = /* @__PURE__ */ lt(s);
    if (!s)
      throw Xt;
    nt(!0), ge(
      /** @type {Comment} */
      s
    );
    const l = cs(e, { ...t, anchor: s });
    return nt(!1), /**  @type {Exports} */
    l;
  } catch (l) {
    if (l instanceof Error && l.message.split(`
`).some((a) => a.startsWith("https://svelte.dev/e/")))
      throw l;
    return l !== Xt && console.warn("Failed to hydrate: ", l), t.recover === !1 && Qs(), ur(), Xi(n), nt(!1), fs(e, t);
  } finally {
    nt(r), ge(i);
  }
}
const $n = /* @__PURE__ */ new Map();
function cs(e, { target: t, anchor: n, props: r = {}, events: i, context: s, intro: l = !0, transformError: a }) {
  ur();
  var o = void 0, u = Nl(() => {
    var d = n ?? t.appendChild(Ye());
    gl(
      /** @type {TemplateNode} */
      d,
      {
        pending: () => {
        }
      },
      (v) => {
        Se({});
        var m = (
          /** @type {ComponentContext} */
          se
        );
        if (s && (m.c = s), i && (r.$$events = i), D && dr(
          /** @type {TemplateNode} */
          v,
          null
        ), o = e(v, r) || {}, D && (C.nodes.end = j, j === null || j.nodeType !== _n || /** @type {Comment} */
        j.data !== br))
          throw Xn(), Xt;
        Ce();
      },
      a
    );
    var p = /* @__PURE__ */ new Set(), h = (v) => {
      for (var m = 0; m < v.length; m++) {
        var g = v[m];
        if (!p.has(g)) {
          p.add(g);
          var c = Yl(g);
          for (const _ of [t, document]) {
            var b = $n.get(_);
            b === void 0 && (b = /* @__PURE__ */ new Map(), $n.set(_, b));
            var x = b.get(g);
            x === void 0 ? (_.addEventListener(g, cr, { passive: c }), b.set(g, 1)) : b.set(g, x + 1);
          }
        }
      }
    };
    return h(zn(os)), fr.add(h), () => {
      var c;
      for (var v of p)
        for (const b of [t, document]) {
          var m = (
            /** @type {Map<string, number>} */
            $n.get(b)
          ), g = (
            /** @type {number} */
            m.get(v)
          );
          --g == 0 ? (b.removeEventListener(v, cr), m.delete(v), m.size === 0 && $n.delete(b)) : m.set(v, g);
        }
      fr.delete(h), d !== n && ((c = d.parentNode) == null || c.removeChild(d));
    };
  });
  return hr.set(o, u), o;
}
let hr = /* @__PURE__ */ new WeakMap();
function Bl(e, t) {
  const n = hr.get(e);
  return n ? (hr.delete(e), n(t)) : Promise.resolve();
}
var Oe, Fe, ve, Et, gn, mn, Yn;
class ql {
  /**
   * @param {TemplateNode} anchor
   * @param {boolean} transition
   */
  constructor(t, n = !0) {
    /** @type {TemplateNode} */
    W(this, "anchor");
    /** @type {Map<Batch, Key>} */
    S(this, Oe, /* @__PURE__ */ new Map());
    /**
     * Map of keys to effects that are currently rendered in the DOM.
     * These effects are visible and actively part of the document tree.
     * Example:
     * ```
     * {#if condition}
     * 	foo
     * {:else}
     * 	bar
     * {/if}
     * ```
     * Can result in the entries `true->Effect` and `false->Effect`
     * @type {Map<Key, Effect>}
     */
    S(this, Fe, /* @__PURE__ */ new Map());
    /**
     * Similar to #onscreen with respect to the keys, but contains branches that are not yet
     * in the DOM, because their insertion is deferred.
     * @type {Map<Key, Branch>}
     */
    S(this, ve, /* @__PURE__ */ new Map());
    /**
     * Keys of effects that are currently outroing
     * @type {Set<Key>}
     */
    S(this, Et, /* @__PURE__ */ new Set());
    /**
     * Whether to pause (i.e. outro) on change, or destroy immediately.
     * This is necessary for `<svelte:element>`
     */
    S(this, gn, !0);
    /**
     * @param {Batch} batch
     */
    S(this, mn, (t) => {
      if (f(this, Oe).has(t)) {
        var n = (
          /** @type {Key} */
          f(this, Oe).get(t)
        ), r = f(this, Fe).get(n);
        if (r)
          Hn(r), f(this, Et).delete(n);
        else {
          var i = f(this, ve).get(n);
          i && (Hn(i.effect), f(this, Fe).set(n, i.effect), f(this, ve).delete(n), i.fragment.lastChild.remove(), this.anchor.before(i.fragment), r = i.effect);
        }
        for (const [s, l] of f(this, Oe)) {
          if (f(this, Oe).delete(s), s === t)
            break;
          const a = f(this, ve).get(l);
          a && (le(a.effect), f(this, ve).delete(l));
        }
        for (const [s, l] of f(this, Fe)) {
          if (s === n || f(this, Et).has(s)) continue;
          const a = () => {
            if (Array.from(f(this, Oe).values()).includes(s)) {
              var u = document.createDocumentFragment();
              Dr(l, u), u.append(Ye()), f(this, ve).set(s, { effect: l, fragment: u });
            } else
              le(l);
            f(this, Et).delete(s), f(this, Fe).delete(s);
          };
          f(this, gn) || !r ? (f(this, Et).add(s), St(l, a, !1)) : a();
        }
      }
    });
    /**
     * @param {Batch} batch
     */
    S(this, Yn, (t) => {
      f(this, Oe).delete(t);
      const n = Array.from(f(this, Oe).values());
      for (const [r, i] of f(this, ve))
        n.includes(r) || (le(i.effect), f(this, ve).delete(r));
    });
    this.anchor = t, k(this, gn, n);
  }
  /**
   *
   * @param {any} key
   * @param {null | ((target: TemplateNode) => void)} fn
   */
  ensure(t, n) {
    var r = (
      /** @type {Batch} */
      T
    ), i = Vi();
    if (n && !f(this, Fe).has(t) && !f(this, ve).has(t))
      if (i) {
        var s = document.createDocumentFragment(), l = Ye();
        s.append(l), f(this, ve).set(t, {
          effect: we(() => n(l)),
          fragment: s
        });
      } else
        f(this, Fe).set(
          t,
          we(() => n(this.anchor))
        );
    if (f(this, Oe).set(r, t), i) {
      for (const [a, o] of f(this, Fe))
        a === t ? r.unskip_effect(o) : r.skip_effect(o);
      for (const [a, o] of f(this, ve))
        a === t ? r.unskip_effect(o.effect) : r.skip_effect(o.effect);
      r.oncommit(f(this, mn)), r.ondiscard(f(this, Yn));
    } else
      D && (this.anchor = j), f(this, mn).call(this, r);
  }
}
Oe = new WeakMap(), Fe = new WeakMap(), ve = new WeakMap(), Et = new WeakMap(), gn = new WeakMap(), mn = new WeakMap(), Yn = new WeakMap();
function Zt(e, t, n = !1) {
  var r;
  D && (r = j, bn());
  var i = new ql(e), s = n ? Ut : 0;
  function l(a, o) {
    if (D) {
      var u = yi(
        /** @type {TemplateNode} */
        r
      );
      if (a !== parseInt(u.substring(1))) {
        var d = Ln();
        ge(d), i.anchor = d, nt(!1), i.ensure(a, o), nt(!0);
        return;
      }
    }
    i.ensure(a, o);
  }
  Rr(() => {
    var a = !1;
    t((o, u = 0) => {
      a = !0, l(u, o);
    }), a || l(-1, null);
  }, s);
}
function ds(e, t) {
  return t;
}
function Xl(e, t, n) {
  for (var r = [], i = t.length, s, l = t.length, a = 0; a < i; a++) {
    let p = t[a];
    St(
      p,
      () => {
        if (s) {
          if (s.pending.delete(p), s.done.add(p), s.pending.size === 0) {
            var h = (
              /** @type {Set<EachOutroGroup>} */
              e.outrogroups
            );
            vr(e, zn(s.done)), h.delete(s), h.size === 0 && (e.outrogroups = null);
          }
        } else
          l -= 1;
      },
      !1
    );
  }
  if (l === 0) {
    var o = r.length === 0 && n !== null;
    if (o) {
      var u = (
        /** @type {Element} */
        n
      ), d = (
        /** @type {Element} */
        u.parentNode
      );
      Xi(d), d.append(u), e.items.clear();
    }
    vr(e, t, !o);
  } else
    s = {
      pending: new Set(t),
      done: /* @__PURE__ */ new Set()
    }, (e.outrogroups ?? (e.outrogroups = /* @__PURE__ */ new Set())).add(s);
}
function vr(e, t, n = !0) {
  var r;
  if (e.pending.size > 0) {
    r = /* @__PURE__ */ new Set();
    for (const l of e.pending.values())
      for (const a of l)
        r.add(
          /** @type {EachItem} */
          e.items.get(a).e
        );
  }
  for (var i = 0; i < t.length; i++) {
    var s = t[i];
    if (r != null && r.has(s)) {
      s.f |= We;
      const l = document.createDocumentFragment();
      Dr(s, l);
    } else
      le(t[i], n);
  }
}
var ti;
function hs(e, t, n, r, i, s = null) {
  var l = e, a = /* @__PURE__ */ new Map(), o = (t & ci) !== 0;
  if (o) {
    var u = (
      /** @type {Element} */
      e
    );
    l = D ? ge(/* @__PURE__ */ fn(u)) : u.appendChild(Ye());
  }
  D && bn();
  var d = null, p = /* @__PURE__ */ Ni(() => {
    var _ = n();
    return (
      /** @type {V[]} */
      yr(_) ? _ : _ == null ? [] : zn(_)
    );
  }), h, v = /* @__PURE__ */ new Map(), m = !0;
  function g(_) {
    (x.effect.f & pe) === 0 && (x.pending.delete(_), x.fallback = d, Vl(x, h, l, t, r), d !== null && (h.length === 0 ? (d.f & We) === 0 ? Hn(d) : (d.f ^= We, sn(d, null, l)) : St(d, () => {
      d = null;
    })));
  }
  function c(_) {
    x.pending.delete(_);
  }
  var b = Rr(() => {
    h = /** @type {V[]} */
    O(p);
    var _ = h.length;
    let E = !1;
    if (D) {
      var y = yi(l) === _r;
      y !== (_ === 0) && (l = Ln(), ge(l), nt(!1), E = !0);
    }
    for (var $ = /* @__PURE__ */ new Set(), I = (
      /** @type {Batch} */
      T
    ), z = Vi(), P = 0; P < _; P += 1) {
      D && j.nodeType === _n && /** @type {Comment} */
      j.data === br && (l = /** @type {Comment} */
      j, E = !0, nt(!1));
      var H = h[P], q = r(H, P), Q = m ? null : a.get(q);
      Q ? (Q.v && Kt(Q.v, H), Q.i && Kt(Q.i, P), z && I.unskip_effect(Q.e)) : (Q = Ul(
        a,
        m ? l : ti ?? (ti = Ye()),
        H,
        q,
        P,
        i,
        t,
        n
      ), m || (Q.e.f |= We), a.set(q, Q)), $.add(q);
    }
    if (_ === 0 && s && !d && (m ? d = we(() => s(l)) : (d = we(() => s(ti ?? (ti = Ye()))), d.f |= We)), _ > $.size && Us(), D && _ > 0 && ge(Ln()), !m)
      if (v.set(I, $), z) {
        for (const [vt, It] of a)
          $.has(vt) || I.skip_effect(It.e);
        I.oncommit(g), I.ondiscard(c);
      } else
        g(I);
    E && nt(!0), O(p);
  }), x = { effect: b, items: a, pending: v, outrogroups: null, fallback: d };
  m = !1, D && (l = j);
}
function Qt(e) {
  for (; e !== null && (e.f & De) === 0; )
    e = e.next;
  return e;
}
function Vl(e, t, n, r, i) {
  var H, q, Q, vt, It, Ge, w, ae, Yr;
  var s = (r & Ms) !== 0, l = t.length, a = e.items, o = Qt(e.effect.first), u, d = null, p, h = [], v = [], m, g, c, b;
  if (s)
    for (b = 0; b < l; b += 1)
      m = t[b], g = i(m, b), c = /** @type {EachItem} */
      a.get(g).e, (c.f & We) === 0 && ((q = (H = c.nodes) == null ? void 0 : H.a) == null || q.measure(), (p ?? (p = /* @__PURE__ */ new Set())).add(c));
  for (b = 0; b < l; b += 1) {
    if (m = t[b], g = i(m, b), c = /** @type {EachItem} */
    a.get(g).e, e.outrogroups !== null)
      for (const Ke of e.outrogroups)
        Ke.pending.delete(c), Ke.done.delete(c);
    if ((c.f & ie) !== 0 && (Hn(c), s && ((vt = (Q = c.nodes) == null ? void 0 : Q.a) == null || vt.unfix(), (p ?? (p = /* @__PURE__ */ new Set())).delete(c))), (c.f & We) !== 0)
      if (c.f ^= We, c === o)
        sn(c, null, n);
      else {
        var x = d ? d.next : o;
        c === e.effect.last && (e.effect.last = c.prev), c.prev && (c.prev.next = c.next), c.next && (c.next.prev = c.prev), at(e, d, c), at(e, c, x), sn(c, x, n), d = c, h = [], v = [], o = Qt(d.next);
        continue;
      }
    if (c !== o) {
      if (u !== void 0 && u.has(c)) {
        if (h.length < v.length) {
          var _ = v[0], E;
          d = _.prev;
          var y = h[0], $ = h[h.length - 1];
          for (E = 0; E < h.length; E += 1)
            sn(h[E], _, n);
          for (E = 0; E < v.length; E += 1)
            u.delete(v[E]);
          at(e, y.prev, $.next), at(e, d, y), at(e, $, _), o = _, d = $, b -= 1, h = [], v = [];
        } else
          u.delete(c), sn(c, o, n), at(e, c.prev, c.next), at(e, c, d === null ? e.effect.first : d.next), at(e, d, c), d = c;
        continue;
      }
      for (h = [], v = []; o !== null && o !== c; )
        (u ?? (u = /* @__PURE__ */ new Set())).add(o), v.push(o), o = Qt(o.next);
      if (o === null)
        continue;
    }
    (c.f & We) === 0 && h.push(c), d = c, o = Qt(c.next);
  }
  if (e.outrogroups !== null) {
    for (const Ke of e.outrogroups)
      Ke.pending.size === 0 && (vr(e, zn(Ke.done)), (It = e.outrogroups) == null || It.delete(Ke));
    e.outrogroups.size === 0 && (e.outrogroups = null);
  }
  if (o !== null || u !== void 0) {
    var I = [];
    if (u !== void 0)
      for (c of u)
        (c.f & ie) === 0 && I.push(c);
    for (; o !== null; )
      (o.f & ie) === 0 && o !== e.fallback && I.push(o), o = Qt(o.next);
    var z = I.length;
    if (z > 0) {
      var P = (r & ci) !== 0 && l === 0 ? n : null;
      if (s) {
        for (b = 0; b < z; b += 1)
          (w = (Ge = I[b].nodes) == null ? void 0 : Ge.a) == null || w.measure();
        for (b = 0; b < z; b += 1)
          (Yr = (ae = I[b].nodes) == null ? void 0 : ae.a) == null || Yr.fix();
      }
      Xl(e, I, P);
    }
  }
  s && rt(() => {
    var Ke, zr;
    if (p !== void 0)
      for (c of p)
        (zr = (Ke = c.nodes) == null ? void 0 : Ke.a) == null || zr.apply();
  });
}
function Ul(e, t, n, r, i, s, l, a) {
  var o = (l & Ss) !== 0 ? (l & As) === 0 ? /* @__PURE__ */ Wi(n, !1, !1) : At(n) : null, u = (l & Cs) !== 0 ? At(i) : null;
  return {
    v: o,
    i: u,
    e: we(() => (s(t, o ?? n, u ?? i, a), () => {
      e.delete(r);
    }))
  };
}
function sn(e, t, n) {
  if (e.nodes)
    for (var r = e.nodes.start, i = e.nodes.end, s = t && (t.f & We) === 0 ? (
      /** @type {EffectNodes} */
      t.nodes.start
    ) : n; r !== null; ) {
      var l = (
        /** @type {TemplateNode} */
        /* @__PURE__ */ lt(r)
      );
      if (s.before(r), r === i)
        return;
      r = l;
    }
}
function at(e, t, n) {
  t === null ? e.effect.first = n : t.next = n, n === null ? e.effect.last = t : n.prev = t;
}
function vs(e, t, n, r, i) {
  var a;
  D && bn();
  var s = (a = t.$$slots) == null ? void 0 : a[n], l = !1;
  s === !0 && (s = t.children, l = !0), s === void 0 || s(e, l ? () => r : r);
}
function Le(e, t) {
  Ki(() => {
    var n = e.getRootNode(), r = (
      /** @type {ShadowRoot} */
      n.host ? (
        /** @type {ShadowRoot} */
        n
      ) : (
        /** @type {Document} */
        n.head ?? /** @type {Document} */
        n.ownerDocument.head
      )
    );
    if (!r.querySelector("#" + t.hash)) {
      const i = Sr("style");
      i.id = t.hash, i.textContent = t.code, r.appendChild(i);
    }
  });
}
const ni = [...` 	
\r\f \v\uFEFF`];
function Gl(e, t, n) {
  var r = e == null ? "" : "" + e;
  if (n) {
    for (var i of Object.keys(n))
      if (n[i])
        r = r ? r + " " + i : i;
      else if (r.length)
        for (var s = i.length, l = 0; (l = r.indexOf(i, l)) >= 0; ) {
          var a = l + s;
          (l === 0 || ni.includes(r[l - 1])) && (a === r.length || ni.includes(r[a])) ? r = (l === 0 ? "" : r.substring(0, l)) + r.substring(a + 1) : l = a;
        }
  }
  return r === "" ? null : r;
}
function ps(e, t, n, r, i, s) {
  var l = (
    /** @type {any} */
    e[nr]
  );
  if (D || l !== n || l === void 0) {
    var a = Gl(n, r, s);
    (!D || a !== e.getAttribute("class")) && (a == null ? e.removeAttribute("class") : e.className = a), e[nr] = n;
  } else if (s && i !== s)
    for (var o in s) {
      var u = !!s[o];
      (i == null || u !== !!i[o]) && e.classList.toggle(o, u);
    }
  return s;
}
function gs(e, t, n = !1) {
  if (e.multiple) {
    if (t == null)
      return;
    if (!yr(t))
      return ll();
    for (var r of e.options)
      r.selected = t.includes(ri(r));
    return;
  }
  for (r of e.options) {
    var i = ri(r);
    if (Sl(i, t)) {
      r.selected = !0;
      return;
    }
  }
  (!n || t !== void 0) && (e.selectedIndex = -1);
}
function Kl(e) {
  var t = new MutationObserver(() => {
    gs(e, e.__value);
  });
  t.observe(e, {
    // Listen to option element changes
    childList: !0,
    subtree: !0,
    // because of <optgroup>
    // Listen to option element value attribute changes
    // (doesn't get notified of select value changes,
    // because that property is not reflected as an attribute)
    attributes: !0,
    attributeFilter: ["value"]
  }), Ar(() => {
    t.disconnect();
  });
}
function ri(e) {
  return "__value" in e ? e.__value : e.value;
}
const Jl = Symbol("is custom element"), Zl = Symbol("is html"), Ql = bi ? "link" : "LINK", ea = bi ? "progress" : "PROGRESS";
function xn(e) {
  if (D) {
    var t = !1, n = () => {
      if (!t) {
        if (t = !0, e.hasAttribute("value")) {
          var r = e.value;
          Be(e, "value", null), e.value = r;
        }
        if (e.hasAttribute("checked")) {
          var i = e.checked;
          Be(e, "checked", null), e.checked = i;
        }
      }
    };
    e[_i] = n, rt(n), Cl();
  }
}
function Vn(e, t) {
  var n = Pr(e);
  n.value === (n.value = // treat null and undefined the same for the initial value
  t ?? void 0) || // @ts-expect-error
  // `progress` elements always need their value set when it's `0`
  e.value === t && (t !== 0 || e.nodeName !== ea) || (e.value = t ?? "");
}
function ms(e, t) {
  var n = Pr(e);
  n.checked !== (n.checked = // treat null and undefined the same for the initial value
  t ?? void 0) && (e.checked = t);
}
function ta(e, t) {
  t ? e.hasAttribute("selected") || e.setAttribute("selected", "") : e.removeAttribute("selected");
}
function Be(e, t, n, r) {
  var i = Pr(e);
  D && (i[t] = e.getAttribute(t), t === "src" || t === "srcset" || t === "href" && e.nodeName === Ql) || i[t] !== (i[t] = n) && (t === "loading" && (e[qs] = n), n == null ? e.removeAttribute(t) : typeof n != "string" && na(e).includes(t) ? e[t] = n : e.setAttribute(t, n));
}
function Pr(e) {
  return (
    /** @type {Record<string | symbol, unknown>} **/
    /** @type {any} */
    e[kn] ?? (e[kn] = {
      [Jl]: e.nodeName.includes("-"),
      [Zl]: e.namespaceURI === Ls
    })
  );
}
var ii = /* @__PURE__ */ new Map();
function na(e) {
  var t = e.getAttribute("is") || e.nodeName, n = ii.get(t);
  if (n) return n;
  ii.set(t, n = []);
  for (var r, i = e, s = Element.prototype; s !== i; ) {
    r = js(i);
    for (var l in r)
      r[l].set && // better safe than sorry, we don't want spread attributes to mess with HTML content
      l !== "innerHTML" && l !== "textContent" && l !== "innerText" && n.push(l);
    i = vi(i);
  }
  return n;
}
function Qn(e, t) {
  return e === t || (e == null ? void 0 : e[kt]) === t;
}
function jr(e = {}, t, n, r) {
  var i = (
    /** @type {ComponentContext} */
    se.r
  ), s = (
    /** @type {Effect} */
    C
  );
  return Ki(() => {
    var l, a;
    return Nr(() => {
      l = a, a = [], Lr(() => {
        Qn(n(...a), e) || (t(e, ...a), l && Qn(n(...l), e) && t(null, ...l));
      });
    }), () => {
      let o = s;
      for (; o !== i && o.parent !== null && o.parent.f & tr; )
        o = o.parent;
      const u = () => {
        a && Qn(n(...a), e) && t(null, ...a);
      }, d = o.teardown;
      o.teardown = () => {
        u(), d == null || d();
      };
    };
  }), e;
}
function N(e, t, n, r) {
  var E;
  var i = !0, s = (n & Rs) !== 0, l = (n & Is) !== 0, a = (
    /** @type {V} */
    r
  ), o = !0, u = (
    /** @type {Derived<V> | undefined} */
    void 0
  ), d = () => l && i ? (u ?? (u = /* @__PURE__ */ un(
    /** @type {() => V} */
    r
  )), O(u)) : (o && (o = !1, a = l ? Lr(
    /** @type {() => V} */
    r
  ) : (
    /** @type {V} */
    r
  )), a);
  let p;
  if (s) {
    var h = kt in e || mi in e;
    p = ((E = $t(e, t)) == null ? void 0 : E.set) ?? (h && t in e ? (y) => e[t] = y : void 0);
  }
  var v, m = !1;
  s ? [v, m] = hl(() => (
    /** @type {V} */
    e[t]
  )) : v = /** @type {V} */
  e[t], v === void 0 && r !== void 0 && (v = d(), p && (el(), p(v)));
  var g;
  if (g = () => {
    var y = (
      /** @type {V} */
      e[t]
    );
    return y === void 0 ? d() : (o = !0, y);
  }, (n & Ns) === 0)
    return g;
  if (p) {
    var c = e.$$legacy;
    return (
      /** @type {() => V} */
      (function(y, $) {
        return arguments.length > 0 ? ((!$ || c || m) && p($ ? g() : y), y) : g();
      })
    );
  }
  var b = !1, x = ((n & Os) !== 0 ? un : Ni)(() => (b = !1, g()));
  s && O(x);
  var _ = (
    /** @type {Effect} */
    C
  );
  return (
    /** @type {() => V} */
    (function(y, $) {
      if (arguments.length > 0) {
        const I = $ ? O(x) : s ? gt(y) : y;
        return Ne(x, I), b = !0, a !== void 0 && (a = I), y;
      }
      return st && b || (_.f & pe) !== 0 ? x.v : O(x);
    })
  );
}
function ra(e) {
  return new ia(e);
}
var tt, xe;
class ia {
  /**
   * @param {ComponentConstructorOptions & {
   *  component: any;
   * }} options
   */
  constructor(t) {
    /** @type {any} */
    S(this, tt);
    /** @type {Record<string, any>} */
    S(this, xe);
    var s;
    var n = /* @__PURE__ */ new Map(), r = (l, a) => {
      var o = /* @__PURE__ */ Wi(a, !1, !1);
      return n.set(l, o), o;
    };
    const i = new Proxy(
      { ...t.props || {}, $$events: {} },
      {
        get(l, a) {
          return O(n.get(a) ?? r(a, Reflect.get(l, a)));
        },
        has(l, a) {
          return a === mi ? !0 : (O(n.get(a) ?? r(a, Reflect.get(l, a))), Reflect.has(l, a));
        },
        set(l, a, o) {
          return Ne(n.get(a) ?? r(a, o), o), Reflect.set(l, a, o);
        }
      }
    );
    k(this, xe, (t.hydrate ? zl : fs)(t.component, {
      target: t.target,
      anchor: t.anchor,
      props: i,
      context: t.context,
      intro: t.intro ?? !1,
      recover: t.recover,
      transformError: t.transformError
    })), (!((s = t == null ? void 0 : t.props) != null && s.$$host) || t.sync === !1) && A(), k(this, tt, i.$$events);
    for (const l of Object.keys(f(this, xe)))
      l === "$set" || l === "$destroy" || l === "$on" || Rn(this, l, {
        get() {
          return f(this, xe)[l];
        },
        /** @param {any} value */
        set(a) {
          f(this, xe)[l] = a;
        },
        enumerable: !0
      });
    f(this, xe).$set = /** @param {Record<string, any>} next */
    (l) => {
      Object.assign(i, l);
    }, f(this, xe).$destroy = () => {
      Bl(f(this, xe));
    };
  }
  /** @param {Record<string, any>} props */
  $set(t) {
    f(this, xe).$set(t);
  }
  /**
   * @param {string} event
   * @param {(...args: any[]) => any} callback
   * @returns {any}
   */
  $on(t, n) {
    f(this, tt)[t] = f(this, tt)[t] || [];
    const r = (...i) => n.call(this, ...i);
    return f(this, tt)[t].push(r), () => {
      f(this, tt)[t] = f(this, tt)[t].filter(
        /** @param {any} fn */
        (i) => i !== r
      );
    };
  }
  $destroy() {
    f(this, xe).$destroy();
  }
}
tt = new WeakMap(), xe = new WeakMap();
let _s;
typeof HTMLElement == "function" && (_s = class extends HTMLElement {
  /**
   * @param {*} $$componentCtor
   * @param {*} $$slots
   * @param {ShadowRootInit | undefined} shadow_root_init
   */
  constructor(t, n, r) {
    super();
    /** The Svelte component constructor */
    W(this, "$$ctor");
    /** Slots */
    W(this, "$$s");
    /** @type {any} The Svelte component instance */
    W(this, "$$c");
    /** Whether or not the custom element is connected */
    W(this, "$$cn", !1);
    /** @type {Record<string, any>} Component props data */
    W(this, "$$d", {});
    /** `true` if currently in the process of reflecting component props back to attributes */
    W(this, "$$r", !1);
    /** @type {Record<string, CustomElementPropDefinition>} Props definition (name, reflected, type etc) */
    W(this, "$$p_d", {});
    /** @type {Record<string, EventListenerOrEventListenerObject[]>} Event listeners */
    W(this, "$$l", {});
    /** @type {Map<EventListenerOrEventListenerObject, Function>} Event listener unsubscribe functions */
    W(this, "$$l_u", /* @__PURE__ */ new Map());
    /** @type {any} The managed render effect for reflecting attributes */
    W(this, "$$me");
    /** @type {ShadowRoot | null} The ShadowRoot of the custom element */
    W(this, "$$shadowRoot", null);
    this.$$ctor = t, this.$$s = n, r && (this.$$shadowRoot = this.attachShadow(r));
  }
  /**
   * @param {string} type
   * @param {EventListenerOrEventListenerObject} listener
   * @param {boolean | AddEventListenerOptions} [options]
   */
  addEventListener(t, n, r) {
    if (this.$$l[t] = this.$$l[t] || [], this.$$l[t].push(n), this.$$c) {
      const i = this.$$c.$on(t, n);
      this.$$l_u.set(n, i);
    }
    super.addEventListener(t, n, r);
  }
  /**
   * @param {string} type
   * @param {EventListenerOrEventListenerObject} listener
   * @param {boolean | AddEventListenerOptions} [options]
   */
  removeEventListener(t, n, r) {
    if (super.removeEventListener(t, n, r), this.$$c) {
      const i = this.$$l_u.get(n);
      i && (i(), this.$$l_u.delete(n));
    }
  }
  async connectedCallback() {
    if (this.$$cn = !0, !this.$$c) {
      let t = function(i) {
        return (s) => {
          const l = Sr("slot");
          i !== "default" && (l.name = i), V(s, l);
        };
      };
      if (await Promise.resolve(), !this.$$cn || this.$$c)
        return;
      const n = {}, r = sa(this);
      for (const i of this.$$s)
        i in r && (i === "default" && !this.$$d.children ? (this.$$d.children = t(i), n.default = !0) : n[i] = t(i));
      for (const i of this.attributes) {
        const s = this.$$g_p(i.name);
        s in this.$$d || (this.$$d[s] = An(s, i.value, this.$$p_d, "toProp"));
      }
      for (const i in this.$$p_d)
        !(i in this.$$d) && this[i] !== void 0 && (this.$$d[i] = this[i], delete this[i]);
      this.$$c = ra({
        component: this.$$ctor,
        target: this.$$shadowRoot || this,
        props: {
          ...this.$$d,
          $$slots: n,
          $$host: this
        }
      }), this.$$me = Ol(() => {
        Nr(() => {
          var i;
          this.$$r = !0;
          for (const s of Nn(this.$$c)) {
            if (!((i = this.$$p_d[s]) != null && i.reflect)) continue;
            this.$$d[s] = this.$$c[s];
            const l = An(
              s,
              this.$$d[s],
              this.$$p_d,
              "toAttribute"
            );
            l == null ? this.removeAttribute(this.$$p_d[s].attribute || s) : this.setAttribute(this.$$p_d[s].attribute || s, l);
          }
          this.$$r = !1;
        });
      });
      for (const i in this.$$l)
        for (const s of this.$$l[i]) {
          const l = this.$$c.$on(i, s);
          this.$$l_u.set(s, l);
        }
      this.$$l = {};
    }
  }
  // We don't need this when working within Svelte code, but for compatibility of people using this outside of Svelte
  // and setting attributes through setAttribute etc, this is helpful
  /**
   * @param {string} attr
   * @param {string} _oldValue
   * @param {string} newValue
   */
  attributeChangedCallback(t, n, r) {
    var i;
    this.$$r || (t = this.$$g_p(t), this.$$d[t] = An(t, r, this.$$p_d, "toProp"), (i = this.$$c) == null || i.$set({ [t]: this.$$d[t] }));
  }
  disconnectedCallback() {
    this.$$cn = !1, Promise.resolve().then(() => {
      !this.$$cn && this.$$c && (this.$$c.$destroy(), this.$$me(), this.$$c = void 0);
    });
  }
  /**
   * @param {string} attribute_name
   */
  $$g_p(t) {
    return Nn(this.$$p_d).find(
      (n) => this.$$p_d[n].attribute === t || !this.$$p_d[n].attribute && n.toLowerCase() === t
    ) || t;
  }
});
function An(e, t, n, r) {
  var s;
  const i = (s = n[e]) == null ? void 0 : s.type;
  if (t = i === "Boolean" && typeof t != "boolean" ? t != null : t, !r || !n[e])
    return t;
  if (r === "toAttribute")
    switch (i) {
      case "Object":
      case "Array":
        return t == null ? null : JSON.stringify(t);
      case "Boolean":
        return t ? "" : null;
      case "Number":
        return t ?? null;
      default:
        return t;
    }
  else
    switch (i) {
      case "Object":
      case "Array":
        return t && JSON.parse(t);
      case "Boolean":
        return t;
      // conversion already handled above
      case "Number":
        return t != null ? +t : t;
      default:
        return t;
    }
}
function sa(e) {
  const t = {};
  return e.childNodes.forEach((n) => {
    t[
      /** @type {Element} node */
      n.slot || "default"
    ] = !0;
  }), t;
}
function Pe(e, t, n, r, i, s) {
  let l = class extends _s {
    constructor() {
      super(e, n, i), this.$$p_d = t;
    }
    static get observedAttributes() {
      return Nn(t).map(
        (a) => (t[a].attribute || a).toLowerCase()
      );
    }
  };
  return Nn(t).forEach((a) => {
    Rn(l.prototype, a, {
      get() {
        return this.$$c && a in this.$$c ? this.$$c[a] : this.$$d[a];
      },
      set(o) {
        var p;
        o = An(a, o, t), this.$$d[a] = o;
        var u = this.$$c;
        if (u) {
          var d = (p = $t(u, a)) == null ? void 0 : p.get;
          d ? u[a] = o : u.$set({ [a]: o });
        }
      }
    });
  }), r.forEach((a) => {
    Rn(l.prototype, a, {
      get() {
        var o;
        return (o = this.$$c) == null ? void 0 : o[a];
      }
    });
  }), e.element = /** @type {any} */
  l, l;
}
var la = /* @__PURE__ */ K('<span class="lbl"> </span>'), aa = /* @__PURE__ */ K('<label class="xi-slider svelte-8xo3l7"><!> <input type="range" class="svelte-8xo3l7"/> <span class="val svelte-8xo3l7"> </span></label>');
const oa = {
  hash: "svelte-8xo3l7",
  code: '.xi-slider.svelte-8xo3l7 {display:inline-flex;align-items:center;gap:0.5rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}input[type="range"].svelte-8xo3l7 {accent-color:var(--xi-accent, #3b82f6);vertical-align:middle;}.val.svelte-8xo3l7 {min-width:2.5em;text-align:right;font-variant-numeric:tabular-nums;}'
};
function ua(e, t) {
  Se(t, !0), Le(e, oa);
  let n = N(t, "value", 15, 0), r = N(t, "min", 7, 0), i = N(t, "max", 7, 100), s = N(t, "step", 7, 1), l = N(t, "label", 7, ""), a = N(t, "disabled", 7, !1);
  const o = t.$$host, u = (_) => o.dispatchEvent(new CustomEvent(_, { detail: { value: n() }, bubbles: !0, composed: !0 }));
  function d(_) {
    n(Number(_.target.value)), u("input");
  }
  function p(_) {
    n(Number(_.target.value)), u("change");
  }
  var h = {
    get value() {
      return n();
    },
    set value(_ = 0) {
      n(_), A();
    },
    get min() {
      return r();
    },
    set min(_ = 0) {
      r(_), A();
    },
    get max() {
      return i();
    },
    set max(_ = 100) {
      i(_), A();
    },
    get step() {
      return s();
    },
    set step(_ = 1) {
      s(_), A();
    },
    get label() {
      return l();
    },
    set label(_ = "") {
      l(_), A();
    },
    get disabled() {
      return a();
    },
    set disabled(_ = !1) {
      a(_), A();
    }
  }, v = aa(), m = B(v);
  {
    var g = (_) => {
      var E = la(), y = B(E, !0);
      Y(E), te(() => ke(y, l())), V(_, E);
    };
    Zt(m, (_) => {
      l() && _(g);
    });
  }
  var c = me(m, 2);
  xn(c);
  var b = me(c, 2), x = B(b, !0);
  return Y(b), Y(v), te(() => {
    Be(c, "min", r()), Be(c, "max", i()), Be(c, "step", s()), Vn(c, n()), c.disabled = a(), ke(x, n());
  }), J("input", c, d), J("change", c, p), V(e, v), Ce(h);
}
ht(["input", "change"]);
customElements.define("xi-slider", Pe(
  ua,
  {
    value: { reflect: !0, type: "Number" },
    min: { type: "Number" },
    max: { type: "Number" },
    step: { type: "Number" },
    label: {},
    disabled: {}
  },
  [],
  [],
  { mode: "open" }
));
var fa = /* @__PURE__ */ K('<span class="lbl"> </span>'), ca = /* @__PURE__ */ K('<label class="xi-number svelte-1f6ykwb"><!> <input type="number" class="svelte-1f6ykwb"/></label>');
const da = {
  hash: "svelte-1f6ykwb",
  code: ".xi-number.svelte-1f6ykwb {display:inline-flex;align-items:center;gap:0.5rem;font:var(--xi-font, 13px system-ui, sans-serif);color:var(--xi-fg, inherit);}input.svelte-1f6ykwb {width:6em;padding:0.15rem 0.3rem;color:var(--xi-fg, inherit);background:var(--xi-bg, #fff);border:1px solid var(--xi-border, #ccc);border-radius:var(--xi-radius, 3px);accent-color:var(--xi-accent, #3b82f6);}input.svelte-1f6ykwb:focus {outline:1px solid var(--xi-accent, #3b82f6);outline-offset:-1px;}"
};
function ha(e, t) {
  Se(t, !0), Le(e, da);
  let n = N(t, "value", 15, 0), r = N(t, "min", 7), i = N(t, "max", 7), s = N(t, "step", 7, 1), l = N(t, "label", 7, ""), a = N(t, "disabled", 7, !1);
  const o = t.$$host, u = (x) => o.dispatchEvent(new CustomEvent(x, { detail: { value: n() }, bubbles: !0, composed: !0 })), d = (x) => x.target.value === "" ? null : Number(x.target.value);
  function p(x) {
    n(d(x)), u("input");
  }
  function h(x) {
    n(d(x)), u("change");
  }
  var v = {
    get value() {
      return n();
    },
    set value(x = 0) {
      n(x), A();
    },
    get min() {
      return r();
    },
    set min(x) {
      r(x), A();
    },
    get max() {
      return i();
    },
    set max(x) {
      i(x), A();
    },
    get step() {
      return s();
    },
    set step(x = 1) {
      s(x), A();
    },
    get label() {
      return l();
    },
    set label(x = "") {
      l(x), A();
    },
    get disabled() {
      return a();
    },
    set disabled(x = !1) {
      a(x), A();
    }
  }, m = ca(), g = B(m);
  {
    var c = (x) => {
      var _ = fa(), E = B(_, !0);
      Y(_), te(() => ke(E, l())), V(x, _);
    };
    Zt(g, (x) => {
      l() && x(c);
    });
  }
  var b = me(g, 2);
  return xn(b), Y(m), te(() => {
    Be(b, "min", r()), Be(b, "max", i()), Be(b, "step", s()), Vn(b, n()), b.disabled = a();
  }), J("input", b, p), J("change", b, h), V(e, m), Ce(v);
}
ht(["input", "change"]);
customElements.define("xi-number", Pe(
  ha,
  {
    value: { reflect: !0, type: "Number" },
    min: { type: "Number" },
    max: { type: "Number" },
    step: { type: "Number" },
    label: {},
    disabled: {}
  },
  [],
  [],
  { mode: "open" }
));
var va = /* @__PURE__ */ K('<span class="lbl"> </span>'), pa = /* @__PURE__ */ K('<label class="xi-toggle svelte-141rfru"><input type="checkbox" class="svelte-141rfru"/> <!></label>');
const ga = {
  hash: "svelte-141rfru",
  code: ".xi-toggle.svelte-141rfru {display:inline-flex;align-items:center;gap:0.4rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);cursor:pointer;}input.svelte-141rfru {accent-color:var(--xi-accent, #3b82f6);}"
};
function ma(e, t) {
  Se(t, !0), Le(e, ga);
  let n = N(t, "value", 15, !1), r = N(t, "label", 7, ""), i = N(t, "disabled", 7, !1);
  const s = t.$$host;
  function l(h) {
    n(h.target.checked), s.dispatchEvent(new CustomEvent("change", { detail: { value: n() }, bubbles: !0, composed: !0 }));
  }
  var a = {
    get value() {
      return n();
    },
    set value(h = !1) {
      n(h), A();
    },
    get label() {
      return r();
    },
    set label(h = "") {
      r(h), A();
    },
    get disabled() {
      return i();
    },
    set disabled(h = !1) {
      i(h), A();
    }
  }, o = pa(), u = B(o);
  xn(u);
  var d = me(u, 2);
  {
    var p = (h) => {
      var v = va(), m = B(v, !0);
      Y(v), te(() => ke(m, r())), V(h, v);
    };
    Zt(d, (h) => {
      r() && h(p);
    });
  }
  return Y(o), te(() => {
    ms(u, n()), u.disabled = i();
  }), J("change", u, l), V(e, o), Ce(a);
}
ht(["change"]);
customElements.define("xi-toggle", Pe(
  ma,
  {
    value: { reflect: !0, type: "Boolean" },
    label: {},
    disabled: {}
  },
  [],
  [],
  { mode: "open" }
));
function bs(e) {
  let t = e;
  if (typeof e == "string")
    try {
      t = JSON.parse(e);
    } catch {
      t = [];
    }
  return Array.isArray(t) ? t.map((n) => n && typeof n == "object" ? { value: n.value, label: n.label ?? String(n.value) } : { value: n, label: String(n) }) : [];
}
var _a = /* @__PURE__ */ K('<span class="lbl"> </span>'), ba = /* @__PURE__ */ K('<label class="opt svelte-mtcxej"><input type="radio" class="svelte-mtcxej"/> <span> </span></label>'), ya = /* @__PURE__ */ K('<div class="xi-radio svelte-mtcxej" role="radiogroup"><!> <!></div>');
const xa = {
  hash: "svelte-mtcxej",
  code: ".xi-radio.svelte-mtcxej {display:inline-flex;align-items:center;gap:0.75rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}.opt.svelte-mtcxej {display:inline-flex;align-items:center;gap:0.25rem;cursor:pointer;}input.svelte-mtcxej {accent-color:var(--xi-accent, #3b82f6);}"
};
function wa(e, t) {
  Se(t, !0), Le(e, xa);
  let n = N(t, "value", 15, ""), r = N(t, "options", 23, () => []), i = N(t, "label", 7, ""), s = N(t, "disabled", 7, !1), l = N(t, "name", 7, "xi-radio");
  const a = t.$$host, o = /* @__PURE__ */ Er(() => bs(r()));
  function u(g) {
    n(g), a.dispatchEvent(new CustomEvent("change", { detail: { value: n() }, bubbles: !0, composed: !0 }));
  }
  var d = {
    get value() {
      return n();
    },
    set value(g = "") {
      n(g), A();
    },
    get options() {
      return r();
    },
    set options(g = []) {
      r(g), A();
    },
    get label() {
      return i();
    },
    set label(g = "") {
      i(g), A();
    },
    get disabled() {
      return s();
    },
    set disabled(g = !1) {
      s(g), A();
    },
    get name() {
      return l();
    },
    set name(g = "xi-radio") {
      l(g), A();
    }
  }, p = ya(), h = B(p);
  {
    var v = (g) => {
      var c = _a(), b = B(c, !0);
      Y(c), te(() => ke(b, i())), V(g, c);
    };
    Zt(h, (g) => {
      i() && g(v);
    });
  }
  var m = me(h, 2);
  return hs(m, 17, () => O(o), ds, (g, c) => {
    var b = ba(), x = B(b);
    xn(x);
    var _ = me(x, 2), E = B(_, !0);
    Y(_), Y(b), te(() => {
      Be(x, "name", l()), Vn(x, O(c).value), ms(x, O(c).value === n()), x.disabled = s(), ke(E, O(c).label);
    }), J("change", x, () => u(O(c).value)), V(g, b);
  }), Y(p), V(e, p), Ce(d);
}
ht(["change"]);
customElements.define("xi-radio", Pe(
  wa,
  {
    value: { reflect: !0 },
    options: {},
    label: {},
    disabled: {},
    name: {}
  },
  [],
  [],
  { mode: "open" }
));
var Ea = /* @__PURE__ */ K('<span class="lbl"> </span>'), $a = /* @__PURE__ */ K("<option> </option>"), ka = /* @__PURE__ */ K('<label class="xi-dropdown svelte-1wd9iqr"><!> <select class="svelte-1wd9iqr"></select></label>');
const Ta = {
  hash: "svelte-1wd9iqr",
  code: ".xi-dropdown.svelte-1wd9iqr {display:inline-flex;align-items:center;gap:0.5rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}select.svelte-1wd9iqr {padding:0.15rem 0.3rem;}"
};
function Sa(e, t) {
  Se(t, !0), Le(e, Ta);
  let n = N(t, "value", 15, ""), r = N(t, "options", 23, () => []), i = N(t, "label", 7, ""), s = N(t, "disabled", 7, !1);
  const l = t.$$host, a = /* @__PURE__ */ Er(() => bs(r()));
  function o(g) {
    n(g.target.value), l.dispatchEvent(new CustomEvent("change", { detail: { value: n() }, bubbles: !0, composed: !0 }));
  }
  var u = {
    get value() {
      return n();
    },
    set value(g = "") {
      n(g), A();
    },
    get options() {
      return r();
    },
    set options(g = []) {
      r(g), A();
    },
    get label() {
      return i();
    },
    set label(g = "") {
      i(g), A();
    },
    get disabled() {
      return s();
    },
    set disabled(g = !1) {
      s(g), A();
    }
  }, d = ka(), p = B(d);
  {
    var h = (g) => {
      var c = Ea(), b = B(c, !0);
      Y(c), te(() => ke(b, i())), V(g, c);
    };
    Zt(p, (g) => {
      i() && g(h);
    });
  }
  var v = me(p, 2);
  hs(v, 21, () => O(a), ds, (g, c) => {
    var b = $a(), x = B(b, !0);
    Y(b);
    var _ = {};
    te(() => {
      ta(b, O(c).value === n()), ke(x, O(c).label), _ !== (_ = O(c).value) && (b.value = (b.__value = O(c).value) ?? "");
    }), V(g, b);
  }), Y(v);
  var m;
  return Kl(v), Y(d), te(() => {
    v.disabled = s(), m !== (m = n()) && (v.value = (v.__value = n()) ?? "", gs(v, n()));
  }), J("change", v, o), V(e, d), Ce(u);
}
ht(["change"]);
customElements.define("xi-dropdown", Pe(
  Sa,
  {
    value: { reflect: !0 },
    options: {},
    label: {},
    disabled: {}
  },
  [],
  [],
  { mode: "open" }
));
var Ca = /* @__PURE__ */ K('<input class="xi-text svelte-krpro1" type="text"/>');
const Ma = {
  hash: "svelte-krpro1",
  code: ".xi-text.svelte-krpro1 {box-sizing:border-box;width:100%;font:var(--xi-font, 13px system-ui, sans-serif);padding:0.3em 0.5em;color:var(--xi-fg, inherit);background:var(--xi-bg, #fff);border:1px solid var(--xi-border, #ccc);border-radius:var(--xi-radius, 3px);}.xi-text.svelte-krpro1:focus {outline:1px solid var(--xi-accent, #3b82f6);outline-offset:-1px;}"
};
function Aa(e, t) {
  Se(t, !0), Le(e, Ma);
  let n = N(t, "value", 15, ""), r = N(t, "placeholder", 7, ""), i = N(t, "disabled", 7, !1);
  const s = t.$$host, l = (p) => s.dispatchEvent(new CustomEvent(p, { detail: { value: n() }, bubbles: !0, composed: !0 }));
  function a(p) {
    n(p.target.value), l("input");
  }
  function o(p) {
    n(p.target.value), l("change");
  }
  var u = {
    get value() {
      return n();
    },
    set value(p = "") {
      n(p), A();
    },
    get placeholder() {
      return r();
    },
    set placeholder(p = "") {
      r(p), A();
    },
    get disabled() {
      return i();
    },
    set disabled(p = !1) {
      i(p), A();
    }
  }, d = Ca();
  return xn(d), te(() => {
    Vn(d, n()), Be(d, "placeholder", r()), d.disabled = i();
  }), J("input", d, a), J("change", d, o), V(e, d), Ce(u);
}
ht(["input", "change"]);
customElements.define("xi-text", Pe(Aa, { value: { reflect: !0 }, placeholder: {}, disabled: {} }, [], [], { mode: "open" }));
var Oa = /* @__PURE__ */ K('<span class="ico svelte-1v6o256" aria-hidden="true"> </span>'), Na = /* @__PURE__ */ K("<button><!> <!></button>");
const Ra = {
  hash: "svelte-1v6o256",
  code: ".xi-button.svelte-1v6o256 {display:inline-flex;align-items:center;gap:0.4em;font:var(--xi-font, 13px system-ui, sans-serif);padding:0.35em 0.9em;border:1px solid transparent;border-radius:var(--xi-radius, 3px);background:var(--xi-btn-bg, #3b82f6);color:var(--xi-btn-fg, #fff);cursor:pointer;}.xi-button.svelte-1v6o256:hover {background:var(--xi-btn-hover-bg, #2f6fe0);}.xi-button.svelte-1v6o256:focus-visible {outline:1px solid var(--xi-accent, #3b82f6);outline-offset:2px;}.xi-button.secondary.svelte-1v6o256 {background:var(--xi-btn-secondary-bg, #444);color:var(--xi-btn-secondary-fg, #fff);}.xi-button.secondary.svelte-1v6o256:hover {background:var(--xi-btn-secondary-hover-bg, #4f4f4f);}.xi-button.svelte-1v6o256:disabled {opacity:0.5;cursor:default;}.ico.svelte-1v6o256 {font-size:0.9em;line-height:1;}"
};
function Ia(e, t) {
  Se(t, !0), Le(e, Ra);
  let n = N(t, "secondary", 7, !1), r = N(t, "disabled", 7, !1), i = N(t, "icon", 7, "");
  const s = { add: "＋", play: "▶", "debug-stop": "■", stop: "■" }, l = /* @__PURE__ */ Er(() => i() ? s[i()] ?? "" : "");
  var a = {
    get secondary() {
      return n();
    },
    set secondary(v = !1) {
      n(v), A();
    },
    get disabled() {
      return r();
    },
    set disabled(v = !1) {
      r(v), A();
    },
    get icon() {
      return i();
    },
    set icon(v = "") {
      i(v), A();
    }
  }, o = Na();
  let u;
  var d = B(o);
  {
    var p = (v) => {
      var m = Oa(), g = B(m, !0);
      Y(m), te(() => ke(g, O(l))), V(v, m);
    };
    Zt(d, (v) => {
      O(l) && v(p);
    });
  }
  var h = me(d, 2);
  return vs(h, t, "default", {}), Y(o), te(() => {
    u = ps(o, 1, "xi-button svelte-1v6o256", null, u, { secondary: n() }), o.disabled = r();
  }), V(e, o), Ce(a);
}
customElements.define("xi-button", Pe(
  Ia,
  {
    secondary: { reflect: !0, type: "Boolean" },
    disabled: { reflect: !0, type: "Boolean" },
    icon: {}
  },
  ["default"],
  [],
  { mode: "open" }
));
var Da = /* @__PURE__ */ K("<span><!></span>");
const La = {
  hash: "svelte-e9efnj",
  code: ".xi-badge.svelte-e9efnj {display:inline-flex;align-items:center;font:var(--xi-font, 11px system-ui, sans-serif);font-size:0.85em;line-height:1;padding:0.2em 0.55em;border-radius:var(--xi-radius, 3px);background:var(--xi-badge-bg, #4d4d4d);color:var(--xi-badge-fg, #fff);white-space:nowrap;}.xi-badge.counter.svelte-e9efnj {border-radius:999px;padding:0.2em 0.6em;}"
};
function Pa(e, t) {
  Se(t, !0), Le(e, La);
  let n = N(t, "variant", 7, "");
  var r = {
    get variant() {
      return n();
    },
    set variant(a = "") {
      n(a), A();
    }
  }, i = Da();
  let s;
  var l = B(i);
  return vs(l, t, "default", {}), Y(i), te(() => s = ps(i, 1, "xi-badge svelte-e9efnj", null, s, { counter: n() === "counter" })), V(e, i), Ce(r);
}
customElements.define("xi-badge", Pe(Pa, { variant: { reflect: !0 } }, ["default"], [], { mode: "open" }));
var ja = /* @__PURE__ */ K('<div class="xi-trace svelte-1m1z2sd"><span class="lbl"> </span> <canvas class="svelte-1m1z2sd"></canvas> <span class="val svelte-1m1z2sd"> </span></div>');
const Ha = {
  hash: "svelte-1m1z2sd",
  code: ".xi-trace.svelte-1m1z2sd {display:inline-flex;align-items:center;gap:0.5rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}canvas.svelte-1m1z2sd {width:120px;height:28px;display:block;}.val.svelte-1m1z2sd {min-width:3em;text-align:right;font-variant-numeric:tabular-nums;}"
};
function Fa(e, t) {
  Se(t, !0), Le(e, Ha);
  let n = N(t, "key", 7, ""), r = N(t, "label", 7, ""), i = N(t, "max", 7, 60);
  const s = t.$$host;
  let l, a = /* @__PURE__ */ je(null), o = /* @__PURE__ */ je(gt([]));
  function u() {
    if (!l) return;
    const _ = l.getContext && l.getContext("2d");
    if (!_) return;
    const E = l.width = l.clientWidth || 120, y = l.height = l.clientHeight || 28;
    if (_.clearRect(0, 0, E, y), O(o).length < 2) return;
    const $ = Math.min(...O(o)), I = Math.max(...O(o)), z = I - $ || 1;
    _.beginPath(), O(o).forEach((P, H) => {
      const q = H / (O(o).length - 1) * (E - 2) + 1, Q = y - 2 - (P - $) / z * (y - 4);
      H ? _.lineTo(q, Q) : _.moveTo(q, Q);
    }), _.strokeStyle = getComputedStyle(s).getPropertyValue("--xi-accent") || "#3b82f6", _.lineWidth = 1.5, _.stroke();
  }
  function d(_) {
    const E = _ && _[n()];
    E && (Ne(a, E.value, !0), typeof E.value == "number" && Number.isFinite(E.value) && (Ne(o, [...O(o), E.value].slice(-i()), !0), u()), s.dispatchEvent(new CustomEvent("sample", { detail: { value: E.value }, bubbles: !0, composed: !0 })));
  }
  Or(() => {
    s.update = d, Object.defineProperty(s, "latest", { get: () => O(a), configurable: !0 }), Object.defineProperty(s, "history", { get: () => O(o).slice(), configurable: !0 }), u();
  });
  const p = (_) => _ == null ? "—" : typeof _ == "number" ? Number.isInteger(_) ? _ : _.toFixed(3) : String(_);
  var h = {
    get key() {
      return n();
    },
    set key(_ = "") {
      n(_), A();
    },
    get label() {
      return r();
    },
    set label(_ = "") {
      r(_), A();
    },
    get max() {
      return i();
    },
    set max(_ = 60) {
      i(_), A();
    }
  }, v = ja(), m = B(v), g = B(m, !0);
  Y(m);
  var c = me(m, 2);
  jr(c, (_) => l = _, () => l);
  var b = me(c, 2), x = B(b, !0);
  return Y(b), Y(v), te(
    (_) => {
      ke(g, r() || n()), ke(x, _);
    },
    [() => p(O(a))]
  ), V(e, v), Ce(h);
}
customElements.define("xi-trace", Pe(Fa, { key: {}, label: {}, max: { type: "Number" } }, [], [], { mode: "open" }));
function ys() {
  return { scale: 1, panX: 0, panY: 0, imgW: 0, imgH: 0, viewW: 0, viewH: 0 };
}
function Hr(e, t, n) {
  return { x: (t - e.panX) / e.scale, y: (n - e.panY) / e.scale };
}
function Wa(e, t, n) {
  return { x: e.panX + t * e.scale, y: e.panY + n * e.scale };
}
const Ya = 0.05, za = 64, Ba = (e) => Math.max(Ya, Math.min(za, e));
function pr(e) {
  return !e.imgW || !e.imgH || !e.viewW || !e.viewH || (e.scale = Math.min(e.viewW / e.imgW, e.viewH / e.imgH) * 0.95, e.panX = (e.viewW - e.imgW * e.scale) / 2, e.panY = (e.viewH - e.imgH * e.scale) / 2), e;
}
function qa(e) {
  return e.scale = 1, e.panX = (e.viewW - e.imgW) / 2, e.panY = (e.viewH - e.imgH) / 2, e;
}
function xs(e, t, n, r) {
  const { x: i, y: s } = Hr(e, t, n);
  return e.scale = Ba(e.scale * r), e.panX = t - i * e.scale, e.panY = n - s * e.scale, e;
}
function Xa(e, t, n) {
  return e.panX += t, e.panY += n, e;
}
var Va = /* @__PURE__ */ K('<canvas class="svelte-1yjweo0"></canvas>');
const Ua = {
  hash: "svelte-1yjweo0",
  code: ":host {display:block;width:100%;height:100%;}canvas.svelte-1yjweo0 {width:100%;height:100%;display:block;background:var(--xi-viewer-bg, #111);cursor:grab;touch-action:none;image-rendering:pixelated;}canvas.svelte-1yjweo0:active {cursor:grabbing;}"
};
function Ga(e, t) {
  Se(t, !0), Le(e, Ua);
  const n = t.$$host;
  let r;
  const i = ys();
  let s = null, l = null;
  function a() {
    if (!r) return;
    const y = r.getContext("2d");
    y.imageSmoothingEnabled = !1, y.clearRect(0, 0, r.width, r.height), s && (y.setTransform(i.scale, 0, 0, i.scale, i.panX, i.panY), y.drawImage(s, 0, 0), y.setTransform(1, 0, 0, 1, 0, 0));
  }
  function o() {
    if (!r) return;
    const y = r.getBoundingClientRect();
    r.width = Math.max(1, Math.round(y.width)), r.height = Math.max(1, Math.round(y.height)), i.viewW = r.width, i.viewH = r.height, a();
  }
  function u(y, $) {
    n.dispatchEvent(new CustomEvent(y, { detail: $, bubbles: !0, composed: !0 }));
  }
  function d(y) {
    return !!y && typeof y != "string" && !("dataUrl" in y) && (typeof HTMLImageElement < "u" && y instanceof HTMLImageElement || typeof HTMLCanvasElement < "u" && y instanceof HTMLCanvasElement || typeof OffscreenCanvas < "u" && y instanceof OffscreenCanvas || typeof ImageBitmap < "u" && y instanceof ImageBitmap);
  }
  function p(y) {
    if (d(y)) {
      h(y);
      return;
    }
    const $ = new Image();
    $.onload = () => h($), $.src = typeof y == "string" ? y : y.dataUrl;
  }
  function h(y) {
    const $ = !i.imgW;
    s = y, i.imgW = y.naturalWidth || y.width, i.imgH = y.naturalHeight || y.height, l = document.createElement("canvas"), l.width = i.imgW, l.height = i.imgH, l.getContext("2d").drawImage(y, 0, 0), $ && pr(i), a();
  }
  function v(y) {
    if (!s) return;
    y.preventDefault();
    const $ = r.getBoundingClientRect();
    xs(i, y.clientX - $.left, y.clientY - $.top, y.deltaY < 0 ? 1.15 : 1 / 1.15), a(), u("viewchange", { scale: i.scale });
  }
  let m = null, g = !1;
  function c(y) {
    var $;
    s && (m = { x: y.clientX, y: y.clientY }, g = !1, ($ = r.setPointerCapture) == null || $.call(r, y.pointerId));
  }
  function b(y) {
    if (!m) return;
    const $ = y.clientX - m.x, I = y.clientY - m.y;
    ($ || I) && (g = !0), Xa(i, $, I), m = { x: y.clientX, y: y.clientY }, a();
  }
  function x(y) {
    m && !g && _(y), m = null;
  }
  function _(y) {
    if (!s || !l) return;
    const $ = r.getBoundingClientRect(), I = Hr(i, y.clientX - $.left, y.clientY - $.top), z = Math.floor(I.x), P = Math.floor(I.y);
    let H = null;
    if (z >= 0 && P >= 0 && z < i.imgW && P < i.imgH) {
      const q = l.getContext("2d").getImageData(z, P, 1, 1).data;
      H = [q[0], q[1], q[2]];
    }
    u("pixelpick", { x: z, y: P, rgb: H });
  }
  Or(() => {
    n.setFrame = p, n.fit = () => {
      pr(i), a(), u("viewchange", { scale: i.scale });
    }, n.oneToOne = () => {
      qa(i), a(), u("viewchange", { scale: i.scale });
    }, o();
    const y = new ResizeObserver(o);
    return y.observe(r), () => y.disconnect();
  });
  var E = Va();
  jr(E, (y) => r = y, () => r), us("wheel", E, v), J("pointerdown", E, c), J("pointermove", E, b), J("pointerup", E, x), V(e, E), Ce();
}
ht(["pointerdown", "pointermove", "pointerup"]);
customElements.define("xi-image-viewer", Pe(Ga, {}, [], [], { mode: "open" }));
function Ka() {
  let e = null;
  return {
    type: "point",
    onDown(t) {
      e = { x: Math.round(t.x), y: Math.round(t.y) };
    },
    onMove() {
    },
    onUp() {
    },
    onDbl() {
    },
    done() {
      return !!e;
    },
    result() {
      return e ? { ...e } : null;
    },
    draw(t, n) {
      if (!e) return;
      const r = n(e);
      t.fillStyle = "#f59e0b", t.beginPath(), t.arc(r.x, r.y, 4, 0, Math.PI * 2), t.fill();
    }
  };
}
function Ja() {
  let e = null, t = null, n = !1;
  const r = () => ({
    x: Math.round(Math.min(e.x, t.x)),
    y: Math.round(Math.min(e.y, t.y)),
    w: Math.round(Math.abs(e.x - t.x)),
    h: Math.round(Math.abs(e.y - t.y))
  });
  return {
    type: "rect",
    onDown(i) {
      e = { ...i }, t = { ...i }, n = !0;
    },
    onMove(i) {
      n && (t = { ...i });
    },
    onUp(i) {
      n && (t = { ...i }, n = !1);
    },
    onDbl() {
    },
    done() {
      return !!(e && t) && (r().w > 0 || r().h > 0);
    },
    result() {
      return e && t ? r() : null;
    },
    draw(i, s) {
      if (!e || !t) return;
      const l = s(e), a = s(t);
      i.strokeStyle = "#f59e0b", i.lineWidth = 1.5, i.strokeRect(Math.min(l.x, a.x), Math.min(l.y, a.y), Math.abs(a.x - l.x), Math.abs(a.y - l.y));
    }
  };
}
function Za() {
  let e = [], t = !1;
  return {
    type: "polygon",
    onDown(n) {
      t || e.push([Math.round(n.x), Math.round(n.y)]);
    },
    onMove() {
    },
    onUp() {
    },
    onDbl() {
      e.length >= 3 && (t = !0);
    },
    done() {
      return t && e.length >= 3;
    },
    result() {
      return e.length >= 3 ? { points: e.map((n) => [...n]), closed: t } : null;
    },
    draw(n, r) {
      if (e.length) {
        n.strokeStyle = "#f59e0b", n.fillStyle = "#f59e0b", n.lineWidth = 1.5, n.beginPath(), e.forEach((i, s) => {
          const l = r({ x: i[0], y: i[1] });
          s ? n.lineTo(l.x, l.y) : n.moveTo(l.x, l.y);
        }), t && n.closePath(), n.stroke();
        for (const i of e) {
          const s = r({ x: i[0], y: i[1] });
          n.beginPath(), n.arc(s.x, s.y, 3, 0, Math.PI * 2), n.fill();
        }
      }
    }
  };
}
const gr = { point: Ka, rect: Ja, polygon: Za };
function mo(e, t) {
  gr[e] = t;
}
function si(e) {
  return gr[e] ? gr[e]() : null;
}
var Qa = /* @__PURE__ */ K('<div class="xi-editor svelte-1qrk1oj"><div class="bar svelte-1qrk1oj"><span class="lbl"> </span> <span class="spacer svelte-1qrk1oj"></span> <button class="cancel svelte-1qrk1oj">Cancel</button> <button class="commit svelte-1qrk1oj">Commit</button></div> <canvas class="svelte-1qrk1oj"></canvas></div>');
const eo = {
  hash: "svelte-1qrk1oj",
  code: ":host {display:block;width:100%;height:100%;}.xi-editor.svelte-1qrk1oj {display:flex;flex-direction:column;height:100%;}.bar.svelte-1qrk1oj {display:flex;align-items:center;gap:0.5rem;padding:0.35rem 0.5rem;font:13px system-ui, sans-serif;background:var(--xi-bar-bg, #1f2937);color:#e5e7eb;}.spacer.svelte-1qrk1oj {flex:1;}button.svelte-1qrk1oj {padding:0.2rem 0.7rem;border:0;border-radius:4px;cursor:pointer;font:inherit;}.commit.svelte-1qrk1oj {background:var(--xi-accent, #6366f1);color:#fff;}.cancel.svelte-1qrk1oj {background:#374151;color:#e5e7eb;}canvas.svelte-1qrk1oj {flex:1;width:100%;display:block;background:var(--xi-viewer-bg, #111);cursor:crosshair;touch-action:none;image-rendering:pixelated;}"
};
function to(e, t) {
  Se(t, !0), Le(e, eo);
  let n = N(t, "tool", 7, "rect"), r = N(t, "label", 7, "");
  const i = t.$$host;
  let s;
  const l = ys();
  let a = null, o = si(n());
  const u = (w) => Wa(l, w.x, w.y);
  function d() {
    if (!s) return;
    const w = s.getContext("2d");
    w && (w.imageSmoothingEnabled = !1, w.setTransform(1, 0, 0, 1, 0, 0), w.clearRect(0, 0, s.width, s.height), a && (w.setTransform(l.scale, 0, 0, l.scale, l.panX, l.panY), w.drawImage(a, 0, 0), w.setTransform(1, 0, 0, 1, 0, 0)), o && o.draw(w, u));
  }
  function p() {
    if (!s) return;
    const w = s.getBoundingClientRect();
    s.width = Math.max(1, Math.round(w.width)), s.height = Math.max(1, Math.round(w.height)), l.viewW = s.width, l.viewH = s.height, d();
  }
  function h(w) {
    return !!w && typeof w != "string" && !("dataUrl" in w) && (typeof HTMLImageElement < "u" && w instanceof HTMLImageElement || typeof HTMLCanvasElement < "u" && w instanceof HTMLCanvasElement || typeof OffscreenCanvas < "u" && w instanceof OffscreenCanvas || typeof ImageBitmap < "u" && w instanceof ImageBitmap);
  }
  function v(w) {
    if (h(w)) {
      m(w);
      return;
    }
    const ae = new Image();
    ae.onload = () => m(ae), ae.src = typeof w == "string" ? w : w.dataUrl;
  }
  function m(w) {
    const ae = !l.imgW;
    a = w, l.imgW = w.naturalWidth || w.width, l.imgH = w.naturalHeight || w.height, ae && pr(l), d();
  }
  function g(w) {
    o = si(w) || o, d();
  }
  const c = (w) => {
    const ae = s.getBoundingClientRect();
    return Hr(l, w.clientX - ae.left, w.clientY - ae.top);
  };
  function b(w) {
    o && (o.onDown(c(w)), d());
  }
  function x(w) {
    o && w.buttons && (o.onMove(c(w)), d());
  }
  function _(w) {
    o && (o.onUp(c(w)), d());
  }
  function E(w) {
    o && (o.onDbl(c(w)), d());
  }
  function y(w) {
    if (!a) return;
    w.preventDefault();
    const ae = s.getBoundingClientRect();
    xs(l, w.clientX - ae.left, w.clientY - ae.top, w.deltaY < 0 ? 1.15 : 1 / 1.15), d();
  }
  function $() {
    !o || !o.done() || i.dispatchEvent(new CustomEvent("commit", {
      detail: { tool: o.type, result: o.result() },
      bubbles: !0,
      composed: !0
    }));
  }
  function I() {
    i.dispatchEvent(new CustomEvent("cancel", { detail: {}, bubbles: !0, composed: !0 }));
  }
  Or(() => {
    i.setFrame = v, i.setTool = g, i.getResult = () => o && o.done() ? o.result() : null, p();
    const w = new ResizeObserver(p);
    return w.observe(s), () => w.disconnect();
  });
  var z = {
    get tool() {
      return n();
    },
    set tool(w = "rect") {
      n(w), A();
    },
    get label() {
      return r();
    },
    set label(w = "") {
      r(w), A();
    }
  }, P = Qa(), H = B(P), q = B(H), Q = B(q, !0);
  Y(q);
  var vt = me(q, 4), It = me(vt, 2);
  Y(H);
  var Ge = me(H, 2);
  return jr(Ge, (w) => s = w, () => s), Y(P), te(() => ke(Q, r() || n())), J("click", vt, I), J("click", It, $), J("pointerdown", Ge, b), J("pointermove", Ge, x), J("pointerup", Ge, _), J("dblclick", Ge, E), us("wheel", Ge, y), V(e, P), Ce(z);
}
ht([
  "click",
  "pointerdown",
  "pointermove",
  "pointerup",
  "dblclick"
]);
customElements.define("xi-image-editor", Pe(to, { tool: {}, label: {} }, [], [], { mode: "open" }));
const no = 4003;
function li(e, t) {
  const n = e && typeof e.code == "number" ? e.code : null, r = e && e.reason || "";
  return t && t.busy ? { busy: !0, code: n, reason: "single-client-busy" } : n === no || /single-client-busy/i.test(r) ? { busy: !0, code: n, reason: r || "single-client-busy" } : { busy: !1, code: n, reason: r };
}
class _o {
  /**
   * @param {string} url  e.g. "ws://127.0.0.1:7823/"
   * @param {{WebSocketImpl?: any}} [opts]  inject a WebSocket impl (node tests)
   */
  constructor(t, n = {}) {
    if (this.url = t, this._WS = n.WebSocketImpl || (typeof WebSocket < "u" ? WebSocket : null), !this._WS) throw new Error("no WebSocket implementation (pass opts.WebSocketImpl in node)");
    this.ws = null, this._id = 0, this._pending = /* @__PURE__ */ new Map(), this._listeners = {
      // type -> Set<cb>
      instances: /* @__PURE__ */ new Set(),
      log: /* @__PURE__ */ new Set(),
      event: /* @__PURE__ */ new Set(),
      hello: /* @__PURE__ */ new Set(),
      binary: /* @__PURE__ */ new Set(),
      open: /* @__PURE__ */ new Set(),
      close: /* @__PURE__ */ new Set()
    };
  }
  // Open the socket; resolves once it's open. If opts.checkVersion is set, also
  // runs `cmd:version` and rejects on mismatch (fail-fast on protocol drift).
  //   checkVersion: (info) => boolean | RegExp | string   (string/RegExp tests info.version)
  // Emits an `open` event on connect and a `close` event on disconnect; the close
  // payload is {busy, code, reason} (busy = single-client rejection, see
  // classifyClose). Rejects on any failure BEFORE open (bad URL, refused, 503
  // busy) with an Error carrying `.busy`/`.reason`, so a caller that never got an
  // `open` still gets one settled promise. Auto-reconnect is the caller's job.
  connect(t = {}) {
    return new Promise((n, r) => {
      let i, s = !1;
      try {
        i = new this._WS(this.url);
      } catch (u) {
        r(u);
        return;
      }
      i.binaryType = "arraybuffer", this.ws = i;
      let l = null;
      typeof i.on == "function" && i.on("unexpected-response", (u, d) => {
        const p = d && d.headers && d.headers["x-xi-reason"];
        l = {
          statusCode: d && d.statusCode,
          reason: p,
          busy: d && d.statusCode === 503 && p === "single-client-busy"
        };
      }), i.onmessage = (u) => this._onMessage(u);
      let a = !1;
      const o = (u) => {
        if (a) return;
        a = !0;
        const d = li(u, l);
        this._emit("close", d);
        const p = new Error(d.busy ? "single-client-busy: another client owns the backend" : "connection failed before open");
        p.busy = d.busy, p.reason = d.reason, p.code = d.code, r(p);
      };
      i.onerror = () => {
        for (const { reject: u } of this._pending.values()) u(new Error("socket error"));
        this._pending.clear(), s || o(null);
      }, i.onclose = (u) => {
        for (const { reject: d } of this._pending.values()) d(new Error("socket closed"));
        if (this._pending.clear(), !s) {
          o(u);
          return;
        }
        this._emit("close", li(u, l));
      }, i.onopen = async () => {
        s = !0, this._emit("open", { url: this.url });
        try {
          if (t.checkVersion) {
            const u = await this.cmd("version"), d = u && u.version;
            if (!(typeof t.checkVersion == "function" ? t.checkVersion(u) : t.checkVersion instanceof RegExp ? t.checkVersion.test(d) : d === t.checkVersion)) {
              r(new Error(`backend version mismatch: got ${d}`)), i.close();
              return;
            }
          }
          n(this);
        } catch (u) {
          r(u);
        }
      };
    });
  }
  _onMessage(t) {
    const n = t.data;
    if (n instanceof ArrayBuffer || typeof Buffer < "u" && n instanceof Buffer) {
      this._emit("binary", n);
      return;
    }
    let r;
    try {
      r = JSON.parse(typeof n == "string" ? n : n.toString());
    } catch {
      return;
    }
    if (r.type === "rsp") {
      const i = this._pending.get(r.id);
      i && (this._pending.delete(r.id), r.ok ? i.resolve(this._parseData(r.data)) : i.reject(new Error(r.error || "command failed")));
      return;
    }
    this._listeners[r.type] && this._emit(r.type, r);
  }
  _parseData(t) {
    if (typeof t != "string") return t;
    const n = t.trim();
    if (n.startsWith("{") || n.startsWith("["))
      try {
        return JSON.parse(n);
      } catch {
      }
    return t;
  }
  // --- request/response ---------------------------------------------------
  cmd(t, n) {
    const r = ++this._id;
    return new Promise((i, s) => {
      this._pending.set(r, { resolve: i, reject: s });
      const l = { type: "cmd", id: r, name: t };
      n !== void 0 && (l.args = n);
      try {
        this.ws.send(JSON.stringify(l));
      } catch (a) {
        this._pending.delete(r), s(a);
      }
    });
  }
  // --- convenience verbs --------------------------------------------------
  ping() {
    return this.cmd("ping");
  }
  version() {
    return this.cmd("version");
  }
  listInstances() {
    return this.cmd("list_instances");
  }
  getInstanceDef(t) {
    return this.cmd("get_instance_def", { name: t });
  }
  setInstanceDef(t, n) {
    return this.cmd("set_instance_def", { name: t, def: n });
  }
  exchange(t, n) {
    return this.cmd("exchange_instance", { name: t, cmd: n });
  }
  getState(t) {
    return this.cmd("get_state", { name: t });
  }
  prepareInstance(t, n, r) {
    const i = { name: t, def: n };
    return r !== void 0 && (i.folder = r), this.cmd("prepare_instance", i);
  }
  // sel: { instances?, group?, plugin? }
  commitGroup(t) {
    return this.cmd("commit_group", t);
  }
  run(t) {
    return this.cmd("run", t);
  }
  // --- subscriptions (return an unsubscribe fn) ---------------------------
  on(t, n) {
    const r = this._listeners[t];
    if (!r) throw new Error(`unknown event type: ${t}`);
    return r.add(n), () => r.delete(n);
  }
  onInstances(t) {
    return this.on("instances", t);
  }
  onLog(t) {
    return this.on("log", t);
  }
  onEvent(t) {
    return this.on("event", t);
  }
  // Raw binary passthrough: handler(data) gets the ArrayBuffer/Buffer untouched.
  onBinary(t) {
    return this.on("binary", t);
  }
  // Connection lifecycle. onOpen(cb): cb({url}). onClose(cb): cb({busy, code,
  // reason}) — `busy` true means single-client rejection (retry when they leave).
  onOpen(t) {
    return this.on("open", t);
  }
  onClose(t) {
    return this.on("close", t);
  }
  _emit(t, n) {
    for (const r of this._listeners[t])
      try {
        r(n);
      } catch {
      }
  }
  close() {
    try {
      this.ws && this.ws.close();
    } catch {
    }
  }
}
const ro = {
  slider: "xi-slider",
  number: "xi-number",
  toggle: "xi-toggle",
  radio: "xi-radio",
  dropdown: "xi-dropdown"
};
function io(e, { section: t = "Config", tag: n = "control" } = {}) {
  const r = [];
  for (const [i, s] of Object.entries(e || {})) {
    let l = "number";
    if (typeof s == "boolean") l = "toggle";
    else if (typeof s == "string") l = "text";
    else if (typeof s == "number") l = "number";
    else continue;
    r.push({ type: l, key: i, label: i });
  }
  return r.length ? [{ section: t, tag: n, controls: r }] : [];
}
async function bo(e, t) {
  const { client: n, instance: r, sectionFilter: i } = t, s = e.ownerDocument || globalThis.document, l = await n.getInstanceDef(r) || {}, a = { ...l }, o = t.descriptor && t.descriptor.length ? t.descriptor : io(l), u = [];
  e.innerHTML = "";
  for (const d of o) {
    if (i && !i(d)) continue;
    const p = s.createElement("section");
    if (p.className = "xi-section", p.dataset.tag = d.tag || "control", d.section) {
      const h = s.createElement("h3");
      h.className = "xi-section-title", h.textContent = d.section, p.appendChild(h);
    }
    for (const h of d.controls || []) {
      const v = ro[h.type] || "xi-number", m = s.createElement(v);
      h.label && m.setAttribute("label", h.label);
      for (const c of ["min", "max", "step"]) h[c] != null && m.setAttribute(c, String(h[c]));
      const g = s.createElement("div");
      g.className = "xi-control", g.appendChild(m), p.appendChild(g), h.options != null && (m.options = h.options), h.key in a && (m.value = a[h.key]), m.addEventListener("change", async (c) => {
        a[h.key] = c.detail.value;
        try {
          await n.setInstanceDef(r, { ...a });
        } catch {
        }
        e.dispatchEvent(new CustomEvent("xi-change", { detail: { key: h.key, value: c.detail.value }, bubbles: !0 }));
      }), u.push({ el: m, key: h.key });
    }
    e.appendChild(p);
  }
  return {
    // Re-read the def from the backend and push values back into the controls.
    async refresh() {
      const d = await n.getInstanceDef(r) || {};
      Object.assign(a, d);
      for (const { el: p, key: h } of u) h in a && (p.value = a[h]);
    },
    destroy() {
      e.innerHTML = "";
    }
  };
}
const so = `
  :host { display:block; height:100%; box-sizing:border-box; background:#1e1e1e;
          border:1px solid #333; border-radius:6px; color:#ddd; overflow:hidden;
          font:13px/1.4 system-ui,sans-serif; }
  .hd { padding:4px 8px; font-size:11px; letter-spacing:.04em; text-transform:uppercase;
        color:#888; border-bottom:1px solid #2a2a2a; }
  .body { padding:8px; height:calc(100% - 24px); box-sizing:border-box; }
`;
function Un(e, t) {
  return e.attachShadow({ mode: "open" }), e.shadowRoot.innerHTML = `<style>${so}</style>
    <div class="hd">${t || ""}</div><div class="body"></div>`, e.shadowRoot.querySelector(".body");
}
const lo = (e, t) => e.config && e.config.title || t;
function ws(e) {
  return e == null ? { kind: "none", label: "—", color: "#bbb" } : e <= -99e4 ? { kind: "sys", label: "SYS", color: "#c084fc" } : e > 0 ? { kind: "ok", label: e > 1 ? `OK${e}` : "OK", color: "#3ad17a" } : e < 0 ? { kind: "ng", label: e < -1 ? `NG${-e}` : "NG", color: "#ff5b5b" } : { kind: "na", label: "NA", color: "#ffb454" };
}
class ao extends HTMLElement {
  connectedCallback() {
    this.body = Un(this, lo(this, "Verdict")), this.body.style.cssText = "display:flex;flex-direction:column;align-items:center;justify-content:center;gap:6px;text-align:center", this.big = document.createElement("div"), this.big.style.cssText = "font-weight:800;font-size:clamp(20px,7vw,72px);line-height:1", this.sub = document.createElement("div"), this.sub.style.cssText = "font-size:12px;color:#888;max-width:96%;overflow:hidden;text-overflow:ellipsis;white-space:nowrap", this.body.append(this.big, this.sub);
  }
  feed(t) {
    const n = t.result, r = ws(n ? n.code : null);
    this.big.textContent = r.label, this.big.style.color = r.color, this.sub.textContent = n && n.msg ? n.msg : "";
  }
}
class oo extends HTMLElement {
  connectedCallback() {
    var t, n;
    this.body = Un(this, ((t = this.config) == null ? void 0 : t.title) || "Throughput"), this.windowSec = ((n = this.config) == null ? void 0 : n.windowSec) || 60, this.stamps = [], this.lastResult = -1, this.lastCompute = null, this.body.style.cssText = "display:flex;flex-direction:column;align-items:center;justify-content:center;gap:4px", this.big = document.createElement("div"), this.big.style.cssText = "font-size:clamp(18px,6vw,44px);font-weight:700;color:#9ad", this.sub = document.createElement("div"), this.sub.style.cssText = "font-size:12px;color:#888", this.body.append(this.big, this.sub), this.timer = setInterval(() => this.render(), 1e3);
  }
  disconnectedCallback() {
    this.timer && (clearInterval(this.timer), this.timer = 0);
  }
  feed(t) {
    const n = t.result;
    n && n.run_id != null && n.run_id !== this.lastResult && (this.lastResult = n.run_id, this.stamps.push(Date.now())), t.compute_ms != null && (this.lastCompute = t.compute_ms), this.render();
  }
  render() {
    var l, a;
    const t = Date.now(), n = t - this.windowSec * 1e3;
    for (; this.stamps.length && this.stamps[0] < n; ) this.stamps.shift();
    const r = this.stamps.length, i = r ? Math.max((t - this.stamps[0]) / 1e3, 1) : this.windowSec, s = r > 1 ? r / i * 60 : 0;
    this.big.textContent = `${s.toFixed(0)} /min`, this.sub.textContent = `${r} in ${this.windowSec}s` + (this.lastCompute != null ? ` · compute ${((a = (l = this.lastCompute).toFixed) == null ? void 0 : a.call(l, 1)) ?? this.lastCompute} ms` : "");
  }
}
class uo extends HTMLElement {
  connectedCallback() {
    var t;
    this.body = Un(this, ((t = this.config) == null ? void 0 : t.title) || "Yield"), this.ok = 0, this.ng = 0, this.last = -1, this.body.style.cssText = "display:flex;flex-direction:column;align-items:center;justify-content:center;gap:4px", this.big = document.createElement("div"), this.big.style.cssText = "font-size:clamp(18px,6vw,44px);font-weight:700", this.sub = document.createElement("div"), this.sub.style.cssText = "font-size:12px;color:#888", this.body.append(this.big, this.sub);
  }
  feed(t) {
    var s;
    const n = t.result;
    if (n && n.run_id != null && n.run_id !== this.last) {
      this.last = n.run_id;
      const l = ws(n.code);
      l.kind === "ok" ? this.ok++ : l.kind === "ng" ? this.ng++ : l.kind === "na" && (this.na = (this.na || 0) + 1);
    }
    const r = this.ok + this.ng, i = r ? 100 * this.ok / r : 0;
    this.big.textContent = `${i.toFixed(1)}%`, this.big.style.color = i >= (((s = this.config) == null ? void 0 : s.warn) ?? 95) ? "#3ad17a" : "#ffb454", this.sub.textContent = `OK ${this.ok} / NG ${this.ng}` + (this.na ? ` · NA ${this.na}` : "");
  }
}
class fo extends HTMLElement {
  connectedCallback() {
    var t;
    this.body = Un(this, ((t = this.config) == null ? void 0 : t.title) || "Dispatch groups"), this.body.style.cssText = "display:flex;flex-direction:column;gap:10px;padding:10px;overflow:auto", this.peak = {}, this.rows = {};
  }
  feed(t) {
    const n = t.groups || [];
    if (!n.length) {
      this.body.textContent = "no dispatch groups (legacy single pool)", this.body.style.color = "#888";
      return;
    }
    for (const r of n) {
      const i = r.max_parallel || 1, s = r.running || 0;
      this.peak[r.name] = Math.max(this.peak[r.name] || 0, s);
      let l = this.rows[r.name];
      if (!l) {
        l = document.createElement("div"), l.style.cssText = "display:flex;flex-direction:column;gap:3px";
        const a = document.createElement("div");
        a.style.cssText = "display:flex;justify-content:space-between;font-size:12px";
        const o = document.createElement("span");
        o.style.fontWeight = "600";
        const u = document.createElement("span");
        u.style.color = "#888", a.append(o, u);
        const d = document.createElement("div");
        d.style.cssText = "display:flex;gap:3px;height:18px", l.append(a, d), this.body.appendChild(l), this.rows[r.name] = l = { row: l, name: o, meta: u, bar: d, cells: [] };
      }
      if (l.name.textContent = `${r.name}  ${s}/${i}`, l.name.style.color = s >= i ? "#3ad17a" : s > 0 ? "#9ad" : "#bbb", l.meta.textContent = `q ${r.queue_now ?? 0} · drop ${r.dropped ?? 0} · peak ${this.peak[r.name]}`, l.cells.length !== i) {
        l.bar.replaceChildren(), l.cells = [];
        for (let a = 0; a < i; a++) {
          const o = document.createElement("div");
          o.style.cssText = "flex:1 1 0;border-radius:3px;border:1px solid #333;min-width:6px", l.bar.appendChild(o), l.cells.push(o);
        }
      }
      l.cells.forEach((a, o) => {
        a.style.background = o < s ? "#3ad17a" : "#1a1a1a";
      });
    }
  }
}
const Es = {
  verdict: ao,
  throughput: oo,
  yield: uo,
  groups: fo
};
for (const [e, t] of Object.entries(Es)) customElements.define(`xi-card-${e}`, t);
const Fr = (e) => !!(e && e.card), Rt = (e) => !!(e && (e.dir === "row" || e.dir === "col") && Array.isArray(e.children) && e.children.length >= 1), Ue = (e) => !!(e && Array.isArray(e.tabs) && e.tabs.length >= 1 && e.tabs.every((t) => t && t.child)), wn = () => ({ type: "verdict", bind: {}, config: { title: "(empty)" } });
function Wr(e) {
  const t = e.children.length;
  return (Array.isArray(e.weights) && e.weights.length === t ? e.weights.slice() : Array(t).fill(1)).map((r) => typeof r == "number" && r > 0 ? r : 1);
}
function co(e) {
  const t = Wr(e), n = t.reduce((r, i) => r + i, 0) || 1;
  return t.map((r) => r / n);
}
function $s(e, t) {
  return Ue(e) ? e.tabs[t].child : e.children[t];
}
function ho(e, t, n) {
  if (Ue(e)) {
    const i = e.tabs.slice();
    return i[t] = { ...i[t], child: n }, { ...e, tabs: i };
  }
  const r = e.children.slice();
  return r[t] = n, { ...e, children: r };
}
function mr(e, t, n = []) {
  if (Fr(e)) {
    t(e.card, n);
    return;
  }
  Rt(e) ? e.children.forEach((r, i) => mr(r, t, [...n, i])) : Ue(e) && e.tabs.forEach((r, i) => mr(r.child, t, [...n, i]));
}
function yo(e) {
  let t = 0;
  return mr(e, () => t++), t;
}
function vo(e, t) {
  let n = e;
  for (const r of t)
    if (Rt(n) || Ue(n)) n = $s(n, r);
    else return;
  return n;
}
function Te(e, t, n) {
  if (t.length === 0) return n(e);
  const [r, ...i] = t;
  return ho(e, r, Te($s(e, r), i, n));
}
function xo(e, t, n, r = wn()) {
  return Te(e, t, (i) => ({ dir: n === "col" ? "col" : "row", children: [i, { card: r }], weights: [1, 1] }));
}
function wo(e, t, n, r = wn()) {
  if (n = n === "col" ? "col" : "row", t.length === 0) return { dir: n, children: [e, { card: r }], weights: [1, 1] };
  const i = t.slice(0, -1), s = t[t.length - 1], l = vo(e, i);
  return Rt(l) && l.dir === n ? Te(e, i, (a) => {
    const o = a.children.slice();
    o.splice(s + 1, 0, { card: r });
    const u = Wr(a);
    return u.splice(s + 1, 0, u[s]), { ...a, children: o, weights: u };
  }) : Te(e, t, (a) => ({ dir: n, children: [a, { card: r }], weights: [1, 1] }));
}
function Eo(e, t) {
  if (t.length === 0) return { card: wn() };
  const n = t.slice(0, -1), r = t[t.length - 1];
  return Te(e, n, (i) => {
    if (!Rt(i)) return i;
    const s = i.children.filter((a, o) => o !== r), l = Wr(i).filter((a, o) => o !== r);
    return s.length === 1 ? s[0] : { ...i, children: s, weights: l };
  });
}
function $o(e, t, n) {
  return Te(e, t, () => ({ card: n }));
}
function ko(e, t, n) {
  return Te(e, t, (r) => Rt(r) ? { ...r, weights: n.slice() } : r);
}
function To(e, t) {
  return Te(e, t, (n) => ({ tabs: [{ name: "Page 1", child: n }, { name: "Page 2", child: { card: wn() } }], active: 0 }));
}
function So(e, t, n, r = { card: wn() }) {
  return Te(e, t, (i) => Ue(i) ? { ...i, tabs: [...i.tabs, { name: n || `Page ${i.tabs.length + 1}`, child: r }], active: i.tabs.length } : i);
}
function Co(e, t, n) {
  return Te(e, t, (r) => {
    if (!Ue(r)) return r;
    const i = r.tabs.filter((s, l) => l !== n);
    return i.length === 1 ? i[0].child : { ...r, tabs: i, active: Math.max(0, Math.min(r.active || 0, i.length - 1)) };
  });
}
function Mo(e, t, n, r) {
  return Te(e, t, (i) => Ue(i) ? { ...i, tabs: i.tabs.map((s, l) => l === n ? { ...s, name: r } : s) } : i);
}
function Ao(e, t, n) {
  return Te(e, t, (r) => Ue(r) ? { ...r, active: n } : r);
}
function ai(e, t = "root") {
  return Fr(e) ? e.card.type ? [] : [`${t}: leaf has no card.type`] : Rt(e) ? e.children.flatMap((n, r) => ai(n, `${t}.${r}`)) : Ue(e) ? e.tabs.flatMap((n, r) => ai(n.child, `${t}.${n.name || r}`)) : [`${t}: node is not a leaf {card}, split {dir,children}, or tabs {tabs}`];
}
function Oo(e, { client: t, dashboard: n, pollStatsMs: r = 200 }) {
  const i = e.ownerDocument || globalThis.document, s = globalThis.requestAnimationFrame || ((c) => setTimeout(c, 16)), l = { run_id: -1, compute_ms: null, status: null, result: null, groups: [] };
  let a = [], o = 0;
  function u() {
    o || (o = s(() => {
      o = 0;
      for (const c of a)
        try {
          c.feed(l);
        } catch {
        }
    }));
  }
  function d(c) {
    const b = Es[c.type], x = i.createElement(b ? `xi-card-${c.type}` : "div");
    return b || (x.textContent = `unknown card: ${c.type}`, x.style.cssText = "color:#f88;padding:8px"), x.binding = c.bind || {}, x.config = c.config || {}, x.style.minWidth = "0", x.style.minHeight = "0", x.style.overflow = "hidden", b && a.push(x), x;
  }
  function p(c) {
    let b = Math.min(c.active || 0, c.tabs.length - 1);
    const x = i.createElement("div");
    x.style.cssText = "display:flex;flex-direction:column;min-width:0;min-height:0;width:100%;height:100%";
    const _ = i.createElement("div");
    _.style.cssText = "display:flex;gap:2px;flex:0 0 auto;align-items:center;padding:2px 2px 0;overflow:auto";
    const E = i.createElement("div");
    E.style.cssText = "flex:1 1 0;min-width:0;min-height:0;position:relative";
    const y = [], $ = [], I = () => {
      y.forEach((z, P) => {
        const H = P === b;
        z.style.background = H ? "#1e1e1e" : "#121212", z.style.color = H ? "#ddd" : "#888";
      }), $.forEach((z, P) => {
        z.style.display = P === b ? "" : "none";
      });
    };
    return c.tabs.forEach((z, P) => {
      const H = i.createElement("div");
      H.style.cssText = "padding:4px 11px;cursor:pointer;border-radius:6px 6px 0 0;border:1px solid #333;border-bottom:none;font:12px system-ui,sans-serif", H.textContent = z.name || `Page ${P + 1}`, H.onclick = () => {
        b = P, I();
      }, y.push(H), _.appendChild(H);
      const q = h(z.child);
      q.style.cssText += ";position:absolute;inset:0;min-width:0;min-height:0;border:1px solid #333;border-radius:0 6px 6px 6px;overflow:hidden", $.push(q), E.appendChild(q);
    }), I(), x.append(_, E), x;
  }
  function h(c) {
    if (Fr(c)) return d(c.card);
    if (Ue(c)) return p(c);
    if (!Rt(c)) {
      const E = i.createElement("div");
      return E.textContent = "bad layout node", E.style.color = "#f88", E;
    }
    const b = c.dir === "col", x = i.createElement("div");
    x.style.cssText = `display:flex;flex-direction:${b ? "column" : "row"};min-width:0;min-height:0;width:100%;height:100%`;
    const _ = co(c);
    return c.children.forEach((E, y) => {
      const $ = h(E);
      $.style.flex = `${_[y]} 1 0`, $.style.minWidth = "0", $.style.minHeight = "0", x.appendChild($);
    }), x;
  }
  function v() {
    a = [], e.replaceChildren(), e.style.cssText += ";display:flex;min-width:0;min-height:0";
    const c = n && n.layout;
    if (!c) return;
    const b = h(c);
    b.style.flex = "1 1 0", b.style.minWidth = "0", b.style.minHeight = "0", e.appendChild(b), u();
  }
  const m = [
    t.onEvent((c) => {
      c.name === "run_finished" && c.data ? (typeof c.data.run_id == "number" && (l.run_id = c.data.run_id), typeof c.data.inspect_compute_us == "number" ? l.compute_ms = c.data.inspect_compute_us / 1e3 : typeof c.data.ms == "number" && (l.compute_ms = c.data.ms), u()) : c.name === "run_result" && c.data ? (l.result = c.data, u()) : c.name === "status" && (l.status = c.data, u());
    })
  ], g = setInterval(() => {
    t.cmd("dispatch_stats").then((c) => {
      c && Array.isArray(c.groups) && (l.groups = c.groups, u());
    }).catch(() => {
    });
  }, r);
  return v(), {
    setDashboard(c) {
      n = c, v();
    },
    state: l,
    destroy() {
      m.forEach((c) => c()), clearInterval(g), e.replaceChildren();
    }
  };
}
const No = [
  "xi-slider",
  "xi-number",
  "xi-toggle",
  "xi-radio",
  "xi-dropdown",
  "xi-text",
  "xi-button",
  "xi-badge",
  "xi-trace",
  "xi-image-viewer",
  "xi-image-editor"
];
export {
  no as BUSY_CLOSE_CODE,
  Es as CARDS,
  ro as CONTROL_TAGS,
  gr as TOOLS,
  No as XI_COMPONENTS,
  _o as XiClient,
  wo as addSibling,
  So as addTab,
  yo as countLeaves,
  mr as eachLeaf,
  wn as emptyCard,
  vo as getNode,
  io as inferDescriptor,
  Fr as isLeaf,
  Rt as isSplit,
  Ue as isTabs,
  si as makeTool,
  Oo as mountDashboard,
  bo as mountPanel,
  mo as registerTool,
  Eo as removePane,
  Co as removeTab,
  Mo as renameTab,
  Ao as setActive,
  $o as setCard,
  ko as setWeights,
  xo as splitLeaf,
  ai as validate,
  co as weightsOf,
  To as wrapInTabs
};
