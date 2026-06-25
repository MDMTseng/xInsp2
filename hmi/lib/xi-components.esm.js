var cs = Object.defineProperty;
var Ir = (e) => {
  throw TypeError(e);
};
var ds = (e, t, n) => t in e ? cs(e, t, { enumerable: !0, configurable: !0, writable: !0, value: n }) : e[t] = n;
var j = (e, t, n) => ds(e, typeof t != "symbol" ? t + "" : t, n), Yn = (e, t, n) => t.has(e) || Ir("Cannot " + n);
var f = (e, t, n) => (Yn(e, t, "read from private field"), n ? n.call(e) : t.get(e)), k = (e, t, n) => t.has(e) ? Ir("Cannot add the same private member more than once") : t instanceof WeakSet ? t.add(e) : t.set(e, n), E = (e, t, n, r) => (Yn(e, t, "write to private field"), r ? r.call(e, n) : t.set(e, n), n), C = (e, t, n) => (Yn(e, t, "access private method"), n);
var Gr;
typeof window < "u" && ((Gr = window.__svelte ?? (window.__svelte = {})).v ?? (Gr.v = /* @__PURE__ */ new Set())).add("5");
const hs = 1, vs = 2, Zr = 4, ps = 8, _s = 16, gs = 1, ms = 4, bs = 8, ys = 16, ws = 2, Qr = "[", fr = "[!", Dr = "[?", ur = "]", Wt = {}, q = Symbol("uninitialized"), xs = "http://www.w3.org/1999/xhtml", ei = !1;
var cr = Array.isArray, Es = Array.prototype.indexOf, $n = Array.prototype.includes, Pn = Array.from, kn = Object.keys, Sn = Object.defineProperty, yt = Object.getOwnPropertyDescriptor, $s = Object.getOwnPropertyDescriptors, ks = Object.prototype, Ss = Array.prototype, ti = Object.getPrototypeOf, Pr = Object.isExtensible;
const Ts = () => {
};
function Cs(e) {
  for (var t = 0; t < e.length; t++)
    e[t]();
}
function ni() {
  var e, t, n = new Promise((r, i) => {
    e = r, t = i;
  });
  return { promise: n, resolve: e, reject: t };
}
const G = 2, Yt = 4, Ln = 8, ri = 1 << 24, Ae = 16, Oe = 32, Ze = 64, Xn = 128, we = 512, B = 1024, U = 2048, He = 4096, te = 8192, he = 16384, Tt = 32768, zn = 1 << 25, qt = 65536, Tn = 1 << 17, Ms = 1 << 18, Ct = 1 << 19, As = 1 << 20, Le = 1 << 25, kt = 65536, Cn = 1 << 21, Rt = 1 << 22, at = 1 << 23, wt = Symbol("$state"), ii = Symbol("legacy props"), Ns = Symbol(""), mn = Symbol("attributes"), Os = Symbol("class"), Rs = Symbol("style"), zt = Symbol("text"), si = Symbol("form reset"), jn = new class extends Error {
  constructor() {
    super(...arguments);
    j(this, "name", "StaleReactionError");
    j(this, "message", "The reaction that called `getAbortSignal()` was re-run or destroyed");
  }
}();
var Kr;
const li = (
  // We gotta write it like this because after downleveling the pure comment may end up in the wrong location
  !!((Kr = globalThis.document) != null && Kr.contentType) && /* @__PURE__ */ globalThis.document.contentType.includes("xml")
), dr = 3, dn = 8;
function Is() {
  throw new Error("https://svelte.dev/e/async_derived_orphan");
}
function Ds(e, t, n) {
  throw new Error("https://svelte.dev/e/each_key_duplicate");
}
function Ps(e) {
  throw new Error("https://svelte.dev/e/effect_in_teardown");
}
function Ls() {
  throw new Error("https://svelte.dev/e/effect_in_unowned_derived");
}
function js(e) {
  throw new Error("https://svelte.dev/e/effect_orphan");
}
function Fs() {
  throw new Error("https://svelte.dev/e/effect_update_depth_exceeded");
}
function Hs() {
  throw new Error("https://svelte.dev/e/hydration_failed");
}
function Ws(e) {
  throw new Error("https://svelte.dev/e/props_invalid_value");
}
function Ys() {
  throw new Error("https://svelte.dev/e/state_descriptors_fixed");
}
function qs() {
  throw new Error("https://svelte.dev/e/state_prototype_fixed");
}
function Bs() {
  throw new Error("https://svelte.dev/e/state_unsafe_mutation");
}
function Vs() {
  throw new Error("https://svelte.dev/e/svelte_boundary_reset_onerror");
}
function Us() {
  console.warn("https://svelte.dev/e/derived_inert");
}
function Fn(e) {
  console.warn("https://svelte.dev/e/hydration_mismatch");
}
function Xs() {
  console.warn("https://svelte.dev/e/select_multiple_invalid_value");
}
function zs() {
  console.warn("https://svelte.dev/e/svelte_boundary_reset_noop");
}
let D = !1;
function Ge(e) {
  D = e;
}
let P;
function ve(e) {
  if (e === null)
    throw Fn(), Wt;
  return P = e;
}
function Hn() {
  return ve(/* @__PURE__ */ nt(P));
}
function W(e) {
  if (D) {
    if (/* @__PURE__ */ nt(P) !== null)
      throw Fn(), Wt;
    P = e;
  }
}
function Gs(e = 1) {
  if (D) {
    for (var t = e, n = P; t--; )
      n = /** @type {TemplateNode} */
      /* @__PURE__ */ nt(n);
    P = n;
  }
}
function Mn(e = !0) {
  for (var t = 0, n = P; ; ) {
    if (n.nodeType === dn) {
      var r = (
        /** @type {Comment} */
        n.data
      );
      if (r === ur) {
        if (t === 0) return n;
        t -= 1;
      } else (r === Qr || r === fr || // "[1", "[2", etc. for if blocks
      r[0] === "[" && !isNaN(Number(r.slice(1)))) && (t += 1);
    }
    var i = (
      /** @type {TemplateNode} */
      /* @__PURE__ */ nt(n)
    );
    e && n.remove(), n = i;
  }
}
function ai(e) {
  if (!e || e.nodeType !== dn)
    throw Fn(), Wt;
  return (
    /** @type {Comment} */
    e.data
  );
}
function oi(e) {
  return e === this.v;
}
function Ks(e, t) {
  return e != e ? t == t : e !== t || e !== null && typeof e == "object" || typeof e == "function";
}
function fi(e) {
  return !Ks(e, this.v);
}
let Js = !1, ne = null;
function Bt(e) {
  ne = e;
}
function et(e, t = !1, n) {
  ne = {
    p: ne,
    i: !1,
    c: null,
    e: null,
    s: e,
    x: null,
    r: (
      /** @type {Effect} */
      S
    ),
    l: null
  };
}
function tt(e) {
  var t = (
    /** @type {ComponentContext} */
    ne
  ), n = t.e;
  if (n !== null) {
    t.e = null;
    for (var r of n)
      Li(r);
  }
  return e !== void 0 && (t.x = e), t.i = !0, ne = t.p, e ?? /** @type {T} */
  {};
}
function ui() {
  return !0;
}
let ct = [];
function ci() {
  var e = ct;
  ct = [], Cs(e);
}
function Ke(e) {
  if (ct.length === 0 && !en) {
    var t = ct;
    queueMicrotask(() => {
      t === ct && ci();
    });
  }
  ct.push(e);
}
function Zs() {
  for (; ct.length > 0; )
    ci();
}
function di(e) {
  var t = S;
  if (t === null)
    return T.f |= at, e;
  if ((t.f & Tt) === 0 && (t.f & Yt) === 0)
    throw e;
  lt(e, t);
}
function lt(e, t) {
  if (!(t !== null && (t.f & he) !== 0)) {
    for (; t !== null; ) {
      if ((t.f & Xn) !== 0) {
        if ((t.f & Tt) === 0)
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
const Qs = -7169;
function H(e, t) {
  e.f = e.f & Qs | t;
}
function hr(e) {
  (e.f & we) !== 0 || e.deps === null ? H(e, B) : H(e, He);
}
function hi(e) {
  if (e !== null)
    for (const t of e)
      (t.f & G) === 0 || (t.f & kt) === 0 || (t.f ^= kt, hi(
        /** @type {Derived} */
        t.deps
      ));
}
function vi(e, t, n) {
  (e.f & U) !== 0 ? t.add(e) : (e.f & He) !== 0 && n.add(e), hi(e.deps), H(e, B);
}
let _n = !1;
function el(e) {
  var t = _n;
  try {
    return _n = !1, [e(), _n];
  } finally {
    _n = t;
  }
}
function tl(e) {
  let t = 0, n = St(0), r;
  return () => {
    br() && (M(n), xr(() => (t === 0 && (r = Sr(() => e(() => tn(n)))), t += 1, () => {
      Ke(() => {
        t -= 1, t === 0 && (r == null || r(), r = void 0, tn(n));
      });
    })));
  };
}
var nl = qt | Ct;
function rl(e, t, n, r) {
  new il(e, t, n, r);
}
var ue, ln, ge, pt, se, me, ee, ce, Be, _t, it, It, an, on, Ve, Rn, L, pi, _i, gi, Gn, bn, yn, Kn, Jn;
class il {
  /**
   * @param {TemplateNode} node
   * @param {BoundaryProps} props
   * @param {((anchor: Node) => void)} children
   * @param {((error: unknown) => unknown) | undefined} [transform_error]
   */
  constructor(t, n, r, i) {
    k(this, L);
    /** @type {Boundary | null} */
    j(this, "parent");
    j(this, "is_pending", !1);
    /**
     * API-level transformError transform function. Transforms errors before they reach the `failed` snippet.
     * Inherited from parent boundary, or defaults to identity.
     * @type {(error: unknown) => unknown}
     */
    j(this, "transform_error");
    /** @type {TemplateNode} */
    k(this, ue);
    /** @type {TemplateNode | null} */
    k(this, ln, D ? P : null);
    /** @type {BoundaryProps} */
    k(this, ge);
    /** @type {((anchor: Node) => void)} */
    k(this, pt);
    /** @type {Effect} */
    k(this, se);
    /** @type {Effect | null} */
    k(this, me, null);
    /** @type {Effect | null} */
    k(this, ee, null);
    /** @type {Effect | null} */
    k(this, ce, null);
    /** @type {DocumentFragment | null} */
    k(this, Be, null);
    k(this, _t, 0);
    k(this, it, 0);
    k(this, It, !1);
    /** @type {Set<Effect>} */
    k(this, an, /* @__PURE__ */ new Set());
    /** @type {Set<Effect>} */
    k(this, on, /* @__PURE__ */ new Set());
    /**
     * A source containing the number of pending async deriveds/expressions.
     * Only created if `$effect.pending()` is used inside the boundary,
     * otherwise updating the source results in needless `Batch.ensure()`
     * calls followed by no-op flushes
     * @type {Source<number> | null}
     */
    k(this, Ve, null);
    k(this, Rn, tl(() => (E(this, Ve, St(f(this, _t))), () => {
      E(this, Ve, null);
    })));
    var s;
    E(this, ue, t), E(this, ge, n), E(this, pt, (l) => {
      var a = (
        /** @type {Effect} */
        S
      );
      a.b = this, a.f |= Xn, r(l);
    }), this.parent = /** @type {Effect} */
    S.b, this.transform_error = i ?? ((s = this.parent) == null ? void 0 : s.transform_error) ?? ((l) => l), E(this, se, Er(() => {
      if (D) {
        const l = (
          /** @type {Comment} */
          f(this, ln)
        );
        Hn();
        const a = l.data === fr;
        if (l.data.startsWith(Dr)) {
          const c = JSON.parse(l.data.slice(Dr.length));
          C(this, L, _i).call(this, c);
        } else a ? C(this, L, gi).call(this) : C(this, L, pi).call(this);
      } else
        C(this, L, Gn).call(this);
    }, nl)), D && E(this, ue, P);
  }
  /**
   * Defer an effect inside a pending boundary until the boundary resolves
   * @param {Effect} effect
   */
  defer_effect(t) {
    vi(t, f(this, an), f(this, on));
  }
  /**
   * Returns `false` if the effect exists inside a boundary whose pending snippet is shown
   * @returns {boolean}
   */
  is_rendered() {
    return !this.is_pending && (!this.parent || this.parent.is_rendered());
  }
  has_pending_snippet() {
    return !!f(this, ge).pending;
  }
  /**
   * Update the source that powers `$effect.pending()` inside this boundary,
   * and controls when the current `pending` snippet (if any) is removed.
   * Do not call from inside the class
   * @param {1 | -1} d
   * @param {Batch} batch
   */
  update_pending_count(t, n) {
    C(this, L, Kn).call(this, t, n), E(this, _t, f(this, _t) + t), !(!f(this, Ve) || f(this, It)) && (E(this, It, !0), Ke(() => {
      E(this, It, !1), f(this, Ve) && Vt(f(this, Ve), f(this, _t));
    }));
  }
  get_effect_pending() {
    return f(this, Rn).call(this), M(
      /** @type {Source<number>} */
      f(this, Ve)
    );
  }
  /** @param {unknown} error */
  error(t) {
    if (!f(this, ge).onerror && !f(this, ge).failed)
      throw t;
    $ != null && $.is_fork ? (f(this, me) && $.skip_effect(f(this, me)), f(this, ee) && $.skip_effect(f(this, ee)), f(this, ce) && $.skip_effect(f(this, ce)), $.oncommit(() => {
      C(this, L, Jn).call(this, t);
    })) : C(this, L, Jn).call(this, t);
  }
}
ue = new WeakMap(), ln = new WeakMap(), ge = new WeakMap(), pt = new WeakMap(), se = new WeakMap(), me = new WeakMap(), ee = new WeakMap(), ce = new WeakMap(), Be = new WeakMap(), _t = new WeakMap(), it = new WeakMap(), It = new WeakMap(), an = new WeakMap(), on = new WeakMap(), Ve = new WeakMap(), Rn = new WeakMap(), L = new WeakSet(), pi = function() {
  try {
    E(this, me, ye(() => f(this, pt).call(this, f(this, ue))));
  } catch (t) {
    this.error(t);
  }
}, /**
 * @param {unknown} error The deserialized error from the server's hydration comment
 */
_i = function(t) {
  const n = f(this, ge).failed;
  n && E(this, ce, ye(() => {
    n(
      f(this, ue),
      () => t,
      () => () => {
      }
    );
  }));
}, gi = function() {
  const t = f(this, ge).pending;
  t && (this.is_pending = !0, E(this, ee, ye(() => t(f(this, ue)))), Ke(() => {
    var n = E(this, Be, document.createDocumentFragment()), r = je();
    n.append(r), E(this, me, C(this, L, yn).call(this, () => ye(() => f(this, pt).call(this, r)))), f(this, it) === 0 && (f(this, ue).before(n), E(this, Be, null), Et(
      /** @type {Effect} */
      f(this, ee),
      () => {
        E(this, ee, null);
      }
    ), C(this, L, bn).call(
      this,
      /** @type {Batch} */
      $
    ));
  }));
}, Gn = function() {
  try {
    if (this.is_pending = this.has_pending_snippet(), E(this, it, 0), E(this, _t, 0), E(this, me, ye(() => {
      f(this, pt).call(this, f(this, ue));
    })), f(this, it) > 0) {
      var t = E(this, Be, document.createDocumentFragment());
      kr(f(this, me), t);
      const n = (
        /** @type {(anchor: Node) => void} */
        f(this, ge).pending
      );
      E(this, ee, ye(() => n(f(this, ue))));
    } else
      C(this, L, bn).call(
        this,
        /** @type {Batch} */
        $
      );
  } catch (n) {
    this.error(n);
  }
}, /**
 * @param {Batch} batch
 */
bn = function(t) {
  this.is_pending = !1, t.transfer_effects(f(this, an), f(this, on));
}, /**
 * @template T
 * @param {() => T} fn
 */
yn = function(t) {
  var n = S, r = T, i = ne;
  We(f(this, se)), Ee(f(this, se)), Bt(f(this, se).ctx);
  try {
    return ot.ensure(), t();
  } catch (s) {
    return di(s), null;
  } finally {
    We(n), Ee(r), Bt(i);
  }
}, /**
 * Updates the pending count associated with the currently visible pending snippet,
 * if any, such that we can replace the snippet with content once work is done
 * @param {1 | -1} d
 * @param {Batch} batch
 */
Kn = function(t, n) {
  var r;
  if (!this.has_pending_snippet()) {
    this.parent && C(r = this.parent, L, Kn).call(r, t, n);
    return;
  }
  E(this, it, f(this, it) + t), f(this, it) === 0 && (C(this, L, bn).call(this, n), f(this, ee) && Et(f(this, ee), () => {
    E(this, ee, null);
  }), f(this, Be) && (f(this, ue).before(f(this, Be)), E(this, Be, null)));
}, /**
 * @param {unknown} error
 */
Jn = function(t) {
  f(this, me) && (re(f(this, me)), E(this, me, null)), f(this, ee) && (re(f(this, ee)), E(this, ee, null)), f(this, ce) && (re(f(this, ce)), E(this, ce, null)), D && (ve(
    /** @type {TemplateNode} */
    f(this, ln)
  ), Gs(), ve(Mn()));
  var n = f(this, ge).onerror;
  let r = f(this, ge).failed;
  var i = !1, s = !1;
  const l = () => {
    if (i) {
      zs();
      return;
    }
    i = !0, s && Vs(), f(this, ce) !== null && Et(f(this, ce), () => {
      E(this, ce, null);
    }), C(this, L, yn).call(this, () => {
      C(this, L, Gn).call(this);
    });
  }, a = (o) => {
    try {
      s = !0, n == null || n(o, l), s = !1;
    } catch (c) {
      lt(c, f(this, se) && f(this, se).parent);
    }
    r && E(this, ce, C(this, L, yn).call(this, () => {
      try {
        return ye(() => {
          var c = (
            /** @type {Effect} */
            S
          );
          c.b = this, c.f |= Xn, r(
            f(this, ue),
            () => o,
            () => l
          );
        });
      } catch (c) {
        return lt(
          c,
          /** @type {Effect} */
          f(this, se).parent
        ), null;
      }
    }));
  };
  Ke(() => {
    var o;
    try {
      o = this.transform_error(t);
    } catch (c) {
      lt(c, f(this, se) && f(this, se).parent);
      return;
    }
    o !== null && typeof o == "object" && typeof /** @type {any} */
    o.then == "function" ? o.then(
      a,
      /** @param {unknown} e */
      (c) => lt(c, f(this, se) && f(this, se).parent)
    ) : a(o);
  });
};
function sl(e, t, n, r) {
  const i = nn;
  var s = e.filter((h) => !h.settled), l = t.map(i);
  if (n.length === 0 && s.length === 0) {
    r(l);
    return;
  }
  var a = (
    /** @type {Effect} */
    S
  ), o = ll(), c = s.length === 1 ? s[0].promise : s.length > 1 ? Promise.all(s.map((h) => h.promise)) : null;
  function d(h) {
    if ((a.f & he) === 0) {
      o();
      try {
        r([...l, ...h]);
      } catch (g) {
        lt(g, a);
      }
      An();
    }
  }
  var m = mi();
  if (n.length === 0) {
    c.then(() => d([])).finally(m);
    return;
  }
  function u() {
    Promise.all(n.map((h) => /* @__PURE__ */ al(h))).then(d).catch((h) => lt(h, a)).finally(m);
  }
  c ? c.then(() => {
    o(), u(), An();
  }) : u();
}
function ll() {
  var e = (
    /** @type {Effect} */
    S
  ), t = T, n = ne, r = (
    /** @type {Batch} */
    $
  );
  return function(s = !0) {
    We(e), Ee(t), Bt(n), s && (e.f & he) === 0 && (r == null || r.activate(), r == null || r.apply());
  };
}
function An(e = !0) {
  We(null), Ee(null), Bt(null), e && ($ == null || $.deactivate());
}
function mi() {
  var e = (
    /** @type {Effect} */
    S
  ), t = e.b, n = (
    /** @type {Batch} */
    $
  ), r = !!(t != null && t.is_rendered());
  return t == null || t.update_pending_count(1, n), n.increment(r, e), () => {
    t == null || t.update_pending_count(-1, n), n.decrement(r, e);
  };
}
// @__NO_SIDE_EFFECTS__
function nn(e) {
  var t = G | U;
  return S !== null && (S.f |= Ct), {
    ctx: ne,
    deps: null,
    effects: null,
    equals: oi,
    f: t,
    fn: e,
    reactions: null,
    rv: 0,
    v: (
      /** @type {V} */
      q
    ),
    wv: 0,
    parent: S,
    ac: null
  };
}
const Gt = Symbol("obsolete");
// @__NO_SIDE_EFFECTS__
function al(e, t, n) {
  let r = (
    /** @type {Effect | null} */
    S
  );
  r === null && Is();
  var i = (
    /** @type {Promise<V>} */
    /** @type {unknown} */
    void 0
  ), s = St(
    /** @type {V} */
    q
  ), l = !T, a = /* @__PURE__ */ new Set();
  return yl(() => {
    var h, g;
    var o = (
      /** @type {Effect} */
      S
    ), c = ni();
    i = c.promise;
    try {
      Promise.resolve(e()).then(c.resolve, (_) => {
        _ !== jn && c.reject(_);
      }).finally(An);
    } catch (_) {
      c.reject(_), An();
    }
    var d = (
      /** @type {Batch} */
      $
    );
    if (l) {
      if ((o.f & Tt) !== 0)
        var m = mi();
      if (
        // boundary can be null if the async derived is inside an $effect.root not connected to the component render tree
        (h = r.b) != null && h.is_rendered()
      )
        (g = d.async_deriveds.get(o)) == null || g.reject(Gt);
      else
        for (const _ of a.values())
          _.reject(Gt);
      a.add(c), d.async_deriveds.set(o, c);
    }
    const u = (_, p = void 0) => {
      m == null || m(), a.delete(c), p !== Gt && (d.activate(), p ? (s.f |= at, Vt(s, p)) : ((s.f & at) !== 0 && (s.f ^= at), Vt(s, _)), d.deactivate());
    };
    c.promise.then(u, (_) => u(null, _ || "unknown"));
  }), yr(() => {
    for (const o of a)
      o.reject(Gt);
  }), new Promise((o) => {
    function c(d) {
      function m() {
        d === i ? o(s) : c(i);
      }
      d.then(m, m);
    }
    c(i);
  });
}
// @__NO_SIDE_EFFECTS__
function bi(e) {
  const t = /* @__PURE__ */ nn(e);
  return qi(t), t;
}
// @__NO_SIDE_EFFECTS__
function yi(e) {
  const t = /* @__PURE__ */ nn(e);
  return t.equals = fi, t;
}
function ol(e) {
  var t = e.effects;
  if (t !== null) {
    e.effects = null;
    for (var n = 0; n < t.length; n += 1)
      re(
        /** @type {Effect} */
        t[n]
      );
  }
}
function vr(e) {
  var t, n = S, r = e.parent;
  if (!Qe && r !== null && e.v !== q && // if it was never evaluated before, it's guaranteed to fail downstream, so we try to execute instead
  (r.f & (he | te)) !== 0)
    return Us(), e.v;
  We(r);
  try {
    e.f &= ~kt, ol(e), t = Xi(e);
  } finally {
    We(n);
  }
  return t;
}
function wi(e) {
  var t = vr(e);
  if (!e.equals(t) && (e.wv = Vi(), (!($ != null && $.is_fork) || e.deps === null) && ($ !== null ? ($.capture(e, t, !0), Qt == null || Qt.capture(e, t, !0)) : e.v = t, e.deps === null))) {
    H(e, B);
    return;
  }
  Qe || (z !== null ? (br() || $ != null && $.is_fork) && z.set(e, t) : hr(e));
}
function fl(e) {
  var t, n;
  if (e.effects !== null)
    for (const r of e.effects)
      (r.teardown || r.ac) && ((t = r.teardown) == null || t.call(r), (n = r.ac) == null || n.abort(jn), r.fn !== null && (r.teardown = Ts), r.ac = null, sn(r, 0), $r(r));
}
function xi(e) {
  if (e.effects !== null)
    for (const t of e.effects)
      t.teardown && t.fn !== null && Ut(t);
}
let qn = null, At = null, $ = null, Qt = null, z = null, Zn = null, en = !1, Bn = !1, Ot = null, wn = null;
var Lr = 0;
let ul = 1;
var Dt, st, gt, Pt, Lt, jt, Ue, Ft, le, fn, Xe, Te, De, Ht, mt, R, Qn, Kt, er, Ei, $i, Nt, cl, Jt;
const In = class In {
  constructor() {
    k(this, R);
    j(this, "id", ul++);
    /** True as soon as `#process` was called */
    k(this, Dt, !1);
    j(this, "linked", !0);
    /** @type {Batch | null} */
    k(this, st, null);
    /** @type {Batch | null} */
    k(this, gt, null);
    /** @type {Map<Effect, ReturnType<typeof deferred<any>>>} */
    j(this, "async_deriveds", /* @__PURE__ */ new Map());
    /**
     * The current values of any signals that are updated in this batch.
     * Tuple format: [value, is_derived] (note: is_derived is false for deriveds, too, if they were overridden via assignment)
     * They keys of this map are identical to `this.#previous`
     * @type {Map<Value, [any, boolean]>}
     */
    j(this, "current", /* @__PURE__ */ new Map());
    /**
     * The values of any signals (sources and deriveds) that are updated in this batch _before_ those updates took place.
     * They keys of this map are identical to `this.#current`
     * @type {Map<Value, any>}
     */
    j(this, "previous", /* @__PURE__ */ new Map());
    /**
     * When the batch is committed (and the DOM is updated), we need to remove old branches
     * and append new ones by calling the functions added inside (if/each/key/etc) blocks
     * @type {Set<(batch: Batch) => void>}
     */
    k(this, Pt, /* @__PURE__ */ new Set());
    /**
     * If a fork is discarded, we need to destroy any effects that are no longer needed
     * @type {Set<(batch: Batch) => void>}
     */
    k(this, Lt, /* @__PURE__ */ new Set());
    /**
     * The number of async effects that are currently in flight
     */
    k(this, jt, 0);
    /**
     * Async effects that are currently in flight, _not_ inside a pending boundary
     * @type {Map<Effect, number>}
     */
    k(this, Ue, /* @__PURE__ */ new Map());
    /**
     * A deferred that resolves when the batch is committed, used with `settled()`
     * TODO replace with Promise.withResolvers once supported widely enough
     * @type {{ promise: Promise<void>, resolve: (value?: any) => void, reject: (reason: unknown) => void } | null}
     */
    k(this, Ft, null);
    /**
     * The root effects that need to be flushed
     * @type {Effect[]}
     */
    k(this, le, []);
    /**
     * Effects created while this batch was active.
     * @type {Effect[]}
     */
    k(this, fn, []);
    /**
     * Deferred effects (which run after async work has completed) that are DIRTY
     * @type {Set<Effect>}
     */
    k(this, Xe, /* @__PURE__ */ new Set());
    /**
     * Deferred effects that are MAYBE_DIRTY
     * @type {Set<Effect>}
     */
    k(this, Te, /* @__PURE__ */ new Set());
    /**
     * A map of branches that still exist, but will be destroyed when this batch
     * is committed — we skip over these during `process`.
     * The value contains child effects that were dirty/maybe_dirty before being reset,
     * so they can be rescheduled if the branch survives.
     * @type {Map<Effect, { d: Effect[], m: Effect[] }>}
     */
    k(this, De, /* @__PURE__ */ new Map());
    /**
     * Inverse of #skipped_branches which we need to tell prior batches to unskip them when committing
     * @type {Set<Effect>}
     */
    k(this, Ht, /* @__PURE__ */ new Set());
    j(this, "is_fork", !1);
    k(this, mt, !1);
    At === null ? qn = At = this : (E(At, gt, this), E(this, st, At)), At = this;
  }
  /**
   * Add an effect to the #skipped_branches map and reset its children
   * @param {Effect} effect
   */
  skip_effect(t) {
    f(this, De).has(t) || f(this, De).set(t, { d: [], m: [] }), f(this, Ht).delete(t);
  }
  /**
   * Remove an effect from the #skipped_branches map and reschedule
   * any tracked dirty/maybe_dirty child effects
   * @param {Effect} effect
   * @param {(e: Effect) => void} callback
   */
  unskip_effect(t, n = (r) => this.schedule(r)) {
    var r = f(this, De).get(t);
    if (r) {
      f(this, De).delete(t);
      for (var i of r.d)
        H(i, U), n(i);
      for (i of r.m)
        H(i, He), n(i);
    }
    f(this, Ht).add(t);
  }
  /**
   * Associate a change to a given source with the current
   * batch, noting its previous and current values
   * @param {Value} source
   * @param {any} value
   * @param {boolean} [is_derived]
   */
  capture(t, n, r = !1) {
    t.v !== q && !this.previous.has(t) && this.previous.set(t, t.v), (t.f & at) === 0 && (this.current.set(t, [n, r]), z == null || z.set(t, n)), this.is_fork || (t.v = n);
  }
  activate() {
    $ = this;
  }
  deactivate() {
    $ = null, z = null;
  }
  flush() {
    try {
      Bn = !0, $ = this, C(this, R, Kt).call(this);
    } finally {
      Lr = 0, Zn = null, Ot = null, wn = null, Bn = !1, $ = null, z = null, xt.clear();
    }
  }
  discard() {
    var t;
    for (const n of f(this, Lt)) n(this);
    f(this, Lt).clear();
    for (const n of this.async_deriveds.values())
      n.reject(Gt);
    C(this, R, Jt).call(this), (t = f(this, Ft)) == null || t.resolve();
  }
  /**
   * @param {Effect} effect
   */
  register_created_effect(t) {
    f(this, fn).push(t);
  }
  /**
   * @param {boolean} blocking
   * @param {Effect} effect
   */
  increment(t, n) {
    if (E(this, jt, f(this, jt) + 1), t) {
      let r = f(this, Ue).get(n) ?? 0;
      f(this, Ue).set(n, r + 1);
    }
  }
  /**
   * @param {boolean} blocking
   * @param {Effect} effect
   */
  decrement(t, n) {
    if (E(this, jt, f(this, jt) - 1), t) {
      let r = f(this, Ue).get(n) ?? 0;
      r === 1 ? f(this, Ue).delete(n) : f(this, Ue).set(n, r - 1);
    }
    f(this, mt) || (E(this, mt, !0), Ke(() => {
      E(this, mt, !1), this.linked && this.flush();
    }));
  }
  /**
   * @param {Set<Effect>} dirty_effects
   * @param {Set<Effect>} maybe_dirty_effects
   */
  transfer_effects(t, n) {
    for (const r of t)
      f(this, Xe).add(r);
    for (const r of n)
      f(this, Te).add(r);
    t.clear(), n.clear();
  }
  /** @param {(batch: Batch) => void} fn */
  oncommit(t) {
    f(this, Pt).add(t);
  }
  /** @param {(batch: Batch) => void} fn */
  ondiscard(t) {
    f(this, Lt).add(t);
  }
  settled() {
    return (f(this, Ft) ?? E(this, Ft, ni())).promise;
  }
  static ensure() {
    if ($ === null) {
      const t = $ = new In();
      !Bn && !en && Ke(() => {
        f(t, Dt) || t.flush();
      });
    }
    return $;
  }
  apply() {
    {
      z = null;
      return;
    }
  }
  /**
   *
   * @param {Effect} effect
   */
  schedule(t) {
    var i;
    if (Zn = t, (i = t.b) != null && i.is_pending && (t.f & (Yt | Ln | ri)) !== 0 && (t.f & Tt) === 0) {
      t.b.defer_effect(t);
      return;
    }
    for (var n = t; n.parent !== null; ) {
      n = n.parent;
      var r = n.f;
      if (Ot !== null && n === S && (T === null || (T.f & G) === 0))
        return;
      if ((r & (Ze | Oe)) !== 0) {
        if ((r & B) === 0)
          return;
        n.f ^= B;
      }
    }
    f(this, le).push(n);
  }
};
Dt = new WeakMap(), st = new WeakMap(), gt = new WeakMap(), Pt = new WeakMap(), Lt = new WeakMap(), jt = new WeakMap(), Ue = new WeakMap(), Ft = new WeakMap(), le = new WeakMap(), fn = new WeakMap(), Xe = new WeakMap(), Te = new WeakMap(), De = new WeakMap(), Ht = new WeakMap(), mt = new WeakMap(), R = new WeakSet(), Qn = function() {
  if (this.is_fork) return !0;
  for (const r of f(this, Ue).keys()) {
    for (var t = r, n = !1; t.parent !== null; ) {
      if (f(this, De).has(t)) {
        n = !0;
        break;
      }
      t = t.parent;
    }
    if (!n)
      return !0;
  }
  return !1;
}, Kt = function() {
  var o, c, d, m;
  E(this, Dt, !0), Lr++ > 1e3 && (C(this, R, Jt).call(this), dl());
  for (const u of f(this, Xe))
    f(this, Te).delete(u), H(u, U), this.schedule(u);
  for (const u of f(this, Te))
    H(u, He), this.schedule(u);
  const t = f(this, le);
  E(this, le, []), this.apply();
  var n = Ot = [], r = [], i = wn = [];
  for (const u of t)
    try {
      C(this, R, er).call(this, u, n, r);
    } catch (h) {
      throw Ti(u), C(this, R, Qn).call(this) || this.discard(), h;
    }
  if ($ = null, i.length > 0) {
    var s = In.ensure();
    for (const u of i)
      s.schedule(u);
  }
  if (Ot = null, wn = null, C(this, R, Qn).call(this)) {
    C(this, R, Nt).call(this, r), C(this, R, Nt).call(this, n);
    for (const [u, h] of f(this, De))
      Si(u, h);
    i.length > 0 && /** @type {unknown} */
    C(o = $, R, Kt).call(o);
    return;
  }
  const l = C(this, R, Ei).call(this);
  if (l) {
    C(this, R, Nt).call(this, r), C(this, R, Nt).call(this, n), C(c = l, R, $i).call(c, this);
    return;
  }
  f(this, Xe).clear(), f(this, Te).clear();
  for (const u of f(this, Pt)) u(this);
  f(this, Pt).clear(), Qt = this, jr(r), jr(n), Qt = null, (d = f(this, Ft)) == null || d.resolve();
  var a = (
    /** @type {Batch | null} */
    /** @type {unknown} */
    $
  );
  if (f(this, jt) === 0 && (f(this, le).length === 0 || a !== null) && C(this, R, Jt).call(this), f(this, le).length > 0)
    if (a !== null) {
      const u = a;
      f(u, le).push(...f(this, le).filter((h) => !f(u, le).includes(h)));
    } else
      a = this;
  a !== null && C(m = a, R, Kt).call(m);
}, /**
 * Traverse the effect tree, executing effects or stashing
 * them for later execution as appropriate
 * @param {Effect} root
 * @param {Effect[]} effects
 * @param {Effect[]} render_effects
 */
er = function(t, n, r) {
  t.f ^= B;
  for (var i = t.first; i !== null; ) {
    var s = i.f, l = (s & (Oe | Ze)) !== 0, a = l && (s & B) !== 0, o = a || (s & te) !== 0 || f(this, De).has(i);
    if (!o && i.fn !== null) {
      l ? i.f ^= B : (s & Yt) !== 0 ? n.push(i) : hn(i) && ((s & Ae) !== 0 && f(this, Te).add(i), Ut(i));
      var c = i.first;
      if (c !== null) {
        i = c;
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
}, Ei = function() {
  for (var t = f(this, st); t !== null; ) {
    if (!t.is_fork) {
      for (const [n, [, r]] of this.current)
        if (t.current.has(n) && !r)
          return t;
    }
    t = f(t, st);
  }
  return null;
}, /**
 * @param {Batch} batch
 */
$i = function(t) {
  var r;
  for (const [i, s] of t.current)
    !this.previous.has(i) && t.previous.has(i) && this.previous.set(i, t.previous.get(i)), this.current.set(i, s);
  for (const [i, s] of t.async_deriveds) {
    const l = this.async_deriveds.get(i);
    l && s.promise.then(l.resolve).catch(l.reject);
  }
  t.async_deriveds.clear(), this.transfer_effects(f(t, Xe), f(t, Te));
  const n = (i) => {
    var s = i.reactions;
    if (s !== null)
      for (const o of s) {
        var l = o.f;
        if ((l & G) !== 0)
          n(
            /** @type {Derived} */
            o
          );
        else {
          var a = (
            /** @type {Effect} */
            o
          );
          l & (Rt | Ae) && !this.async_deriveds.has(a) && (f(this, Te).delete(a), H(a, U), this.schedule(a));
        }
      }
  };
  for (const i of this.current.keys())
    n(i);
  this.oncommit(() => t.discard()), C(r = t, R, Jt).call(r), $ = this, C(this, R, Kt).call(this);
}, /**
 * @param {Effect[]} effects
 */
Nt = function(t) {
  for (var n = 0; n < t.length; n += 1)
    vi(t[n], f(this, Xe), f(this, Te));
}, cl = function() {
  var m;
  for (let u = qn; u !== null; u = f(u, gt)) {
    var t = u.id < this.id, n = [];
    for (const [h, [g, _]] of this.current) {
      if (u.current.has(h)) {
        var r = (
          /** @type {[any, boolean]} */
          u.current.get(h)[0]
        );
        if (t && g !== r)
          u.current.set(h, [g, _]);
        else
          continue;
      }
      n.push(h);
    }
    if (t)
      for (const [h, g] of this.async_deriveds) {
        const _ = u.async_deriveds.get(h);
        _ && g.promise.then(_.resolve).catch(_.reject);
      }
    var i = [...u.current.keys()].filter(
      (h) => !/** @type {[any, boolean]} */
      u.current.get(h)[1]
    );
    if (!(!f(u, Dt) || i.length === 0)) {
      var s = i.filter((h) => !this.current.has(h));
      if (s.length === 0)
        t && u.discard();
      else if (n.length > 0) {
        if (t)
          for (const h of f(this, Ht))
            u.unskip_effect(h, (g) => {
              var _;
              (g.f & (Ae | Rt)) !== 0 ? u.schedule(g) : C(_ = u, R, Nt).call(_, [g]);
            });
        u.activate();
        var l = /* @__PURE__ */ new Set(), a = /* @__PURE__ */ new Map();
        for (var o of n)
          ki(o, s, l, a);
        a = /* @__PURE__ */ new Map();
        var c = [...u.current].filter(([h, g]) => {
          const _ = this.current.get(h);
          return _ ? _[0] !== g[0] || _[1] !== g[1] : !0;
        }).map(([h]) => h);
        if (c.length > 0)
          for (const h of f(this, fn))
            (h.f & (he | te | Tn)) === 0 && pr(h, c, a) && ((h.f & (Rt | Ae)) !== 0 ? (H(h, U), u.schedule(h)) : f(u, Xe).add(h));
        if (f(u, le).length > 0 && !f(u, mt)) {
          u.apply();
          for (var d of f(u, le))
            C(m = u, R, er).call(m, d, [], []);
          E(u, le, []);
        }
        u.deactivate();
      }
    }
  }
}, Jt = function() {
  if (this.linked) {
    var t = f(this, st), n = f(this, gt);
    t === null ? qn = n : E(t, gt, n), n === null ? At = t : E(n, st, t), this.linked = !1;
  }
};
let ot = In;
function N(e) {
  var t = en;
  en = !0;
  try {
    for (var n; ; ) {
      if (Zs(), $ === null)
        return (
          /** @type {T} */
          n
        );
      $.flush();
    }
  } finally {
    en = t;
  }
}
function dl() {
  try {
    Fs();
  } catch (e) {
    lt(e, Zn);
  }
}
let Se = null;
function jr(e) {
  var t = e.length;
  if (t !== 0) {
    for (var n = 0; n < t; ) {
      var r = e[n++];
      if ((r.f & (he | te)) === 0 && hn(r) && (Se = /* @__PURE__ */ new Set(), Ut(r), r.deps === null && r.first === null && r.nodes === null && r.teardown === null && r.ac === null && Hi(r), (Se == null ? void 0 : Se.size) > 0)) {
        xt.clear();
        for (const i of Se) {
          if ((i.f & (he | te)) !== 0) continue;
          const s = [i];
          let l = i.parent;
          for (; l !== null; )
            Se.has(l) && (Se.delete(l), s.push(l)), l = l.parent;
          for (let a = s.length - 1; a >= 0; a--) {
            const o = s[a];
            (o.f & (he | te)) === 0 && Ut(o);
          }
        }
        Se.clear();
      }
    }
    Se = null;
  }
}
function ki(e, t, n, r) {
  if (!n.has(e) && (n.add(e), e.reactions !== null))
    for (const i of e.reactions) {
      const s = i.f;
      (s & G) !== 0 ? ki(
        /** @type {Derived} */
        i,
        t,
        n,
        r
      ) : (s & (Rt | Ae)) !== 0 && (s & U) === 0 && pr(i, t, r) && (H(i, U), _r(
        /** @type {Effect} */
        i
      ));
    }
}
function pr(e, t, n) {
  const r = n.get(e);
  if (r !== void 0) return r;
  if (e.deps !== null)
    for (const i of e.deps) {
      if ($n.call(t, i))
        return !0;
      if ((i.f & G) !== 0 && pr(
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
function _r(e) {
  $.schedule(e);
}
function Si(e, t) {
  if (!((e.f & Oe) !== 0 && (e.f & B) !== 0)) {
    (e.f & U) !== 0 ? t.d.push(e) : (e.f & He) !== 0 && t.m.push(e), H(e, B);
    for (var n = e.first; n !== null; )
      Si(n, t), n = n.next;
  }
}
function Ti(e) {
  H(e, B);
  for (var t = e.first; t !== null; )
    Ti(t), t = t.next;
}
let Nn = /* @__PURE__ */ new Set();
const xt = /* @__PURE__ */ new Map();
let Ci = !1;
function St(e, t) {
  var n = {
    f: 0,
    // TODO ideally we could skip this altogether, but it causes type errors
    v: e,
    reactions: null,
    equals: oi,
    rv: 0,
    wv: 0
  };
  return n;
}
// @__NO_SIDE_EFFECTS__
function Ie(e, t) {
  const n = St(e);
  return qi(n), n;
}
// @__NO_SIDE_EFFECTS__
function Mi(e, t = !1, n = !0) {
  const r = St(e);
  return t || (r.equals = fi), r;
}
function Me(e, t, n = !1) {
  T !== null && // since we are untracking the function inside `$inspect.with` we need to add this check
  // to ensure we error if state is set inside an inspect effect
  (!Ne || (T.f & Tn) !== 0) && ui() && (T.f & (G | Ae | Rt | Tn)) !== 0 && (Fe === null || !Fe.has(e)) && Bs();
  let r = n ? dt(t) : t;
  return Vt(e, r, wn);
}
function Vt(e, t, n = null) {
  if (!e.equals(t)) {
    xt.set(e, Qe ? t : e.v);
    var r = ot.ensure();
    if (r.capture(e, t), (e.f & G) !== 0) {
      const i = (
        /** @type {Derived} */
        e
      );
      (e.f & U) !== 0 && vr(i), z === null && hr(i);
    }
    e.wv = Vi(), Ai(e, U, n), S !== null && (S.f & B) !== 0 && (S.f & (Oe | Ze)) === 0 && (_e === null ? El([e]) : _e.push(e)), !r.is_fork && Nn.size > 0 && !Ci && hl();
  }
  return t;
}
function hl() {
  Ci = !1;
  for (const e of Nn) {
    (e.f & B) !== 0 && H(e, He);
    let t;
    try {
      t = hn(e);
    } catch {
      t = !0;
    }
    t && Ut(e);
  }
  Nn.clear();
}
function tn(e) {
  Me(e, e.v + 1);
}
function Ai(e, t, n) {
  var r = e.reactions;
  if (r !== null)
    for (var i = r.length, s = 0; s < i; s++) {
      var l = r[s], a = l.f, o = (a & U) === 0;
      if (o && H(l, t), (a & Tn) !== 0)
        Nn.add(
          /** @type {Effect} */
          l
        );
      else if ((a & G) !== 0) {
        var c = (
          /** @type {Derived} */
          l
        );
        z == null || z.delete(c), (a & kt) === 0 && (a & we && (S === null || (S.f & Cn) === 0) && (l.f |= kt), Ai(c, He, n));
      } else if (o) {
        var d = (
          /** @type {Effect} */
          l
        );
        (a & Ae) !== 0 && Se !== null && Se.add(d), n !== null ? n.push(d) : _r(d);
      }
    }
}
function dt(e) {
  if (typeof e != "object" || e === null || wt in e)
    return e;
  const t = ti(e);
  if (t !== ks && t !== Ss)
    return e;
  var n = /* @__PURE__ */ new Map(), r = cr(e), i = /* @__PURE__ */ Ie(0), s = $t, l = (a) => {
    if ($t === s)
      return a();
    var o = T, c = $t;
    Ee(null), qr(s);
    var d = a();
    return Ee(o), qr(c), d;
  };
  return r && n.set("length", /* @__PURE__ */ Ie(
    /** @type {any[]} */
    e.length
  )), new Proxy(
    /** @type {any} */
    e,
    {
      defineProperty(a, o, c) {
        (!("value" in c) || c.configurable === !1 || c.enumerable === !1 || c.writable === !1) && Ys();
        var d = n.get(o);
        return d === void 0 ? l(() => {
          var m = /* @__PURE__ */ Ie(c.value);
          return n.set(o, m), m;
        }) : Me(d, c.value, !0), !0;
      },
      deleteProperty(a, o) {
        var c = n.get(o);
        if (c === void 0) {
          if (o in a) {
            const d = l(() => /* @__PURE__ */ Ie(q));
            n.set(o, d), tn(i);
          }
        } else
          Me(c, q), tn(i);
        return !0;
      },
      get(a, o, c) {
        var h;
        if (o === wt)
          return e;
        var d = n.get(o), m = o in a;
        if (d === void 0 && (!m || (h = yt(a, o)) != null && h.writable) && (d = l(() => {
          var g = dt(m ? a[o] : q), _ = /* @__PURE__ */ Ie(g);
          return _;
        }), n.set(o, d)), d !== void 0) {
          var u = M(d);
          return u === q ? void 0 : u;
        }
        return Reflect.get(a, o, c);
      },
      getOwnPropertyDescriptor(a, o) {
        var c = Reflect.getOwnPropertyDescriptor(a, o);
        if (c && "value" in c) {
          var d = n.get(o);
          d && (c.value = M(d));
        } else if (c === void 0) {
          var m = n.get(o), u = m == null ? void 0 : m.v;
          if (m !== void 0 && u !== q)
            return {
              enumerable: !0,
              configurable: !0,
              value: u,
              writable: !0
            };
        }
        return c;
      },
      has(a, o) {
        var u;
        if (o === wt)
          return !0;
        var c = n.get(o), d = c !== void 0 && c.v !== q || Reflect.has(a, o);
        if (c !== void 0 || S !== null && (!d || (u = yt(a, o)) != null && u.writable)) {
          c === void 0 && (c = l(() => {
            var h = d ? dt(a[o]) : q, g = /* @__PURE__ */ Ie(h);
            return g;
          }), n.set(o, c));
          var m = M(c);
          if (m === q)
            return !1;
        }
        return d;
      },
      set(a, o, c, d) {
        var v;
        var m = n.get(o), u = o in a;
        if (r && o === "length")
          for (var h = c; h < /** @type {Source<number>} */
          m.v; h += 1) {
            var g = n.get(h + "");
            g !== void 0 ? Me(g, q) : h in a && (g = l(() => /* @__PURE__ */ Ie(q)), n.set(h + "", g));
          }
        if (m === void 0)
          (!u || (v = yt(a, o)) != null && v.writable) && (m = l(() => /* @__PURE__ */ Ie(void 0)), Me(m, dt(c)), n.set(o, m));
        else {
          u = m.v !== q;
          var _ = l(() => dt(c));
          Me(m, _);
        }
        var p = Reflect.getOwnPropertyDescriptor(a, o);
        if (p != null && p.set && p.set.call(d, c), !u) {
          if (r && typeof o == "string") {
            var b = (
              /** @type {Source<number>} */
              n.get("length")
            ), y = Number(o);
            Number.isInteger(y) && y >= b.v && Me(b, y + 1);
          }
          tn(i);
        }
        return !0;
      },
      ownKeys(a) {
        M(i);
        var o = Reflect.ownKeys(a).filter((m) => {
          var u = n.get(m);
          return u === void 0 || u.v !== q;
        });
        for (var [c, d] of n)
          d.v !== q && !(c in a) && o.push(c);
        return o;
      },
      setPrototypeOf() {
        qs();
      }
    }
  );
}
function Fr(e) {
  try {
    if (e !== null && typeof e == "object" && wt in e)
      return e[wt];
  } catch {
  }
  return e;
}
function vl(e, t) {
  return Object.is(Fr(e), Fr(t));
}
var Hr, Ni, Oi, Ri;
function tr() {
  if (Hr === void 0) {
    Hr = window, Ni = /Firefox/.test(navigator.userAgent);
    var e = Element.prototype, t = Node.prototype, n = Text.prototype;
    Oi = yt(t, "firstChild").get, Ri = yt(t, "nextSibling").get, Pr(e) && (e[Os] = void 0, e[mn] = null, e[Rs] = void 0, e.__e = void 0), Pr(n) && (n[zt] = void 0);
  }
}
function je(e = "") {
  return document.createTextNode(e);
}
// @__NO_SIDE_EFFECTS__
function rn(e) {
  return (
    /** @type {TemplateNode | null} */
    Oi.call(e)
  );
}
// @__NO_SIDE_EFFECTS__
function nt(e) {
  return (
    /** @type {TemplateNode | null} */
    Ri.call(e)
  );
}
function Y(e, t) {
  if (!D)
    return /* @__PURE__ */ rn(e);
  var n = /* @__PURE__ */ rn(P);
  if (n === null)
    n = P.appendChild(je());
  else if (t && n.nodeType !== dr) {
    var r = je();
    return n == null || n.before(r), ve(r), r;
  }
  return t && Pi(
    /** @type {Text} */
    n
  ), ve(n), n;
}
function xe(e, t = 1, n = !1) {
  let r = D ? P : e;
  for (var i; t--; )
    i = r, r = /** @type {TemplateNode} */
    /* @__PURE__ */ nt(r);
  if (!D)
    return r;
  if (n) {
    if ((r == null ? void 0 : r.nodeType) !== dr) {
      var s = je();
      return r === null ? i == null || i.after(s) : r.before(s), ve(s), s;
    }
    Pi(
      /** @type {Text} */
      r
    );
  }
  return ve(r), r;
}
function Ii(e) {
  e.textContent = "";
}
function Di() {
  return !1;
}
function gr(e, t, n) {
  return (
    /** @type {T extends keyof HTMLElementTagNameMap ? HTMLElementTagNameMap[T] : Element} */
    n ? document.createElement(e, { is: n }) : document.createElement(e)
  );
}
function Pi(e) {
  if (
    /** @type {string} */
    e.nodeValue.length < 65536
  )
    return;
  let t = e.nextSibling;
  for (; t !== null && t.nodeType === dr; )
    t.remove(), e.nodeValue += /** @type {string} */
    t.nodeValue, t = e.nextSibling;
}
let Wr = !1;
function pl() {
  Wr || (Wr = !0, document.addEventListener(
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
            (t = n[si]) == null || t.call(n);
      });
    },
    // In the capture phase to guarantee we get noticed of it (no possibility of stopPropagation)
    { capture: !0 }
  ));
}
function mr(e) {
  var t = T, n = S;
  Ee(null), We(null);
  try {
    return e();
  } finally {
    Ee(t), We(n);
  }
}
function _l(e) {
  S === null && (T === null && js(), Ls()), Qe && Ps();
}
function gl(e, t) {
  var n = t.last;
  n === null ? t.last = t.first = e : (n.next = e, e.prev = n, t.last = e);
}
function Ye(e, t) {
  var n = S;
  n !== null && (n.f & te) !== 0 && (e |= te);
  var r = {
    ctx: ne,
    deps: null,
    nodes: null,
    f: e | U | we,
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
  $ == null || $.register_created_effect(r);
  var i = r;
  if ((e & Yt) !== 0)
    Ot !== null ? Ot.push(r) : ot.ensure().schedule(r);
  else if (t !== null) {
    try {
      Ut(r);
    } catch (l) {
      throw re(r), l;
    }
    i.deps === null && i.teardown === null && i.nodes === null && i.first === i.last && // either `null`, or a singular child
    (i.f & Ct) === 0 && (i = i.first, (e & Ae) !== 0 && (e & qt) !== 0 && i !== null && (i.f |= qt));
  }
  if (i !== null && (i.parent = n, n !== null && gl(i, n), T !== null && (T.f & G) !== 0 && (e & Ze) === 0)) {
    var s = (
      /** @type {Derived} */
      T
    );
    (s.effects ?? (s.effects = [])).push(i);
  }
  return r;
}
function br() {
  return T !== null && !Ne;
}
function yr(e) {
  const t = Ye(Ln, null);
  return H(t, B), t.teardown = e, t;
}
function wr(e) {
  _l();
  var t = (
    /** @type {Effect} */
    S.f
  ), n = !T && (t & Oe) !== 0 && ne !== null && !ne.i;
  if (n) {
    var r = (
      /** @type {ComponentContext} */
      ne
    );
    (r.e ?? (r.e = [])).push(e);
  } else
    return Li(e);
}
function Li(e) {
  return Ye(Yt | As, e);
}
function ml(e) {
  ot.ensure();
  const t = Ye(Ze | Ct, e);
  return () => {
    re(t);
  };
}
function bl(e) {
  ot.ensure();
  const t = Ye(Ze | Ct, e);
  return (n = {}) => new Promise((r) => {
    n.outro ? Et(t, () => {
      re(t), r(void 0);
    }) : (re(t), r(void 0));
  });
}
function ji(e) {
  return Ye(Yt, e);
}
function yl(e) {
  return Ye(Rt | Ct, e);
}
function xr(e, t = 0) {
  return Ye(Ln | t, e);
}
function pe(e, t = [], n = [], r = []) {
  sl(r, t, n, (i) => {
    Ye(Ln, () => {
      e(...i.map(M));
    });
  });
}
function Er(e, t = 0) {
  var n = Ye(Ae | t, e);
  return n;
}
function ye(e) {
  return Ye(Oe | Ct, e);
}
function Fi(e) {
  var t = e.teardown;
  if (t !== null) {
    const n = Qe, r = T;
    Yr(!0), Ee(null);
    try {
      t.call(null);
    } finally {
      Yr(n), Ee(r);
    }
  }
}
function $r(e, t = !1) {
  var n = e.first;
  for (e.first = e.last = null; n !== null; ) {
    const i = n.ac;
    i !== null && mr(() => {
      i.abort(jn);
    });
    var r = n.next;
    (n.f & Ze) !== 0 ? n.parent = null : re(n, t), n = r;
  }
}
function wl(e) {
  for (var t = e.first; t !== null; ) {
    var n = t.next;
    (t.f & Oe) === 0 && re(t), t = n;
  }
}
function re(e, t = !0) {
  var n = !1;
  (t || (e.f & Ms) !== 0) && e.nodes !== null && e.nodes.end !== null && (xl(
    e.nodes.start,
    /** @type {TemplateNode} */
    e.nodes.end
  ), n = !0), e.f |= zn, $r(e, t && !n), sn(e, 0);
  var r = e.nodes && e.nodes.t;
  if (r !== null)
    for (const s of r)
      s.stop();
  Fi(e), e.f ^= zn, e.f |= he;
  var i = e.parent;
  i !== null && i.first !== null && Hi(e), e.next = e.prev = e.teardown = e.ctx = e.deps = e.fn = e.nodes = e.ac = e.b = null;
}
function xl(e, t) {
  for (; e !== null; ) {
    var n = e === t ? null : /* @__PURE__ */ nt(e);
    e.remove(), e = n;
  }
}
function Hi(e) {
  var t = e.parent, n = e.prev, r = e.next;
  n !== null && (n.next = r), r !== null && (r.prev = n), t !== null && (t.first === e && (t.first = r), t.last === e && (t.last = n));
}
function Et(e, t, n = !0) {
  var r = [];
  Wi(e, r, !0);
  var i = () => {
    n && re(e), t && t();
  }, s = r.length;
  if (s > 0) {
    var l = () => --s || i();
    for (var a of r)
      a.out(l);
  } else
    i();
}
function Wi(e, t, n) {
  if ((e.f & te) === 0) {
    e.f ^= te;
    var r = e.nodes && e.nodes.t;
    if (r !== null)
      for (const a of r)
        (a.is_global || n) && t.push(a);
    for (var i = e.first; i !== null; ) {
      var s = i.next;
      if ((i.f & Ze) === 0) {
        var l = (i.f & qt) !== 0 || // If this is a branch effect without a block effect parent,
        // it means the parent block effect was pruned. In that case,
        // transparency information was transferred to the branch effect.
        (i.f & Oe) !== 0 && (e.f & Ae) !== 0;
        Wi(i, t, l ? n : !1);
      }
      i = s;
    }
  }
}
function On(e) {
  Yi(e, !0);
}
function Yi(e, t) {
  if ((e.f & te) !== 0) {
    e.f ^= te, (e.f & B) === 0 && (H(e, U), ot.ensure().schedule(e));
    for (var n = e.first; n !== null; ) {
      var r = n.next, i = (n.f & qt) !== 0 || (n.f & Oe) !== 0;
      Yi(n, i ? t : !1), n = r;
    }
    var s = e.nodes && e.nodes.t;
    if (s !== null)
      for (const l of s)
        (l.is_global || t) && l.in();
  }
}
function kr(e, t) {
  if (e.nodes)
    for (var n = e.nodes.start, r = e.nodes.end; n !== null; ) {
      var i = n === r ? null : /* @__PURE__ */ nt(n);
      t.append(n), n = i;
    }
}
let xn = !1, Qe = !1;
function Yr(e) {
  Qe = e;
}
let T = null, Ne = !1;
function Ee(e) {
  T = e;
}
let S = null;
function We(e) {
  S = e;
}
let Fe = null;
function qi(e) {
  T !== null && (Fe ?? (Fe = /* @__PURE__ */ new Set())).add(e);
}
let ae = null, fe = 0, _e = null;
function El(e) {
  _e = e;
}
let Bi = 1, ht = 0, $t = ht;
function qr(e) {
  $t = e;
}
function Vi() {
  return ++Bi;
}
function hn(e) {
  var t = e.f;
  if ((t & U) !== 0)
    return !0;
  if (t & G && (e.f &= ~kt), (t & He) !== 0) {
    for (var n = (
      /** @type {Value[]} */
      e.deps
    ), r = n.length, i = 0; i < r; i++) {
      var s = n[i];
      if (hn(
        /** @type {Derived} */
        s
      ) && wi(
        /** @type {Derived} */
        s
      ), s.wv > e.wv)
        return !0;
    }
    (t & we) !== 0 && // During time traveling we don't want to reset the status so that
    // traversal of the graph in the other batches still happens
    z === null && H(e, B);
  }
  return !1;
}
function Ui(e, t, n = !0) {
  var r = e.reactions;
  if (r !== null && !(Fe !== null && Fe.has(e)))
    for (var i = 0; i < r.length; i++) {
      var s = r[i];
      (s.f & G) !== 0 ? Ui(
        /** @type {Derived} */
        s,
        t,
        !1
      ) : t === s && (n ? H(s, U) : (s.f & B) !== 0 && H(s, He), _r(
        /** @type {Effect} */
        s
      ));
    }
}
function Xi(e) {
  var _;
  var t = ae, n = fe, r = _e, i = T, s = Fe, l = ne, a = Ne, o = $t, c = e.f;
  ae = /** @type {null | Value[]} */
  null, fe = 0, _e = null, T = (c & (Oe | Ze)) === 0 ? e : null, Fe = null, Bt(e.ctx), Ne = !1, $t = ++ht, e.ac !== null && (mr(() => {
    e.ac.abort(jn);
  }), e.ac = null);
  try {
    e.f |= Cn;
    var d = (
      /** @type {Function} */
      e.fn
    ), m = d();
    e.f |= Tt;
    var u = e.deps, h = $ == null ? void 0 : $.is_fork;
    if (ae !== null) {
      var g;
      if (h || sn(e, fe), u !== null && fe > 0)
        for (u.length = fe + ae.length, g = 0; g < ae.length; g++)
          u[fe + g] = ae[g];
      else
        e.deps = u = ae;
      if (br() && (e.f & we) !== 0)
        for (g = fe; g < u.length; g++)
          ((_ = u[g]).reactions ?? (_.reactions = [])).push(e);
    } else !h && u !== null && fe < u.length && (sn(e, fe), u.length = fe);
    if (ui() && _e !== null && !Ne && u !== null && (e.f & (G | He | U)) === 0)
      for (g = 0; g < /** @type {Source[]} */
      _e.length; g++)
        Ui(
          _e[g],
          /** @type {Effect} */
          e
        );
    if (i !== null && i !== e) {
      if (ht++, i.deps !== null)
        for (let p = 0; p < n; p += 1)
          i.deps[p].rv = ht;
      if (t !== null)
        for (const p of t)
          p.rv = ht;
      _e !== null && (r === null ? r = _e : r.push(.../** @type {Source[]} */
      _e));
    }
    return (e.f & at) !== 0 && (e.f ^= at), m;
  } catch (p) {
    return di(p);
  } finally {
    e.f ^= Cn, ae = t, fe = n, _e = r, T = i, Fe = s, Bt(l), Ne = a, $t = o;
  }
}
function $l(e, t) {
  let n = t.reactions;
  if (n !== null) {
    var r = Es.call(n, e);
    if (r !== -1) {
      var i = n.length - 1;
      i === 0 ? n = t.reactions = null : (n[r] = n[i], n.pop());
    }
  }
  if (n === null && (t.f & G) !== 0 && // Destroying a child effect while updating a parent effect can cause a dependency to appear
  // to be unused, when in fact it is used by the currently-updating parent. Checking `new_deps`
  // allows us to skip the expensive work of disconnecting and immediately reconnecting it
  (ae === null || !$n.call(ae, t))) {
    var s = (
      /** @type {Derived} */
      t
    );
    (s.f & we) !== 0 && (s.f ^= we, s.f &= ~kt), s.v !== q && hr(s), fl(s), sn(s, 0);
  }
}
function sn(e, t) {
  var n = e.deps;
  if (n !== null)
    for (var r = t; r < n.length; r++)
      $l(e, n[r]);
}
function Ut(e) {
  var t = e.f;
  if ((t & he) === 0) {
    H(e, B);
    var n = S, r = xn;
    S = e, xn = !0;
    try {
      (t & (Ae | ri)) !== 0 ? wl(e) : $r(e), Fi(e);
      var i = Xi(e);
      e.teardown = typeof i == "function" ? i : null, e.wv = Bi;
      var s;
      ei && Js && (e.f & U) !== 0 && e.deps;
    } finally {
      xn = r, S = n;
    }
  }
}
function M(e) {
  var t = e.f, n = (t & G) !== 0;
  if (T !== null && !Ne) {
    var r = S !== null && (S.f & he) !== 0;
    if (!r && (Fe === null || !Fe.has(e))) {
      var i = T.deps;
      if ((T.f & Cn) !== 0)
        e.rv < ht && (e.rv = ht, ae === null && i !== null && i[fe] === e ? fe++ : ae === null ? ae = [e] : ae.push(e));
      else {
        T.deps ?? (T.deps = []), $n.call(T.deps, e) || T.deps.push(e);
        var s = e.reactions;
        s === null ? e.reactions = [T] : $n.call(s, T) || s.push(T);
      }
    }
  }
  if (Qe && xt.has(e))
    return xt.get(e);
  if (n) {
    var l = (
      /** @type {Derived} */
      e
    );
    if (Qe) {
      var a = l.v;
      return ((l.f & B) === 0 && l.reactions !== null || Gi(l)) && (a = vr(l)), xt.set(l, a), a;
    }
    var o = (l.f & we) === 0 && !Ne && T !== null && (xn || (T.f & we) !== 0), c = (l.f & Tt) === 0;
    hn(l) && (o && (l.f |= we), wi(l)), o && !c && (xi(l), zi(l));
  }
  if (z != null && z.has(e))
    return z.get(e);
  if ((e.f & at) !== 0)
    throw e.v;
  return e.v;
}
function zi(e) {
  if (e.f |= we, e.deps !== null)
    for (const t of e.deps)
      (t.reactions ?? (t.reactions = [])).push(e), (t.f & G) !== 0 && (t.f & we) === 0 && (xi(
        /** @type {Derived} */
        t
      ), zi(
        /** @type {Derived} */
        t
      ));
}
function Gi(e) {
  if (e.v === q) return !0;
  if (e.deps === null) return !1;
  for (const t of e.deps)
    if (xt.has(t) || (t.f & G) !== 0 && Gi(
      /** @type {Derived} */
      t
    ))
      return !0;
  return !1;
}
function Sr(e) {
  var t = Ne;
  try {
    return Ne = !0, e();
  } finally {
    Ne = t;
  }
}
const vt = Symbol("events"), Ki = /* @__PURE__ */ new Set(), nr = /* @__PURE__ */ new Set();
function kl(e, t, n, r = {}) {
  function i(s) {
    if (r.capture || rr.call(t, s), !s.cancelBubble)
      return mr(() => n == null ? void 0 : n.call(this, s));
  }
  return Ke(() => {
    t.addEventListener(e, i, r);
  }), i;
}
function Ji(e, t, n, r, i) {
  var s = { capture: r, passive: i }, l = kl(e, t, n, s);
  (t === document.body || // @ts-ignore
  t === window || // @ts-ignore
  t === document || // Firefox has quirky behavior, it can happen that we still get "canplay" events when the element is already removed
  t instanceof HTMLMediaElement) && yr(() => {
    t.removeEventListener(e, l, s);
  });
}
function J(e, t, n) {
  (t[vt] ?? (t[vt] = {}))[e] = n;
}
function Mt(e) {
  for (var t = 0; t < e.length; t++)
    Ki.add(e[t]);
  for (var n of nr)
    n(e);
}
let Br = null;
function rr(e) {
  var _, p;
  var t = this, n = (
    /** @type {Node} */
    t.ownerDocument
  ), r = e.type, i = ((_ = e.composedPath) == null ? void 0 : _.call(e)) || [], s = (
    /** @type {null | Element} */
    i[0] || e.target
  );
  Br = e;
  var l = 0, a = Br === e && e[vt];
  if (a) {
    var o = i.indexOf(a);
    if (o !== -1 && (t === document || t === /** @type {any} */
    window)) {
      e[vt] = t;
      return;
    }
    var c = i.indexOf(t);
    if (c === -1)
      return;
    o <= c && (l = o);
  }
  if (s = /** @type {Element} */
  i[l] || e.target, s !== t) {
    Sn(e, "currentTarget", {
      configurable: !0,
      get() {
        return s || n;
      }
    });
    var d = T, m = S;
    Ee(null), We(null);
    try {
      for (var u, h = []; s !== null && s !== t; ) {
        try {
          var g = (p = s[vt]) == null ? void 0 : p[r];
          g != null && (!/** @type {any} */
          s.disabled || // DOM could've been updated already by the time this is reached, so we check this as well
          // -> the target could not have been disabled because it emits the event in the first place
          e.target === s) && g.call(s, e);
        } catch (b) {
          u ? h.push(b) : u = b;
        }
        if (e.cancelBubble) break;
        l++, s = l < i.length ? (
          /** @type {Element} */
          i[l]
        ) : null;
      }
      if (u) {
        for (let b of h)
          queueMicrotask(() => {
            throw b;
          });
        throw u;
      }
    } finally {
      e[vt] = t, delete e.currentTarget, Ee(d), We(m);
    }
  }
}
var Jr;
const Vn = (
  // We gotta write it like this because after downleveling the pure comment may end up in the wrong location
  ((Jr = globalThis == null ? void 0 : globalThis.window) == null ? void 0 : Jr.trustedTypes) && /* @__PURE__ */ globalThis.window.trustedTypes.createPolicy("svelte-trusted-html", {
    /** @param {string} html */
    createHTML: (e) => e
  })
);
function Sl(e) {
  return (
    /** @type {string} */
    (Vn == null ? void 0 : Vn.createHTML(e)) ?? e
  );
}
function Tl(e) {
  var t = gr("template");
  return t.innerHTML = Sl(e.replaceAll("<!>", "<!---->")), t.content;
}
function ir(e, t) {
  var n = (
    /** @type {Effect} */
    S
  );
  n.nodes === null && (n.nodes = { start: e, end: t, a: null, t: null });
}
// @__NO_SIDE_EFFECTS__
function ie(e, t) {
  var n = (t & ws) !== 0, r, i = !e.startsWith("<!>");
  return () => {
    if (D)
      return ir(P, null), P;
    r === void 0 && (r = Tl(i ? e : "<!>" + e), r = /** @type {TemplateNode} */
    /* @__PURE__ */ rn(r));
    var s = (
      /** @type {TemplateNode} */
      n || Ni ? document.importNode(r, !0) : r.cloneNode(!0)
    );
    return ir(s, s), s;
  };
}
function Z(e, t) {
  if (D) {
    var n = (
      /** @type {Effect & { nodes: EffectNodes }} */
      S
    );
    ((n.f & Tt) === 0 || n.nodes.end === null) && (n.nodes.end = P), Hn();
    return;
  }
  e !== null && e.before(
    /** @type {Node} */
    t
  );
}
const Cl = ["touchstart", "touchmove"];
function Ml(e) {
  return Cl.includes(e);
}
function Re(e, t) {
  var n = t == null ? "" : typeof t == "object" ? `${t}` : t;
  n !== /** @type {any} */
  (e[zt] ?? (e[zt] = e.nodeValue)) && (e[zt] = n, e.nodeValue = `${n}`);
}
function Zi(e, t) {
  return Qi(e, t);
}
function Al(e, t) {
  tr(), t.intro = t.intro ?? !1;
  const n = t.target, r = D, i = P;
  try {
    for (var s = /* @__PURE__ */ rn(n); s && (s.nodeType !== dn || /** @type {Comment} */
    s.data !== Qr); )
      s = /* @__PURE__ */ nt(s);
    if (!s)
      throw Wt;
    Ge(!0), ve(
      /** @type {Comment} */
      s
    );
    const l = Qi(e, { ...t, anchor: s });
    return Ge(!1), /**  @type {Exports} */
    l;
  } catch (l) {
    if (l instanceof Error && l.message.split(`
`).some((a) => a.startsWith("https://svelte.dev/e/")))
      throw l;
    return l !== Wt && console.warn("Failed to hydrate: ", l), t.recover === !1 && Hs(), tr(), Ii(n), Ge(!1), Zi(e, t);
  } finally {
    Ge(r), ve(i);
  }
}
const gn = /* @__PURE__ */ new Map();
function Qi(e, { target: t, anchor: n, props: r = {}, events: i, context: s, intro: l = !0, transformError: a }) {
  tr();
  var o = void 0, c = bl(() => {
    var d = n ?? t.appendChild(je());
    rl(
      /** @type {TemplateNode} */
      d,
      {
        pending: () => {
        }
      },
      (h) => {
        et({});
        var g = (
          /** @type {ComponentContext} */
          ne
        );
        if (s && (g.c = s), i && (r.$$events = i), D && ir(
          /** @type {TemplateNode} */
          h,
          null
        ), o = e(h, r) || {}, D && (S.nodes.end = P, P === null || P.nodeType !== dn || /** @type {Comment} */
        P.data !== ur))
          throw Fn(), Wt;
        tt();
      },
      a
    );
    var m = /* @__PURE__ */ new Set(), u = (h) => {
      for (var g = 0; g < h.length; g++) {
        var _ = h[g];
        if (!m.has(_)) {
          m.add(_);
          var p = Ml(_);
          for (const v of [t, document]) {
            var b = gn.get(v);
            b === void 0 && (b = /* @__PURE__ */ new Map(), gn.set(v, b));
            var y = b.get(_);
            y === void 0 ? (v.addEventListener(_, rr, { passive: p }), b.set(_, 1)) : b.set(_, y + 1);
          }
        }
      }
    };
    return u(Pn(Ki)), nr.add(u), () => {
      var p;
      for (var h of m)
        for (const b of [t, document]) {
          var g = (
            /** @type {Map<string, number>} */
            gn.get(b)
          ), _ = (
            /** @type {number} */
            g.get(h)
          );
          --_ == 0 ? (b.removeEventListener(h, rr), g.delete(h), g.size === 0 && gn.delete(b)) : g.set(h, _);
        }
      nr.delete(u), d !== n && ((p = d.parentNode) == null || p.removeChild(d));
    };
  });
  return sr.set(o, c), o;
}
let sr = /* @__PURE__ */ new WeakMap();
function Nl(e, t) {
  const n = sr.get(e);
  return n ? (sr.delete(e), n(t)) : Promise.resolve();
}
var Ce, Pe, de, bt, un, cn, Dn;
class Ol {
  /**
   * @param {TemplateNode} anchor
   * @param {boolean} transition
   */
  constructor(t, n = !0) {
    /** @type {TemplateNode} */
    j(this, "anchor");
    /** @type {Map<Batch, Key>} */
    k(this, Ce, /* @__PURE__ */ new Map());
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
    k(this, Pe, /* @__PURE__ */ new Map());
    /**
     * Similar to #onscreen with respect to the keys, but contains branches that are not yet
     * in the DOM, because their insertion is deferred.
     * @type {Map<Key, Branch>}
     */
    k(this, de, /* @__PURE__ */ new Map());
    /**
     * Keys of effects that are currently outroing
     * @type {Set<Key>}
     */
    k(this, bt, /* @__PURE__ */ new Set());
    /**
     * Whether to pause (i.e. outro) on change, or destroy immediately.
     * This is necessary for `<svelte:element>`
     */
    k(this, un, !0);
    /**
     * @param {Batch} batch
     */
    k(this, cn, (t) => {
      if (f(this, Ce).has(t)) {
        var n = (
          /** @type {Key} */
          f(this, Ce).get(t)
        ), r = f(this, Pe).get(n);
        if (r)
          On(r), f(this, bt).delete(n);
        else {
          var i = f(this, de).get(n);
          i && (On(i.effect), f(this, Pe).set(n, i.effect), f(this, de).delete(n), i.fragment.lastChild.remove(), this.anchor.before(i.fragment), r = i.effect);
        }
        for (const [s, l] of f(this, Ce)) {
          if (f(this, Ce).delete(s), s === t)
            break;
          const a = f(this, de).get(l);
          a && (re(a.effect), f(this, de).delete(l));
        }
        for (const [s, l] of f(this, Pe)) {
          if (s === n || f(this, bt).has(s)) continue;
          const a = () => {
            if (Array.from(f(this, Ce).values()).includes(s)) {
              var c = document.createDocumentFragment();
              kr(l, c), c.append(je()), f(this, de).set(s, { effect: l, fragment: c });
            } else
              re(l);
            f(this, bt).delete(s), f(this, Pe).delete(s);
          };
          f(this, un) || !r ? (f(this, bt).add(s), Et(l, a, !1)) : a();
        }
      }
    });
    /**
     * @param {Batch} batch
     */
    k(this, Dn, (t) => {
      f(this, Ce).delete(t);
      const n = Array.from(f(this, Ce).values());
      for (const [r, i] of f(this, de))
        n.includes(r) || (re(i.effect), f(this, de).delete(r));
    });
    this.anchor = t, E(this, un, n);
  }
  /**
   *
   * @param {any} key
   * @param {null | ((target: TemplateNode) => void)} fn
   */
  ensure(t, n) {
    var r = (
      /** @type {Batch} */
      $
    ), i = Di();
    if (n && !f(this, Pe).has(t) && !f(this, de).has(t))
      if (i) {
        var s = document.createDocumentFragment(), l = je();
        s.append(l), f(this, de).set(t, {
          effect: ye(() => n(l)),
          fragment: s
        });
      } else
        f(this, Pe).set(
          t,
          ye(() => n(this.anchor))
        );
    if (f(this, Ce).set(r, t), i) {
      for (const [a, o] of f(this, Pe))
        a === t ? r.unskip_effect(o) : r.skip_effect(o);
      for (const [a, o] of f(this, de))
        a === t ? r.unskip_effect(o.effect) : r.skip_effect(o.effect);
      r.oncommit(f(this, cn)), r.ondiscard(f(this, Dn));
    } else
      D && (this.anchor = P), f(this, cn).call(this, r);
  }
}
Ce = new WeakMap(), Pe = new WeakMap(), de = new WeakMap(), bt = new WeakMap(), un = new WeakMap(), cn = new WeakMap(), Dn = new WeakMap();
function vn(e, t, n = !1) {
  var r;
  D && (r = P, Hn());
  var i = new Ol(e), s = n ? qt : 0;
  function l(a, o) {
    if (D) {
      var c = ai(
        /** @type {TemplateNode} */
        r
      );
      if (a !== parseInt(c.substring(1))) {
        var d = Mn();
        ve(d), i.anchor = d, Ge(!1), i.ensure(a, o), Ge(!0);
        return;
      }
    }
    i.ensure(a, o);
  }
  Er(() => {
    var a = !1;
    t((o, c = 0) => {
      a = !0, l(c, o);
    }), a || l(-1, null);
  }, s);
}
function es(e, t) {
  return t;
}
function Rl(e, t, n) {
  for (var r = [], i = t.length, s, l = t.length, a = 0; a < i; a++) {
    let m = t[a];
    Et(
      m,
      () => {
        if (s) {
          if (s.pending.delete(m), s.done.add(m), s.pending.size === 0) {
            var u = (
              /** @type {Set<EachOutroGroup>} */
              e.outrogroups
            );
            lr(e, Pn(s.done)), u.delete(s), u.size === 0 && (e.outrogroups = null);
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
      var c = (
        /** @type {Element} */
        n
      ), d = (
        /** @type {Element} */
        c.parentNode
      );
      Ii(d), d.append(c), e.items.clear();
    }
    lr(e, t, !o);
  } else
    s = {
      pending: new Set(t),
      done: /* @__PURE__ */ new Set()
    }, (e.outrogroups ?? (e.outrogroups = /* @__PURE__ */ new Set())).add(s);
}
function lr(e, t, n = !0) {
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
      s.f |= Le;
      const l = document.createDocumentFragment();
      kr(s, l);
    } else
      re(t[i], n);
  }
}
var Vr;
function ts(e, t, n, r, i, s = null) {
  var l = e, a = /* @__PURE__ */ new Map(), o = (t & Zr) !== 0;
  if (o) {
    var c = (
      /** @type {Element} */
      e
    );
    l = D ? ve(/* @__PURE__ */ rn(c)) : c.appendChild(je());
  }
  D && Hn();
  var d = null, m = /* @__PURE__ */ yi(() => {
    var v = n();
    return (
      /** @type {V[]} */
      cr(v) ? v : v == null ? [] : Pn(v)
    );
  }), u, h = /* @__PURE__ */ new Map(), g = !0;
  function _(v) {
    (y.effect.f & he) === 0 && (y.pending.delete(v), y.fallback = d, Il(y, u, l, t, r), d !== null && (u.length === 0 ? (d.f & Le) === 0 ? On(d) : (d.f ^= Le, Zt(d, null, l)) : Et(d, () => {
      d = null;
    })));
  }
  function p(v) {
    y.pending.delete(v);
  }
  var b = Er(() => {
    u = /** @type {V[]} */
    M(m);
    var v = u.length;
    let w = !1;
    if (D) {
      var A = ai(l) === fr;
      A !== (v === 0) && (l = Mn(), ve(l), Ge(!1), w = !0);
    }
    for (var F = /* @__PURE__ */ new Set(), I = (
      /** @type {Batch} */
      $
    ), Q = Di(), V = 0; V < v; V += 1) {
      D && P.nodeType === dn && /** @type {Comment} */
      P.data === ur && (l = /** @type {Comment} */
      P, w = !0, Ge(!1));
      var $e = u[V], oe = r($e, V), X = g ? null : a.get(oe);
      X ? (X.v && Vt(X.v, $e), X.i && Vt(X.i, V), Q && I.unskip_effect(X.e)) : (X = Dl(
        a,
        g ? l : Vr ?? (Vr = je()),
        $e,
        oe,
        V,
        i,
        t,
        n
      ), g || (X.e.f |= Le), a.set(oe, X)), F.add(oe);
    }
    if (v === 0 && s && !d && (g ? d = ye(() => s(l)) : (d = ye(() => s(Vr ?? (Vr = je()))), d.f |= Le)), v > F.size && Ds(), D && v > 0 && ve(Mn()), !g)
      if (h.set(I, F), Q) {
        for (const [ke, x] of a)
          F.has(ke) || I.skip_effect(x.e);
        I.oncommit(_), I.ondiscard(p);
      } else
        _(I);
    w && Ge(!0), M(m);
  }), y = { effect: b, items: a, pending: h, outrogroups: null, fallback: d };
  g = !1, D && (l = P);
}
function Xt(e) {
  for (; e !== null && (e.f & Oe) === 0; )
    e = e.next;
  return e;
}
function Il(e, t, n, r, i) {
  var $e, oe, X, ke, x, K, pn, Nr, Or;
  var s = (r & ps) !== 0, l = t.length, a = e.items, o = Xt(e.effect.first), c, d = null, m, u = [], h = [], g, _, p, b;
  if (s)
    for (b = 0; b < l; b += 1)
      g = t[b], _ = i(g, b), p = /** @type {EachItem} */
      a.get(_).e, (p.f & Le) === 0 && ((oe = ($e = p.nodes) == null ? void 0 : $e.a) == null || oe.measure(), (m ?? (m = /* @__PURE__ */ new Set())).add(p));
  for (b = 0; b < l; b += 1) {
    if (g = t[b], _ = i(g, b), p = /** @type {EachItem} */
    a.get(_).e, e.outrogroups !== null)
      for (const qe of e.outrogroups)
        qe.pending.delete(p), qe.done.delete(p);
    if ((p.f & te) !== 0 && (On(p), s && ((ke = (X = p.nodes) == null ? void 0 : X.a) == null || ke.unfix(), (m ?? (m = /* @__PURE__ */ new Set())).delete(p))), (p.f & Le) !== 0)
      if (p.f ^= Le, p === o)
        Zt(p, null, n);
      else {
        var y = d ? d.next : o;
        p === e.effect.last && (e.effect.last = p.prev), p.prev && (p.prev.next = p.next), p.next && (p.next.prev = p.prev), rt(e, d, p), rt(e, p, y), Zt(p, y, n), d = p, u = [], h = [], o = Xt(d.next);
        continue;
      }
    if (p !== o) {
      if (c !== void 0 && c.has(p)) {
        if (u.length < h.length) {
          var v = h[0], w;
          d = v.prev;
          var A = u[0], F = u[u.length - 1];
          for (w = 0; w < u.length; w += 1)
            Zt(u[w], v, n);
          for (w = 0; w < h.length; w += 1)
            c.delete(h[w]);
          rt(e, A.prev, F.next), rt(e, d, A), rt(e, F, v), o = v, d = F, b -= 1, u = [], h = [];
        } else
          c.delete(p), Zt(p, o, n), rt(e, p.prev, p.next), rt(e, p, d === null ? e.effect.first : d.next), rt(e, d, p), d = p;
        continue;
      }
      for (u = [], h = []; o !== null && o !== p; )
        (c ?? (c = /* @__PURE__ */ new Set())).add(o), h.push(o), o = Xt(o.next);
      if (o === null)
        continue;
    }
    (p.f & Le) === 0 && u.push(p), d = p, o = Xt(p.next);
  }
  if (e.outrogroups !== null) {
    for (const qe of e.outrogroups)
      qe.pending.size === 0 && (lr(e, Pn(qe.done)), (x = e.outrogroups) == null || x.delete(qe));
    e.outrogroups.size === 0 && (e.outrogroups = null);
  }
  if (o !== null || c !== void 0) {
    var I = [];
    if (c !== void 0)
      for (p of c)
        (p.f & te) === 0 && I.push(p);
    for (; o !== null; )
      (o.f & te) === 0 && o !== e.fallback && I.push(o), o = Xt(o.next);
    var Q = I.length;
    if (Q > 0) {
      var V = (r & Zr) !== 0 && l === 0 ? n : null;
      if (s) {
        for (b = 0; b < Q; b += 1)
          (pn = (K = I[b].nodes) == null ? void 0 : K.a) == null || pn.measure();
        for (b = 0; b < Q; b += 1)
          (Or = (Nr = I[b].nodes) == null ? void 0 : Nr.a) == null || Or.fix();
      }
      Rl(e, I, V);
    }
  }
  s && Ke(() => {
    var qe, Rr;
    if (m !== void 0)
      for (p of m)
        (Rr = (qe = p.nodes) == null ? void 0 : qe.a) == null || Rr.apply();
  });
}
function Dl(e, t, n, r, i, s, l, a) {
  var o = (l & hs) !== 0 ? (l & _s) === 0 ? /* @__PURE__ */ Mi(n, !1, !1) : St(n) : null, c = (l & vs) !== 0 ? St(i) : null;
  return {
    v: o,
    i: c,
    e: ye(() => (s(t, o ?? n, c ?? i, a), () => {
      e.delete(r);
    }))
  };
}
function Zt(e, t, n) {
  if (e.nodes)
    for (var r = e.nodes.start, i = e.nodes.end, s = t && (t.f & Le) === 0 ? (
      /** @type {EffectNodes} */
      t.nodes.start
    ) : n; r !== null; ) {
      var l = (
        /** @type {TemplateNode} */
        /* @__PURE__ */ nt(r)
      );
      if (s.before(r), r === i)
        return;
      r = l;
    }
}
function rt(e, t, n) {
  t === null ? e.effect.first = n : t.next = n, n === null ? e.effect.last = t : n.prev = t;
}
function ft(e, t) {
  ji(() => {
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
      const i = gr("style");
      i.id = t.hash, i.textContent = t.code, r.appendChild(i);
    }
  });
}
function ns(e, t, n = !1) {
  if (e.multiple) {
    if (t == null)
      return;
    if (!cr(t))
      return Xs();
    for (var r of e.options)
      r.selected = t.includes(Ur(r));
    return;
  }
  for (r of e.options) {
    var i = Ur(r);
    if (vl(i, t)) {
      r.selected = !0;
      return;
    }
  }
  (!n || t !== void 0) && (e.selectedIndex = -1);
}
function Pl(e) {
  var t = new MutationObserver(() => {
    ns(e, e.__value);
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
  }), yr(() => {
    t.disconnect();
  });
}
function Ur(e) {
  return "__value" in e ? e.__value : e.value;
}
const Ll = Symbol("is custom element"), jl = Symbol("is html"), Fl = li ? "link" : "LINK", Hl = li ? "progress" : "PROGRESS";
function Wn(e) {
  if (D) {
    var t = !1, n = () => {
      if (!t) {
        if (t = !0, e.hasAttribute("value")) {
          var r = e.value;
          Je(e, "value", null), e.value = r;
        }
        if (e.hasAttribute("checked")) {
          var i = e.checked;
          Je(e, "checked", null), e.checked = i;
        }
      }
    };
    e[si] = n, Ke(n), pl();
  }
}
function Tr(e, t) {
  var n = Cr(e);
  n.value === (n.value = // treat null and undefined the same for the initial value
  t ?? void 0) || // @ts-expect-error
  // `progress` elements always need their value set when it's `0`
  e.value === t && (t !== 0 || e.nodeName !== Hl) || (e.value = t ?? "");
}
function rs(e, t) {
  var n = Cr(e);
  n.checked !== (n.checked = // treat null and undefined the same for the initial value
  t ?? void 0) && (e.checked = t);
}
function Wl(e, t) {
  t ? e.hasAttribute("selected") || e.setAttribute("selected", "") : e.removeAttribute("selected");
}
function Je(e, t, n, r) {
  var i = Cr(e);
  D && (i[t] = e.getAttribute(t), t === "src" || t === "srcset" || t === "href" && e.nodeName === Fl) || i[t] !== (i[t] = n) && (t === "loading" && (e[Ns] = n), n == null ? e.removeAttribute(t) : typeof n != "string" && Yl(e).includes(t) ? e[t] = n : e.setAttribute(t, n));
}
function Cr(e) {
  return (
    /** @type {Record<string | symbol, unknown>} **/
    /** @type {any} */
    e[mn] ?? (e[mn] = {
      [Ll]: e.nodeName.includes("-"),
      [jl]: e.namespaceURI === xs
    })
  );
}
var Xr = /* @__PURE__ */ new Map();
function Yl(e) {
  var t = e.getAttribute("is") || e.nodeName, n = Xr.get(t);
  if (n) return n;
  Xr.set(t, n = []);
  for (var r, i = e, s = Element.prototype; s !== i; ) {
    r = $s(i);
    for (var l in r)
      r[l].set && // better safe than sorry, we don't want spread attributes to mess with HTML content
      l !== "innerHTML" && l !== "textContent" && l !== "innerText" && n.push(l);
    i = ti(i);
  }
  return n;
}
function Un(e, t) {
  return e === t || (e == null ? void 0 : e[wt]) === t;
}
function Mr(e = {}, t, n, r) {
  var i = (
    /** @type {ComponentContext} */
    ne.r
  ), s = (
    /** @type {Effect} */
    S
  );
  return ji(() => {
    var l, a;
    return xr(() => {
      l = a, a = [], Sr(() => {
        Un(n(...a), e) || (t(e, ...a), l && Un(n(...l), e) && t(null, ...l));
      });
    }), () => {
      let o = s;
      for (; o !== i && o.parent !== null && o.parent.f & zn; )
        o = o.parent;
      const c = () => {
        a && Un(n(...a), e) && t(null, ...a);
      }, d = o.teardown;
      o.teardown = () => {
        c(), d == null || d();
      };
    };
  }), e;
}
function O(e, t, n, r) {
  var w;
  var i = !0, s = (n & bs) !== 0, l = (n & ys) !== 0, a = (
    /** @type {V} */
    r
  ), o = !0, c = (
    /** @type {Derived<V> | undefined} */
    void 0
  ), d = () => l && i ? (c ?? (c = /* @__PURE__ */ nn(
    /** @type {() => V} */
    r
  )), M(c)) : (o && (o = !1, a = l ? Sr(
    /** @type {() => V} */
    r
  ) : (
    /** @type {V} */
    r
  )), a);
  let m;
  if (s) {
    var u = wt in e || ii in e;
    m = ((w = yt(e, t)) == null ? void 0 : w.set) ?? (u && t in e ? (A) => e[t] = A : void 0);
  }
  var h, g = !1;
  s ? [h, g] = el(() => (
    /** @type {V} */
    e[t]
  )) : h = /** @type {V} */
  e[t], h === void 0 && r !== void 0 && (h = d(), m && (Ws(), m(h)));
  var _;
  if (_ = () => {
    var A = (
      /** @type {V} */
      e[t]
    );
    return A === void 0 ? d() : (o = !0, A);
  }, (n & ms) === 0)
    return _;
  if (m) {
    var p = e.$$legacy;
    return (
      /** @type {() => V} */
      (function(A, F) {
        return arguments.length > 0 ? ((!F || p || g) && m(F ? _() : A), A) : _();
      })
    );
  }
  var b = !1, y = ((n & gs) !== 0 ? nn : yi)(() => (b = !1, _()));
  s && M(y);
  var v = (
    /** @type {Effect} */
    S
  );
  return (
    /** @type {() => V} */
    (function(A, F) {
      if (arguments.length > 0) {
        const I = F ? M(y) : s ? dt(A) : A;
        return Me(y, I), b = !0, a !== void 0 && (a = I), A;
      }
      return Qe && b || (v.f & he) !== 0 ? y.v : M(y);
    })
  );
}
function ql(e) {
  return new Bl(e);
}
var ze, be;
class Bl {
  /**
   * @param {ComponentConstructorOptions & {
   *  component: any;
   * }} options
   */
  constructor(t) {
    /** @type {any} */
    k(this, ze);
    /** @type {Record<string, any>} */
    k(this, be);
    var s;
    var n = /* @__PURE__ */ new Map(), r = (l, a) => {
      var o = /* @__PURE__ */ Mi(a, !1, !1);
      return n.set(l, o), o;
    };
    const i = new Proxy(
      { ...t.props || {}, $$events: {} },
      {
        get(l, a) {
          return M(n.get(a) ?? r(a, Reflect.get(l, a)));
        },
        has(l, a) {
          return a === ii ? !0 : (M(n.get(a) ?? r(a, Reflect.get(l, a))), Reflect.has(l, a));
        },
        set(l, a, o) {
          return Me(n.get(a) ?? r(a, o), o), Reflect.set(l, a, o);
        }
      }
    );
    E(this, be, (t.hydrate ? Al : Zi)(t.component, {
      target: t.target,
      anchor: t.anchor,
      props: i,
      context: t.context,
      intro: t.intro ?? !1,
      recover: t.recover,
      transformError: t.transformError
    })), (!((s = t == null ? void 0 : t.props) != null && s.$$host) || t.sync === !1) && N(), E(this, ze, i.$$events);
    for (const l of Object.keys(f(this, be)))
      l === "$set" || l === "$destroy" || l === "$on" || Sn(this, l, {
        get() {
          return f(this, be)[l];
        },
        /** @param {any} value */
        set(a) {
          f(this, be)[l] = a;
        },
        enumerable: !0
      });
    f(this, be).$set = /** @param {Record<string, any>} next */
    (l) => {
      Object.assign(i, l);
    }, f(this, be).$destroy = () => {
      Nl(f(this, be));
    };
  }
  /** @param {Record<string, any>} props */
  $set(t) {
    f(this, be).$set(t);
  }
  /**
   * @param {string} event
   * @param {(...args: any[]) => any} callback
   * @returns {any}
   */
  $on(t, n) {
    f(this, ze)[t] = f(this, ze)[t] || [];
    const r = (...i) => n.call(this, ...i);
    return f(this, ze)[t].push(r), () => {
      f(this, ze)[t] = f(this, ze)[t].filter(
        /** @param {any} fn */
        (i) => i !== r
      );
    };
  }
  $destroy() {
    f(this, be).$destroy();
  }
}
ze = new WeakMap(), be = new WeakMap();
let is;
typeof HTMLElement == "function" && (is = class extends HTMLElement {
  /**
   * @param {*} $$componentCtor
   * @param {*} $$slots
   * @param {ShadowRootInit | undefined} shadow_root_init
   */
  constructor(t, n, r) {
    super();
    /** The Svelte component constructor */
    j(this, "$$ctor");
    /** Slots */
    j(this, "$$s");
    /** @type {any} The Svelte component instance */
    j(this, "$$c");
    /** Whether or not the custom element is connected */
    j(this, "$$cn", !1);
    /** @type {Record<string, any>} Component props data */
    j(this, "$$d", {});
    /** `true` if currently in the process of reflecting component props back to attributes */
    j(this, "$$r", !1);
    /** @type {Record<string, CustomElementPropDefinition>} Props definition (name, reflected, type etc) */
    j(this, "$$p_d", {});
    /** @type {Record<string, EventListenerOrEventListenerObject[]>} Event listeners */
    j(this, "$$l", {});
    /** @type {Map<EventListenerOrEventListenerObject, Function>} Event listener unsubscribe functions */
    j(this, "$$l_u", /* @__PURE__ */ new Map());
    /** @type {any} The managed render effect for reflecting attributes */
    j(this, "$$me");
    /** @type {ShadowRoot | null} The ShadowRoot of the custom element */
    j(this, "$$shadowRoot", null);
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
          const l = gr("slot");
          i !== "default" && (l.name = i), Z(s, l);
        };
      };
      if (await Promise.resolve(), !this.$$cn || this.$$c)
        return;
      const n = {}, r = Vl(this);
      for (const i of this.$$s)
        i in r && (i === "default" && !this.$$d.children ? (this.$$d.children = t(i), n.default = !0) : n[i] = t(i));
      for (const i of this.attributes) {
        const s = this.$$g_p(i.name);
        s in this.$$d || (this.$$d[s] = En(s, i.value, this.$$p_d, "toProp"));
      }
      for (const i in this.$$p_d)
        !(i in this.$$d) && this[i] !== void 0 && (this.$$d[i] = this[i], delete this[i]);
      this.$$c = ql({
        component: this.$$ctor,
        target: this.$$shadowRoot || this,
        props: {
          ...this.$$d,
          $$slots: n,
          $$host: this
        }
      }), this.$$me = ml(() => {
        xr(() => {
          var i;
          this.$$r = !0;
          for (const s of kn(this.$$c)) {
            if (!((i = this.$$p_d[s]) != null && i.reflect)) continue;
            this.$$d[s] = this.$$c[s];
            const l = En(
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
    this.$$r || (t = this.$$g_p(t), this.$$d[t] = En(t, r, this.$$p_d, "toProp"), (i = this.$$c) == null || i.$set({ [t]: this.$$d[t] }));
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
    return kn(this.$$p_d).find(
      (n) => this.$$p_d[n].attribute === t || !this.$$p_d[n].attribute && n.toLowerCase() === t
    ) || t;
  }
});
function En(e, t, n, r) {
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
function Vl(e) {
  const t = {};
  return e.childNodes.forEach((n) => {
    t[
      /** @type {Element} node */
      n.slot || "default"
    ] = !0;
  }), t;
}
function ut(e, t, n, r, i, s) {
  let l = class extends is {
    constructor() {
      super(e, n, i), this.$$p_d = t;
    }
    static get observedAttributes() {
      return kn(t).map(
        (a) => (t[a].attribute || a).toLowerCase()
      );
    }
  };
  return kn(t).forEach((a) => {
    Sn(l.prototype, a, {
      get() {
        return this.$$c && a in this.$$c ? this.$$c[a] : this.$$d[a];
      },
      set(o) {
        var m;
        o = En(a, o, t), this.$$d[a] = o;
        var c = this.$$c;
        if (c) {
          var d = (m = yt(c, a)) == null ? void 0 : m.get;
          d ? c[a] = o : c.$set({ [a]: o });
        }
      }
    });
  }), r.forEach((a) => {
    Sn(l.prototype, a, {
      get() {
        var o;
        return (o = this.$$c) == null ? void 0 : o[a];
      }
    });
  }), e.element = /** @type {any} */
  l, l;
}
var Ul = /* @__PURE__ */ ie('<span class="lbl"> </span>'), Xl = /* @__PURE__ */ ie('<label class="xi-slider svelte-8xo3l7"><!> <input type="range" class="svelte-8xo3l7"/> <span class="val svelte-8xo3l7"> </span></label>');
const zl = {
  hash: "svelte-8xo3l7",
  code: '.xi-slider.svelte-8xo3l7 {display:inline-flex;align-items:center;gap:0.5rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}input[type="range"].svelte-8xo3l7 {accent-color:var(--xi-accent, #3b82f6);vertical-align:middle;}.val.svelte-8xo3l7 {min-width:2.5em;text-align:right;font-variant-numeric:tabular-nums;}'
};
function Gl(e, t) {
  et(t, !0), ft(e, zl);
  let n = O(t, "value", 15, 0), r = O(t, "min", 7, 0), i = O(t, "max", 7, 100), s = O(t, "step", 7, 1), l = O(t, "label", 7, ""), a = O(t, "disabled", 7, !1);
  const o = t.$$host, c = (v) => o.dispatchEvent(new CustomEvent(v, { detail: { value: n() }, bubbles: !0, composed: !0 }));
  function d(v) {
    n(Number(v.target.value)), c("input");
  }
  function m(v) {
    n(Number(v.target.value)), c("change");
  }
  var u = {
    get value() {
      return n();
    },
    set value(v = 0) {
      n(v), N();
    },
    get min() {
      return r();
    },
    set min(v = 0) {
      r(v), N();
    },
    get max() {
      return i();
    },
    set max(v = 100) {
      i(v), N();
    },
    get step() {
      return s();
    },
    set step(v = 1) {
      s(v), N();
    },
    get label() {
      return l();
    },
    set label(v = "") {
      l(v), N();
    },
    get disabled() {
      return a();
    },
    set disabled(v = !1) {
      a(v), N();
    }
  }, h = Xl(), g = Y(h);
  {
    var _ = (v) => {
      var w = Ul(), A = Y(w, !0);
      W(w), pe(() => Re(A, l())), Z(v, w);
    };
    vn(g, (v) => {
      l() && v(_);
    });
  }
  var p = xe(g, 2);
  Wn(p);
  var b = xe(p, 2), y = Y(b, !0);
  return W(b), W(h), pe(() => {
    Je(p, "min", r()), Je(p, "max", i()), Je(p, "step", s()), Tr(p, n()), p.disabled = a(), Re(y, n());
  }), J("input", p, d), J("change", p, m), Z(e, h), tt(u);
}
Mt(["input", "change"]);
customElements.define("xi-slider", ut(
  Gl,
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
var Kl = /* @__PURE__ */ ie('<span class="lbl"> </span>'), Jl = /* @__PURE__ */ ie('<label class="xi-number svelte-1f6ykwb"><!> <input type="number" class="svelte-1f6ykwb"/></label>');
const Zl = {
  hash: "svelte-1f6ykwb",
  code: ".xi-number.svelte-1f6ykwb {display:inline-flex;align-items:center;gap:0.5rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}input.svelte-1f6ykwb {width:6em;padding:0.15rem 0.3rem;accent-color:var(--xi-accent, #3b82f6);}"
};
function Ql(e, t) {
  et(t, !0), ft(e, Zl);
  let n = O(t, "value", 15, 0), r = O(t, "min", 7), i = O(t, "max", 7), s = O(t, "step", 7, 1), l = O(t, "label", 7, ""), a = O(t, "disabled", 7, !1);
  const o = t.$$host, c = (y) => o.dispatchEvent(new CustomEvent(y, { detail: { value: n() }, bubbles: !0, composed: !0 })), d = (y) => y.target.value === "" ? null : Number(y.target.value);
  function m(y) {
    n(d(y)), c("input");
  }
  function u(y) {
    n(d(y)), c("change");
  }
  var h = {
    get value() {
      return n();
    },
    set value(y = 0) {
      n(y), N();
    },
    get min() {
      return r();
    },
    set min(y) {
      r(y), N();
    },
    get max() {
      return i();
    },
    set max(y) {
      i(y), N();
    },
    get step() {
      return s();
    },
    set step(y = 1) {
      s(y), N();
    },
    get label() {
      return l();
    },
    set label(y = "") {
      l(y), N();
    },
    get disabled() {
      return a();
    },
    set disabled(y = !1) {
      a(y), N();
    }
  }, g = Jl(), _ = Y(g);
  {
    var p = (y) => {
      var v = Kl(), w = Y(v, !0);
      W(v), pe(() => Re(w, l())), Z(y, v);
    };
    vn(_, (y) => {
      l() && y(p);
    });
  }
  var b = xe(_, 2);
  return Wn(b), W(g), pe(() => {
    Je(b, "min", r()), Je(b, "max", i()), Je(b, "step", s()), Tr(b, n()), b.disabled = a();
  }), J("input", b, m), J("change", b, u), Z(e, g), tt(h);
}
Mt(["input", "change"]);
customElements.define("xi-number", ut(
  Ql,
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
var ea = /* @__PURE__ */ ie('<span class="lbl"> </span>'), ta = /* @__PURE__ */ ie('<label class="xi-toggle svelte-141rfru"><input type="checkbox" class="svelte-141rfru"/> <!></label>');
const na = {
  hash: "svelte-141rfru",
  code: ".xi-toggle.svelte-141rfru {display:inline-flex;align-items:center;gap:0.4rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);cursor:pointer;}input.svelte-141rfru {accent-color:var(--xi-accent, #3b82f6);}"
};
function ra(e, t) {
  et(t, !0), ft(e, na);
  let n = O(t, "value", 15, !1), r = O(t, "label", 7, ""), i = O(t, "disabled", 7, !1);
  const s = t.$$host;
  function l(u) {
    n(u.target.checked), s.dispatchEvent(new CustomEvent("change", { detail: { value: n() }, bubbles: !0, composed: !0 }));
  }
  var a = {
    get value() {
      return n();
    },
    set value(u = !1) {
      n(u), N();
    },
    get label() {
      return r();
    },
    set label(u = "") {
      r(u), N();
    },
    get disabled() {
      return i();
    },
    set disabled(u = !1) {
      i(u), N();
    }
  }, o = ta(), c = Y(o);
  Wn(c);
  var d = xe(c, 2);
  {
    var m = (u) => {
      var h = ea(), g = Y(h, !0);
      W(h), pe(() => Re(g, r())), Z(u, h);
    };
    vn(d, (u) => {
      r() && u(m);
    });
  }
  return W(o), pe(() => {
    rs(c, n()), c.disabled = i();
  }), J("change", c, l), Z(e, o), tt(a);
}
Mt(["change"]);
customElements.define("xi-toggle", ut(
  ra,
  {
    value: { reflect: !0, type: "Boolean" },
    label: {},
    disabled: {}
  },
  [],
  [],
  { mode: "open" }
));
function ss(e) {
  let t = e;
  if (typeof e == "string")
    try {
      t = JSON.parse(e);
    } catch {
      t = [];
    }
  return Array.isArray(t) ? t.map((n) => n && typeof n == "object" ? { value: n.value, label: n.label ?? String(n.value) } : { value: n, label: String(n) }) : [];
}
var ia = /* @__PURE__ */ ie('<span class="lbl"> </span>'), sa = /* @__PURE__ */ ie('<label class="opt svelte-mtcxej"><input type="radio" class="svelte-mtcxej"/> <span> </span></label>'), la = /* @__PURE__ */ ie('<div class="xi-radio svelte-mtcxej" role="radiogroup"><!> <!></div>');
const aa = {
  hash: "svelte-mtcxej",
  code: ".xi-radio.svelte-mtcxej {display:inline-flex;align-items:center;gap:0.75rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}.opt.svelte-mtcxej {display:inline-flex;align-items:center;gap:0.25rem;cursor:pointer;}input.svelte-mtcxej {accent-color:var(--xi-accent, #3b82f6);}"
};
function oa(e, t) {
  et(t, !0), ft(e, aa);
  let n = O(t, "value", 15, ""), r = O(t, "options", 23, () => []), i = O(t, "label", 7, ""), s = O(t, "disabled", 7, !1), l = O(t, "name", 7, "xi-radio");
  const a = t.$$host, o = /* @__PURE__ */ bi(() => ss(r()));
  function c(_) {
    n(_), a.dispatchEvent(new CustomEvent("change", { detail: { value: n() }, bubbles: !0, composed: !0 }));
  }
  var d = {
    get value() {
      return n();
    },
    set value(_ = "") {
      n(_), N();
    },
    get options() {
      return r();
    },
    set options(_ = []) {
      r(_), N();
    },
    get label() {
      return i();
    },
    set label(_ = "") {
      i(_), N();
    },
    get disabled() {
      return s();
    },
    set disabled(_ = !1) {
      s(_), N();
    },
    get name() {
      return l();
    },
    set name(_ = "xi-radio") {
      l(_), N();
    }
  }, m = la(), u = Y(m);
  {
    var h = (_) => {
      var p = ia(), b = Y(p, !0);
      W(p), pe(() => Re(b, i())), Z(_, p);
    };
    vn(u, (_) => {
      i() && _(h);
    });
  }
  var g = xe(u, 2);
  return ts(g, 17, () => M(o), es, (_, p) => {
    var b = sa(), y = Y(b);
    Wn(y);
    var v = xe(y, 2), w = Y(v, !0);
    W(v), W(b), pe(() => {
      Je(y, "name", l()), Tr(y, M(p).value), rs(y, M(p).value === n()), y.disabled = s(), Re(w, M(p).label);
    }), J("change", y, () => c(M(p).value)), Z(_, b);
  }), W(m), Z(e, m), tt(d);
}
Mt(["change"]);
customElements.define("xi-radio", ut(
  oa,
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
var fa = /* @__PURE__ */ ie('<span class="lbl"> </span>'), ua = /* @__PURE__ */ ie("<option> </option>"), ca = /* @__PURE__ */ ie('<label class="xi-dropdown svelte-1wd9iqr"><!> <select class="svelte-1wd9iqr"></select></label>');
const da = {
  hash: "svelte-1wd9iqr",
  code: ".xi-dropdown.svelte-1wd9iqr {display:inline-flex;align-items:center;gap:0.5rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}select.svelte-1wd9iqr {padding:0.15rem 0.3rem;}"
};
function ha(e, t) {
  et(t, !0), ft(e, da);
  let n = O(t, "value", 15, ""), r = O(t, "options", 23, () => []), i = O(t, "label", 7, ""), s = O(t, "disabled", 7, !1);
  const l = t.$$host, a = /* @__PURE__ */ bi(() => ss(r()));
  function o(_) {
    n(_.target.value), l.dispatchEvent(new CustomEvent("change", { detail: { value: n() }, bubbles: !0, composed: !0 }));
  }
  var c = {
    get value() {
      return n();
    },
    set value(_ = "") {
      n(_), N();
    },
    get options() {
      return r();
    },
    set options(_ = []) {
      r(_), N();
    },
    get label() {
      return i();
    },
    set label(_ = "") {
      i(_), N();
    },
    get disabled() {
      return s();
    },
    set disabled(_ = !1) {
      s(_), N();
    }
  }, d = ca(), m = Y(d);
  {
    var u = (_) => {
      var p = fa(), b = Y(p, !0);
      W(p), pe(() => Re(b, i())), Z(_, p);
    };
    vn(m, (_) => {
      i() && _(u);
    });
  }
  var h = xe(m, 2);
  ts(h, 21, () => M(a), es, (_, p) => {
    var b = ua(), y = Y(b, !0);
    W(b);
    var v = {};
    pe(() => {
      Wl(b, M(p).value === n()), Re(y, M(p).label), v !== (v = M(p).value) && (b.value = (b.__value = M(p).value) ?? "");
    }), Z(_, b);
  }), W(h);
  var g;
  return Pl(h), W(d), pe(() => {
    h.disabled = s(), g !== (g = n()) && (h.value = (h.__value = n()) ?? "", ns(h, n()));
  }), J("change", h, o), Z(e, d), tt(c);
}
Mt(["change"]);
customElements.define("xi-dropdown", ut(
  ha,
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
var va = /* @__PURE__ */ ie('<div class="xi-trace svelte-1m1z2sd"><span class="lbl"> </span> <canvas class="svelte-1m1z2sd"></canvas> <span class="val svelte-1m1z2sd"> </span></div>');
const pa = {
  hash: "svelte-1m1z2sd",
  code: ".xi-trace.svelte-1m1z2sd {display:inline-flex;align-items:center;gap:0.5rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}canvas.svelte-1m1z2sd {width:120px;height:28px;display:block;}.val.svelte-1m1z2sd {min-width:3em;text-align:right;font-variant-numeric:tabular-nums;}"
};
function _a(e, t) {
  et(t, !0), ft(e, pa);
  let n = O(t, "key", 7, ""), r = O(t, "label", 7, ""), i = O(t, "max", 7, 60);
  const s = t.$$host;
  let l, a = /* @__PURE__ */ Ie(null), o = /* @__PURE__ */ Ie(dt([]));
  function c() {
    if (!l) return;
    const v = l.getContext && l.getContext("2d");
    if (!v) return;
    const w = l.width = l.clientWidth || 120, A = l.height = l.clientHeight || 28;
    if (v.clearRect(0, 0, w, A), M(o).length < 2) return;
    const F = Math.min(...M(o)), I = Math.max(...M(o)), Q = I - F || 1;
    v.beginPath(), M(o).forEach((V, $e) => {
      const oe = $e / (M(o).length - 1) * (w - 2) + 1, X = A - 2 - (V - F) / Q * (A - 4);
      $e ? v.lineTo(oe, X) : v.moveTo(oe, X);
    }), v.strokeStyle = getComputedStyle(s).getPropertyValue("--xi-accent") || "#3b82f6", v.lineWidth = 1.5, v.stroke();
  }
  function d(v) {
    const w = v && v[n()];
    w && (Me(a, w.value, !0), typeof w.value == "number" && Number.isFinite(w.value) && (Me(o, [...M(o), w.value].slice(-i()), !0), c()), s.dispatchEvent(new CustomEvent("sample", { detail: { value: w.value }, bubbles: !0, composed: !0 })));
  }
  wr(() => {
    s.update = d, Object.defineProperty(s, "latest", { get: () => M(a), configurable: !0 }), Object.defineProperty(s, "history", { get: () => M(o).slice(), configurable: !0 }), c();
  });
  const m = (v) => v == null ? "—" : typeof v == "number" ? Number.isInteger(v) ? v : v.toFixed(3) : String(v);
  var u = {
    get key() {
      return n();
    },
    set key(v = "") {
      n(v), N();
    },
    get label() {
      return r();
    },
    set label(v = "") {
      r(v), N();
    },
    get max() {
      return i();
    },
    set max(v = 60) {
      i(v), N();
    }
  }, h = va(), g = Y(h), _ = Y(g, !0);
  W(g);
  var p = xe(g, 2);
  Mr(p, (v) => l = v, () => l);
  var b = xe(p, 2), y = Y(b, !0);
  return W(b), W(h), pe(
    (v) => {
      Re(_, r() || n()), Re(y, v);
    },
    [() => m(M(a))]
  ), Z(e, h), tt(u);
}
customElements.define("xi-trace", ut(_a, { key: {}, label: {}, max: { type: "Number" } }, [], [], { mode: "open" }));
function ls() {
  return { scale: 1, panX: 0, panY: 0, imgW: 0, imgH: 0, viewW: 0, viewH: 0 };
}
function Ar(e, t, n) {
  return { x: (t - e.panX) / e.scale, y: (n - e.panY) / e.scale };
}
function ga(e, t, n) {
  return { x: e.panX + t * e.scale, y: e.panY + n * e.scale };
}
const ma = 0.05, ba = 64, ya = (e) => Math.max(ma, Math.min(ba, e));
function ar(e) {
  return !e.imgW || !e.imgH || !e.viewW || !e.viewH || (e.scale = Math.min(e.viewW / e.imgW, e.viewH / e.imgH) * 0.95, e.panX = (e.viewW - e.imgW * e.scale) / 2, e.panY = (e.viewH - e.imgH * e.scale) / 2), e;
}
function wa(e) {
  return e.scale = 1, e.panX = (e.viewW - e.imgW) / 2, e.panY = (e.viewH - e.imgH) / 2, e;
}
function as(e, t, n, r) {
  const { x: i, y: s } = Ar(e, t, n);
  return e.scale = ya(e.scale * r), e.panX = t - i * e.scale, e.panY = n - s * e.scale, e;
}
function xa(e, t, n) {
  return e.panX += t, e.panY += n, e;
}
var Ea = /* @__PURE__ */ ie('<canvas class="svelte-1yjweo0"></canvas>');
const $a = {
  hash: "svelte-1yjweo0",
  code: ":host {display:block;width:100%;height:100%;}canvas.svelte-1yjweo0 {width:100%;height:100%;display:block;background:var(--xi-viewer-bg, #111);cursor:grab;touch-action:none;image-rendering:pixelated;}canvas.svelte-1yjweo0:active {cursor:grabbing;}"
};
function ka(e, t) {
  et(t, !0), ft(e, $a);
  const n = t.$$host;
  let r;
  const i = ls();
  let s = null, l = null;
  function a() {
    if (!r) return;
    const v = r.getContext("2d");
    v.imageSmoothingEnabled = !1, v.clearRect(0, 0, r.width, r.height), s && (v.setTransform(i.scale, 0, 0, i.scale, i.panX, i.panY), v.drawImage(s, 0, 0), v.setTransform(1, 0, 0, 1, 0, 0));
  }
  function o() {
    if (!r) return;
    const v = r.getBoundingClientRect();
    r.width = Math.max(1, Math.round(v.width)), r.height = Math.max(1, Math.round(v.height)), i.viewW = r.width, i.viewH = r.height, a();
  }
  function c(v, w) {
    n.dispatchEvent(new CustomEvent(v, { detail: w, bubbles: !0, composed: !0 }));
  }
  function d(v) {
    const w = new Image();
    w.onload = () => {
      const A = !i.imgW;
      s = w, i.imgW = w.naturalWidth || w.width, i.imgH = w.naturalHeight || w.height, l = document.createElement("canvas"), l.width = i.imgW, l.height = i.imgH, l.getContext("2d").drawImage(w, 0, 0), A && ar(i), a();
    }, w.src = typeof v == "string" ? v : v.dataUrl;
  }
  function m(v) {
    if (!s) return;
    v.preventDefault();
    const w = r.getBoundingClientRect();
    as(i, v.clientX - w.left, v.clientY - w.top, v.deltaY < 0 ? 1.15 : 1 / 1.15), a(), c("viewchange", { scale: i.scale });
  }
  let u = null, h = !1;
  function g(v) {
    var w;
    s && (u = { x: v.clientX, y: v.clientY }, h = !1, (w = r.setPointerCapture) == null || w.call(r, v.pointerId));
  }
  function _(v) {
    if (!u) return;
    const w = v.clientX - u.x, A = v.clientY - u.y;
    (w || A) && (h = !0), xa(i, w, A), u = { x: v.clientX, y: v.clientY }, a();
  }
  function p(v) {
    u && !h && b(v), u = null;
  }
  function b(v) {
    if (!s || !l) return;
    const w = r.getBoundingClientRect(), A = Ar(i, v.clientX - w.left, v.clientY - w.top), F = Math.floor(A.x), I = Math.floor(A.y);
    let Q = null;
    if (F >= 0 && I >= 0 && F < i.imgW && I < i.imgH) {
      const V = l.getContext("2d").getImageData(F, I, 1, 1).data;
      Q = [V[0], V[1], V[2]];
    }
    c("pixelpick", { x: F, y: I, rgb: Q });
  }
  wr(() => {
    n.setFrame = d, n.fit = () => {
      ar(i), a(), c("viewchange", { scale: i.scale });
    }, n.oneToOne = () => {
      wa(i), a(), c("viewchange", { scale: i.scale });
    }, o();
    const v = new ResizeObserver(o);
    return v.observe(r), () => v.disconnect();
  });
  var y = Ea();
  Mr(y, (v) => r = v, () => r), Ji("wheel", y, m), J("pointerdown", y, g), J("pointermove", y, _), J("pointerup", y, p), Z(e, y), tt();
}
Mt(["pointerdown", "pointermove", "pointerup"]);
customElements.define("xi-image-viewer", ut(ka, {}, [], [], { mode: "open" }));
function Sa() {
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
function Ta() {
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
function Ca() {
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
const or = { point: Sa, rect: Ta, polygon: Ca };
function ja(e, t) {
  or[e] = t;
}
function zr(e) {
  return or[e] ? or[e]() : null;
}
var Ma = /* @__PURE__ */ ie('<div class="xi-editor svelte-1qrk1oj"><div class="bar svelte-1qrk1oj"><span class="lbl"> </span> <span class="spacer svelte-1qrk1oj"></span> <button class="cancel svelte-1qrk1oj">Cancel</button> <button class="commit svelte-1qrk1oj">Commit</button></div> <canvas class="svelte-1qrk1oj"></canvas></div>');
const Aa = {
  hash: "svelte-1qrk1oj",
  code: ":host {display:block;width:100%;height:100%;}.xi-editor.svelte-1qrk1oj {display:flex;flex-direction:column;height:100%;}.bar.svelte-1qrk1oj {display:flex;align-items:center;gap:0.5rem;padding:0.35rem 0.5rem;font:13px system-ui, sans-serif;background:var(--xi-bar-bg, #1f2937);color:#e5e7eb;}.spacer.svelte-1qrk1oj {flex:1;}button.svelte-1qrk1oj {padding:0.2rem 0.7rem;border:0;border-radius:4px;cursor:pointer;font:inherit;}.commit.svelte-1qrk1oj {background:var(--xi-accent, #6366f1);color:#fff;}.cancel.svelte-1qrk1oj {background:#374151;color:#e5e7eb;}canvas.svelte-1qrk1oj {flex:1;width:100%;display:block;background:var(--xi-viewer-bg, #111);cursor:crosshair;touch-action:none;image-rendering:pixelated;}"
};
function Na(e, t) {
  et(t, !0), ft(e, Aa);
  let n = O(t, "tool", 7, "rect"), r = O(t, "label", 7, "");
  const i = t.$$host;
  let s;
  const l = ls();
  let a = null, o = zr(n());
  const c = (x) => ga(l, x.x, x.y);
  function d() {
    if (!s) return;
    const x = s.getContext("2d");
    x && (x.imageSmoothingEnabled = !1, x.setTransform(1, 0, 0, 1, 0, 0), x.clearRect(0, 0, s.width, s.height), a && (x.setTransform(l.scale, 0, 0, l.scale, l.panX, l.panY), x.drawImage(a, 0, 0), x.setTransform(1, 0, 0, 1, 0, 0)), o && o.draw(x, c));
  }
  function m() {
    if (!s) return;
    const x = s.getBoundingClientRect();
    s.width = Math.max(1, Math.round(x.width)), s.height = Math.max(1, Math.round(x.height)), l.viewW = s.width, l.viewH = s.height, d();
  }
  function u(x) {
    const K = new Image();
    K.onload = () => {
      const pn = !l.imgW;
      a = K, l.imgW = K.naturalWidth || K.width, l.imgH = K.naturalHeight || K.height, pn && ar(l), d();
    }, K.src = typeof x == "string" ? x : x.dataUrl;
  }
  function h(x) {
    o = zr(x) || o, d();
  }
  const g = (x) => {
    const K = s.getBoundingClientRect();
    return Ar(l, x.clientX - K.left, x.clientY - K.top);
  };
  function _(x) {
    o && (o.onDown(g(x)), d());
  }
  function p(x) {
    o && x.buttons && (o.onMove(g(x)), d());
  }
  function b(x) {
    o && (o.onUp(g(x)), d());
  }
  function y(x) {
    o && (o.onDbl(g(x)), d());
  }
  function v(x) {
    if (!a) return;
    x.preventDefault();
    const K = s.getBoundingClientRect();
    as(l, x.clientX - K.left, x.clientY - K.top, x.deltaY < 0 ? 1.15 : 1 / 1.15), d();
  }
  function w() {
    !o || !o.done() || i.dispatchEvent(new CustomEvent("commit", {
      detail: { tool: o.type, result: o.result() },
      bubbles: !0,
      composed: !0
    }));
  }
  function A() {
    i.dispatchEvent(new CustomEvent("cancel", { detail: {}, bubbles: !0, composed: !0 }));
  }
  wr(() => {
    i.setFrame = u, i.setTool = h, i.getResult = () => o && o.done() ? o.result() : null, m();
    const x = new ResizeObserver(m);
    return x.observe(s), () => x.disconnect();
  });
  var F = {
    get tool() {
      return n();
    },
    set tool(x = "rect") {
      n(x), N();
    },
    get label() {
      return r();
    },
    set label(x = "") {
      r(x), N();
    }
  }, I = Ma(), Q = Y(I), V = Y(Q), $e = Y(V, !0);
  W(V);
  var oe = xe(V, 4), X = xe(oe, 2);
  W(Q);
  var ke = xe(Q, 2);
  return Mr(ke, (x) => s = x, () => s), W(I), pe(() => Re($e, r() || n())), J("click", oe, A), J("click", X, w), J("pointerdown", ke, _), J("pointermove", ke, p), J("pointerup", ke, b), J("dblclick", ke, y), Ji("wheel", ke, v), Z(e, I), tt(F);
}
Mt([
  "click",
  "pointerdown",
  "pointermove",
  "pointerup",
  "dblclick"
]);
customElements.define("xi-image-editor", ut(Na, { tool: {}, label: {} }, [], [], { mode: "open" }));
function os(e) {
  const t = typeof e == "string" ? JSON.parse(e) : e, n = t.items || t.vars || [], r = {};
  for (const i of n) r[i.name] = i;
  return { run_id: t.run_id, items: r };
}
const Oa = { 0: "image/jpeg", 1: "image/bmp", 2: "image/png" };
function fs(e) {
  const t = e instanceof Uint8Array ? e : new Uint8Array(e);
  if (t.byteLength < 20) throw new Error("preview frame shorter than 20-byte header");
  const n = new DataView(t.buffer, t.byteOffset, t.byteLength), r = n.getUint32(0, !1), i = n.getUint32(4, !1), s = n.getUint32(8, !1), l = n.getUint32(12, !1), a = n.getUint32(16, !1), o = t.subarray(20), c = Oa[i] || "application/octet-stream";
  return {
    gid: r,
    codec: i,
    width: s,
    height: l,
    channels: a,
    dataUrl: `data:${c};base64,${us(o)}`
  };
}
function us(e) {
  if (typeof Buffer < "u") return Buffer.from(e).toString("base64");
  let t = "";
  const n = 32768;
  for (let r = 0; r < e.length; r += n)
    t += String.fromCharCode.apply(null, e.subarray(r, r + n));
  return btoa(t);
}
const Fa = /* @__PURE__ */ Object.freeze(/* @__PURE__ */ Object.defineProperty({
  __proto__: null,
  bytesToBase64: us,
  decodePreviewFrame: fs,
  parseVars: os
}, Symbol.toStringTag, { value: "Module" }));
class Ha {
  /**
   * @param {string} url  e.g. "ws://127.0.0.1:7823/"
   * @param {{WebSocketImpl?: any}} [opts]  inject a WebSocket impl (node tests)
   */
  constructor(t, n = {}) {
    if (this.url = t, this._WS = n.WebSocketImpl || (typeof WebSocket < "u" ? WebSocket : null), !this._WS) throw new Error("no WebSocket implementation (pass opts.WebSocketImpl in node)");
    this.ws = null, this._id = 0, this._pending = /* @__PURE__ */ new Map(), this._listeners = {
      // type -> Set<cb>
      vars: /* @__PURE__ */ new Set(),
      instances: /* @__PURE__ */ new Set(),
      log: /* @__PURE__ */ new Set(),
      event: /* @__PURE__ */ new Set(),
      preview: /* @__PURE__ */ new Set(),
      hello: /* @__PURE__ */ new Set()
    };
  }
  // Open the socket; resolves once it's open. If opts.checkVersion is set, also
  // runs `cmd:version` and rejects on mismatch (fail-fast on protocol drift).
  //   checkVersion: (info) => boolean | RegExp | string   (string/RegExp tests info.version)
  connect(t = {}) {
    return new Promise((n, r) => {
      let i;
      try {
        i = new this._WS(this.url);
      } catch (s) {
        r(s);
        return;
      }
      i.binaryType = "arraybuffer", this.ws = i, i.onmessage = (s) => this._onMessage(s), i.onerror = (s) => {
        for (const { reject: l } of this._pending.values()) l(new Error("socket error"));
        this._pending.clear();
      }, i.onclose = () => {
        for (const { reject: s } of this._pending.values()) s(new Error("socket closed"));
        this._pending.clear();
      }, i.onopen = async () => {
        try {
          if (t.checkVersion) {
            const s = await this.cmd("version"), l = s && s.version;
            if (!(typeof t.checkVersion == "function" ? t.checkVersion(s) : t.checkVersion instanceof RegExp ? t.checkVersion.test(l) : l === t.checkVersion)) {
              r(new Error(`backend version mismatch: got ${l}`)), i.close();
              return;
            }
          }
          n(this);
        } catch (s) {
          r(s);
        }
      };
    });
  }
  _onMessage(t) {
    const n = t.data;
    if (n instanceof ArrayBuffer || typeof Buffer < "u" && n instanceof Buffer) {
      try {
        this._emit("preview", fs(n));
      } catch {
      }
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
    if (r.type === "vars") {
      this._emit("vars", os(r));
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
  onVars(t) {
    return this.on("vars", t);
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
  onPreview(t) {
    return this.on("preview", t);
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
const Ra = {
  slider: "xi-slider",
  number: "xi-number",
  toggle: "xi-toggle",
  radio: "xi-radio",
  dropdown: "xi-dropdown"
};
function Ia(e, { section: t = "Config", tag: n = "control" } = {}) {
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
async function Wa(e, t) {
  const { client: n, instance: r, sectionFilter: i } = t, s = e.ownerDocument || globalThis.document, l = await n.getInstanceDef(r) || {}, a = { ...l }, o = t.descriptor && t.descriptor.length ? t.descriptor : Ia(l), c = [];
  e.innerHTML = "";
  for (const d of o) {
    if (i && !i(d)) continue;
    const m = s.createElement("section");
    if (m.className = "xi-section", m.dataset.tag = d.tag || "control", d.section) {
      const u = s.createElement("h3");
      u.className = "xi-section-title", u.textContent = d.section, m.appendChild(u);
    }
    for (const u of d.controls || []) {
      const h = Ra[u.type] || "xi-number", g = s.createElement(h);
      u.label && g.setAttribute("label", u.label);
      for (const p of ["min", "max", "step"]) u[p] != null && g.setAttribute(p, String(u[p]));
      const _ = s.createElement("div");
      _.className = "xi-control", _.appendChild(g), m.appendChild(_), u.options != null && (g.options = u.options), u.key in a && (g.value = a[u.key]), g.addEventListener("change", async (p) => {
        a[u.key] = p.detail.value;
        try {
          await n.setInstanceDef(r, { ...a });
        } catch {
        }
        e.dispatchEvent(new CustomEvent("xi-change", { detail: { key: u.key, value: p.detail.value }, bubbles: !0 }));
      }), c.push({ el: g, key: u.key });
    }
    e.appendChild(m);
  }
  return {
    // Re-read the def from the backend and push values back into the controls.
    async refresh() {
      const d = await n.getInstanceDef(r) || {};
      Object.assign(a, d);
      for (const { el: m, key: u } of c) u in a && (m.value = a[u]);
    },
    destroy() {
      e.innerHTML = "";
    }
  };
}
function Ya(e) {
  const t = [];
  for (const n of e || [])
    if ((n.tag || "") === "status")
      for (const r of n.controls || [])
        t.push({
          type: r.type === "image" ? "image" : r.type === "trace" ? "trace" : "value",
          key: r.key,
          label: r.label || r.key
        });
  return t;
}
function qa(e, { client: t, items: n, columns: r = 3 }) {
  const i = e.ownerDocument || globalThis.document;
  e.innerHTML = "";
  const s = i.createElement("div");
  s.className = "xi-monitor", s.style.display = "grid", s.style.gap = "0.75rem", s.style.gridTemplateColumns = `repeat(${r}, minmax(0, 1fr))`, e.appendChild(s);
  const l = /* @__PURE__ */ new Map(), a = /* @__PURE__ */ new Map();
  for (const u of n) {
    const h = i.createElement("div");
    h.className = "xi-tile", h.dataset.key = u.key;
    const g = i.createElement("div");
    g.className = "xi-tile-label", g.textContent = u.label, h.appendChild(g);
    let _;
    u.type === "trace" ? (_ = i.createElement("xi-trace"), _.setAttribute("key", u.key)) : u.type === "image" ? (_ = i.createElement("xi-image-viewer"), _.style.height = "180px") : (_ = i.createElement("div"), _.className = "xi-value", _.textContent = "—"), h.appendChild(_), s.appendChild(h), l.set(u.key, { type: u.type, el: _ });
  }
  const o = (u) => {
    const h = u.items || {};
    for (const [g, _] of l) {
      const p = h[g];
      p && (_.type === "trace" ? _.el.update(h) : _.type === "image" ? p.gid != null && a.set(p.gid, _.el) : _.el.textContent = Da(p.value));
    }
  }, c = (u) => {
    const h = a.get(u.gid);
    h && h.setFrame(u.dataUrl);
  }, d = t.onVars(o), m = t.onPreview(c);
  return { destroy() {
    d(), m(), e.innerHTML = "";
  } };
}
function Da(e) {
  return e == null ? "—" : typeof e == "number" ? Number.isInteger(e) ? String(e) : e.toFixed(3) : typeof e == "boolean" ? e ? "true" : "false" : String(e);
}
const Ba = [
  "xi-slider",
  "xi-number",
  "xi-toggle",
  "xi-radio",
  "xi-dropdown",
  "xi-trace",
  "xi-image-viewer",
  "xi-image-editor"
];
export {
  Ra as CONTROL_TAGS,
  or as TOOLS,
  Ba as XI_COMPONENTS,
  Ha as XiClient,
  us as bytesToBase64,
  Ya as collectStatusItems,
  fs as decodePreviewFrame,
  Ia as inferDescriptor,
  zr as makeTool,
  qa as mountMonitor,
  Wa as mountPanel,
  os as parseVars,
  Fa as protocol,
  ja as registerTool
};
