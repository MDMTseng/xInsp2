var ws = Object.defineProperty;
var Yr = (e) => {
  throw TypeError(e);
};
var xs = (e, t, n) => t in e ? ws(e, t, { enumerable: !0, configurable: !0, writable: !0, value: n }) : e[t] = n;
var W = (e, t, n) => xs(e, typeof t != "symbol" ? t + "" : t, n), Un = (e, t, n) => t.has(e) || Yr("Cannot " + n);
var f = (e, t, n) => (Un(e, t, "read from private field"), n ? n.call(e) : t.get(e)), S = (e, t, n) => t.has(e) ? Yr("Cannot add the same private member more than once") : t instanceof WeakSet ? t.add(e) : t.set(e, n), k = (e, t, n, r) => (Un(e, t, "write to private field"), r ? r.call(e, n) : t.set(e, n), n), A = (e, t, n) => (Un(e, t, "access private method"), n);
var si;
typeof window < "u" && ((si = window.__svelte ?? (window.__svelte = {})).v ?? (si.v = /* @__PURE__ */ new Set())).add("5");
const Es = 1, $s = 2, oi = 4, ks = 8, Ts = 16, Ss = 1, Cs = 4, Ms = 8, As = 16, Os = 2, ui = "[", gr = "[!", qr = "[?", mr = "]", Xt = {}, V = Symbol("uninitialized"), Ns = "http://www.w3.org/1999/xhtml", fi = !1;
var _r = Array.isArray, Rs = Array.prototype.indexOf, Mn = Array.prototype.includes, Wn = Array.from, An = Object.keys, On = Object.defineProperty, Et = Object.getOwnPropertyDescriptor, Is = Object.getOwnPropertyDescriptors, Ds = Object.prototype, Ls = Array.prototype, ci = Object.getPrototypeOf, Br = Object.isExtensible;
const Ps = () => {
};
function Hs(e) {
  for (var t = 0; t < e.length; t++)
    e[t]();
}
function di() {
  var e, t, n = new Promise((r, i) => {
    e = r, t = i;
  });
  return { promise: n, resolve: e, reject: t };
}
const Z = 2, Vt = 4, Yn = 8, hi = 1 << 24, Ae = 16, Ne = 32, et = 64, Qn = 128, xe = 512, U = 1024, G = 2048, Fe = 4096, ne = 8192, ve = 16384, At = 32768, er = 1 << 25, Ut = 65536, Nn = 1 << 17, js = 1 << 18, Ot = 1 << 19, Fs = 1 << 20, Pe = 1 << 25, Ct = 65536, Rn = 1 << 21, Ht = 1 << 22, ut = 1 << 23, $t = Symbol("$state"), vi = Symbol("legacy props"), Ws = Symbol(""), En = Symbol("attributes"), Ys = Symbol("class"), qs = Symbol("style"), Qt = Symbol("text"), pi = Symbol("form reset"), qn = new class extends Error {
  constructor() {
    super(...arguments);
    W(this, "name", "StaleReactionError");
    W(this, "message", "The reaction that called `getAbortSignal()` was re-run or destroyed");
  }
}();
var li;
const gi = (
  // We gotta write it like this because after downleveling the pure comment may end up in the wrong location
  !!((li = globalThis.document) != null && li.contentType) && /* @__PURE__ */ globalThis.document.contentType.includes("xml")
), br = 3, mn = 8;
function Bs() {
  throw new Error("https://svelte.dev/e/async_derived_orphan");
}
function zs(e, t, n) {
  throw new Error("https://svelte.dev/e/each_key_duplicate");
}
function Xs(e) {
  throw new Error("https://svelte.dev/e/effect_in_teardown");
}
function Vs() {
  throw new Error("https://svelte.dev/e/effect_in_unowned_derived");
}
function Us(e) {
  throw new Error("https://svelte.dev/e/effect_orphan");
}
function Gs() {
  throw new Error("https://svelte.dev/e/effect_update_depth_exceeded");
}
function Ks() {
  throw new Error("https://svelte.dev/e/hydration_failed");
}
function Js(e) {
  throw new Error("https://svelte.dev/e/props_invalid_value");
}
function Zs() {
  throw new Error("https://svelte.dev/e/state_descriptors_fixed");
}
function Qs() {
  throw new Error("https://svelte.dev/e/state_prototype_fixed");
}
function el() {
  throw new Error("https://svelte.dev/e/state_unsafe_mutation");
}
function tl() {
  throw new Error("https://svelte.dev/e/svelte_boundary_reset_onerror");
}
function nl() {
  console.warn("https://svelte.dev/e/derived_inert");
}
function Bn(e) {
  console.warn("https://svelte.dev/e/hydration_mismatch");
}
function rl() {
  console.warn("https://svelte.dev/e/select_multiple_invalid_value");
}
function il() {
  console.warn("https://svelte.dev/e/svelte_boundary_reset_noop");
}
let P = !1;
function Je(e) {
  P = e;
}
let H;
function pe(e) {
  if (e === null)
    throw Bn(), Xt;
  return H = e;
}
function zn() {
  return pe(/* @__PURE__ */ it(H));
}
function z(e) {
  if (P) {
    if (/* @__PURE__ */ it(H) !== null)
      throw Bn(), Xt;
    H = e;
  }
}
function sl(e = 1) {
  if (P) {
    for (var t = e, n = H; t--; )
      n = /** @type {TemplateNode} */
      /* @__PURE__ */ it(n);
    H = n;
  }
}
function In(e = !0) {
  for (var t = 0, n = H; ; ) {
    if (n.nodeType === mn) {
      var r = (
        /** @type {Comment} */
        n.data
      );
      if (r === mr) {
        if (t === 0) return n;
        t -= 1;
      } else (r === ui || r === gr || // "[1", "[2", etc. for if blocks
      r[0] === "[" && !isNaN(Number(r.slice(1)))) && (t += 1);
    }
    var i = (
      /** @type {TemplateNode} */
      /* @__PURE__ */ it(n)
    );
    e && n.remove(), n = i;
  }
}
function mi(e) {
  if (!e || e.nodeType !== mn)
    throw Bn(), Xt;
  return (
    /** @type {Comment} */
    e.data
  );
}
function _i(e) {
  return e === this.v;
}
function ll(e, t) {
  return e != e ? t == t : e !== t || e !== null && typeof e == "object" || typeof e == "function";
}
function bi(e) {
  return !ll(e, this.v);
}
let al = !1, re = null;
function Gt(e) {
  re = e;
}
function nt(e, t = !1, n) {
  re = {
    p: re,
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
function rt(e) {
  var t = (
    /** @type {ComponentContext} */
    re
  ), n = t.e;
  if (n !== null) {
    t.e = null;
    for (var r of n)
      Vi(r);
  }
  return e !== void 0 && (t.x = e), t.i = !0, re = t.p, e ?? /** @type {T} */
  {};
}
function yi() {
  return !0;
}
let vt = [];
function wi() {
  var e = vt;
  vt = [], Hs(e);
}
function Ze(e) {
  if (vt.length === 0 && !ln) {
    var t = vt;
    queueMicrotask(() => {
      t === vt && wi();
    });
  }
  vt.push(e);
}
function ol() {
  for (; vt.length > 0; )
    wi();
}
function xi(e) {
  var t = C;
  if (t === null)
    return M.f |= ut, e;
  if ((t.f & At) === 0 && (t.f & Vt) === 0)
    throw e;
  ot(e, t);
}
function ot(e, t) {
  if (!(t !== null && (t.f & ve) !== 0)) {
    for (; t !== null; ) {
      if ((t.f & Qn) !== 0) {
        if ((t.f & At) === 0)
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
const ul = -7169;
function B(e, t) {
  e.f = e.f & ul | t;
}
function yr(e) {
  (e.f & xe) !== 0 || e.deps === null ? B(e, U) : B(e, Fe);
}
function Ei(e) {
  if (e !== null)
    for (const t of e)
      (t.f & Z) === 0 || (t.f & Ct) === 0 || (t.f ^= Ct, Ei(
        /** @type {Derived} */
        t.deps
      ));
}
function $i(e, t, n) {
  (e.f & G) !== 0 ? t.add(e) : (e.f & Fe) !== 0 && n.add(e), Ei(e.deps), B(e, U);
}
let wn = !1;
function fl(e) {
  var t = wn;
  try {
    return wn = !1, [e(), wn];
  } finally {
    wn = t;
  }
}
function cl(e) {
  let t = 0, n = Mt(0), r;
  return () => {
    Tr() && (O(n), Mr(() => (t === 0 && (r = Rr(() => e(() => an(n)))), t += 1, () => {
      Ze(() => {
        t -= 1, t === 0 && (r == null || r(), r = void 0, an(n));
      });
    })));
  };
}
var dl = Ut | Ot;
function hl(e, t, n, r) {
  new vl(e, t, n, r);
}
var ce, cn, _e, _t, ae, be, te, de, Xe, bt, lt, jt, dn, hn, Ve, Hn, F, ki, Ti, Si, tr, $n, kn, nr, rr;
class vl {
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
    S(this, ce);
    /** @type {TemplateNode | null} */
    S(this, cn, P ? H : null);
    /** @type {BoundaryProps} */
    S(this, _e);
    /** @type {((anchor: Node) => void)} */
    S(this, _t);
    /** @type {Effect} */
    S(this, ae);
    /** @type {Effect | null} */
    S(this, be, null);
    /** @type {Effect | null} */
    S(this, te, null);
    /** @type {Effect | null} */
    S(this, de, null);
    /** @type {DocumentFragment | null} */
    S(this, Xe, null);
    S(this, bt, 0);
    S(this, lt, 0);
    S(this, jt, !1);
    /** @type {Set<Effect>} */
    S(this, dn, /* @__PURE__ */ new Set());
    /** @type {Set<Effect>} */
    S(this, hn, /* @__PURE__ */ new Set());
    /**
     * A source containing the number of pending async deriveds/expressions.
     * Only created if `$effect.pending()` is used inside the boundary,
     * otherwise updating the source results in needless `Batch.ensure()`
     * calls followed by no-op flushes
     * @type {Source<number> | null}
     */
    S(this, Ve, null);
    S(this, Hn, cl(() => (k(this, Ve, Mt(f(this, bt))), () => {
      k(this, Ve, null);
    })));
    var s;
    k(this, ce, t), k(this, _e, n), k(this, _t, (l) => {
      var a = (
        /** @type {Effect} */
        C
      );
      a.b = this, a.f |= Qn, r(l);
    }), this.parent = /** @type {Effect} */
    C.b, this.transform_error = i ?? ((s = this.parent) == null ? void 0 : s.transform_error) ?? ((l) => l), k(this, ae, Ar(() => {
      if (P) {
        const l = (
          /** @type {Comment} */
          f(this, cn)
        );
        zn();
        const a = l.data === gr;
        if (l.data.startsWith(qr)) {
          const u = JSON.parse(l.data.slice(qr.length));
          A(this, F, Ti).call(this, u);
        } else a ? A(this, F, Si).call(this) : A(this, F, ki).call(this);
      } else
        A(this, F, tr).call(this);
    }, dl)), P && k(this, ce, H);
  }
  /**
   * Defer an effect inside a pending boundary until the boundary resolves
   * @param {Effect} effect
   */
  defer_effect(t) {
    $i(t, f(this, dn), f(this, hn));
  }
  /**
   * Returns `false` if the effect exists inside a boundary whose pending snippet is shown
   * @returns {boolean}
   */
  is_rendered() {
    return !this.is_pending && (!this.parent || this.parent.is_rendered());
  }
  has_pending_snippet() {
    return !!f(this, _e).pending;
  }
  /**
   * Update the source that powers `$effect.pending()` inside this boundary,
   * and controls when the current `pending` snippet (if any) is removed.
   * Do not call from inside the class
   * @param {1 | -1} d
   * @param {Batch} batch
   */
  update_pending_count(t, n) {
    A(this, F, nr).call(this, t, n), k(this, bt, f(this, bt) + t), !(!f(this, Ve) || f(this, jt)) && (k(this, jt, !0), Ze(() => {
      k(this, jt, !1), f(this, Ve) && Kt(f(this, Ve), f(this, bt));
    }));
  }
  get_effect_pending() {
    return f(this, Hn).call(this), O(
      /** @type {Source<number>} */
      f(this, Ve)
    );
  }
  /** @param {unknown} error */
  error(t) {
    if (!f(this, _e).onerror && !f(this, _e).failed)
      throw t;
    T != null && T.is_fork ? (f(this, be) && T.skip_effect(f(this, be)), f(this, te) && T.skip_effect(f(this, te)), f(this, de) && T.skip_effect(f(this, de)), T.oncommit(() => {
      A(this, F, rr).call(this, t);
    })) : A(this, F, rr).call(this, t);
  }
}
ce = new WeakMap(), cn = new WeakMap(), _e = new WeakMap(), _t = new WeakMap(), ae = new WeakMap(), be = new WeakMap(), te = new WeakMap(), de = new WeakMap(), Xe = new WeakMap(), bt = new WeakMap(), lt = new WeakMap(), jt = new WeakMap(), dn = new WeakMap(), hn = new WeakMap(), Ve = new WeakMap(), Hn = new WeakMap(), F = new WeakSet(), ki = function() {
  try {
    k(this, be, we(() => f(this, _t).call(this, f(this, ce))));
  } catch (t) {
    this.error(t);
  }
}, /**
 * @param {unknown} error The deserialized error from the server's hydration comment
 */
Ti = function(t) {
  const n = f(this, _e).failed;
  n && k(this, de, we(() => {
    n(
      f(this, ce),
      () => t,
      () => () => {
      }
    );
  }));
}, Si = function() {
  const t = f(this, _e).pending;
  t && (this.is_pending = !0, k(this, te, we(() => t(f(this, ce)))), Ze(() => {
    var n = k(this, Xe, document.createDocumentFragment()), r = He();
    n.append(r), k(this, be, A(this, F, kn).call(this, () => we(() => f(this, _t).call(this, r)))), f(this, lt) === 0 && (f(this, ce).before(n), k(this, Xe, null), Tt(
      /** @type {Effect} */
      f(this, te),
      () => {
        k(this, te, null);
      }
    ), A(this, F, $n).call(
      this,
      /** @type {Batch} */
      T
    ));
  }));
}, tr = function() {
  try {
    if (this.is_pending = this.has_pending_snippet(), k(this, lt, 0), k(this, bt, 0), k(this, be, we(() => {
      f(this, _t).call(this, f(this, ce));
    })), f(this, lt) > 0) {
      var t = k(this, Xe, document.createDocumentFragment());
      Nr(f(this, be), t);
      const n = (
        /** @type {(anchor: Node) => void} */
        f(this, _e).pending
      );
      k(this, te, we(() => n(f(this, ce))));
    } else
      A(this, F, $n).call(
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
$n = function(t) {
  this.is_pending = !1, t.transfer_effects(f(this, dn), f(this, hn));
}, /**
 * @template T
 * @param {() => T} fn
 */
kn = function(t) {
  var n = C, r = M, i = re;
  We(f(this, ae)), $e(f(this, ae)), Gt(f(this, ae).ctx);
  try {
    return ft.ensure(), t();
  } catch (s) {
    return xi(s), null;
  } finally {
    We(n), $e(r), Gt(i);
  }
}, /**
 * Updates the pending count associated with the currently visible pending snippet,
 * if any, such that we can replace the snippet with content once work is done
 * @param {1 | -1} d
 * @param {Batch} batch
 */
nr = function(t, n) {
  var r;
  if (!this.has_pending_snippet()) {
    this.parent && A(r = this.parent, F, nr).call(r, t, n);
    return;
  }
  k(this, lt, f(this, lt) + t), f(this, lt) === 0 && (A(this, F, $n).call(this, n), f(this, te) && Tt(f(this, te), () => {
    k(this, te, null);
  }), f(this, Xe) && (f(this, ce).before(f(this, Xe)), k(this, Xe, null)));
}, /**
 * @param {unknown} error
 */
rr = function(t) {
  f(this, be) && (ie(f(this, be)), k(this, be, null)), f(this, te) && (ie(f(this, te)), k(this, te, null)), f(this, de) && (ie(f(this, de)), k(this, de, null)), P && (pe(
    /** @type {TemplateNode} */
    f(this, cn)
  ), sl(), pe(In()));
  var n = f(this, _e).onerror;
  let r = f(this, _e).failed;
  var i = !1, s = !1;
  const l = () => {
    if (i) {
      il();
      return;
    }
    i = !0, s && tl(), f(this, de) !== null && Tt(f(this, de), () => {
      k(this, de, null);
    }), A(this, F, kn).call(this, () => {
      A(this, F, tr).call(this);
    });
  }, a = (o) => {
    try {
      s = !0, n == null || n(o, l), s = !1;
    } catch (u) {
      ot(u, f(this, ae) && f(this, ae).parent);
    }
    r && k(this, de, A(this, F, kn).call(this, () => {
      try {
        return we(() => {
          var u = (
            /** @type {Effect} */
            C
          );
          u.b = this, u.f |= Qn, r(
            f(this, ce),
            () => o,
            () => l
          );
        });
      } catch (u) {
        return ot(
          u,
          /** @type {Effect} */
          f(this, ae).parent
        ), null;
      }
    }));
  };
  Ze(() => {
    var o;
    try {
      o = this.transform_error(t);
    } catch (u) {
      ot(u, f(this, ae) && f(this, ae).parent);
      return;
    }
    o !== null && typeof o == "object" && typeof /** @type {any} */
    o.then == "function" ? o.then(
      a,
      /** @param {unknown} e */
      (u) => ot(u, f(this, ae) && f(this, ae).parent)
    ) : a(o);
  });
};
function pl(e, t, n, r) {
  const i = on;
  var s = e.filter((v) => !v.settled), l = t.map(i);
  if (n.length === 0 && s.length === 0) {
    r(l);
    return;
  }
  var a = (
    /** @type {Effect} */
    C
  ), o = gl(), u = s.length === 1 ? s[0].promise : s.length > 1 ? Promise.all(s.map((v) => v.promise)) : null;
  function h(v) {
    if ((a.f & ve) === 0) {
      o();
      try {
        r([...l, ...v]);
      } catch (g) {
        ot(g, a);
      }
      Dn();
    }
  }
  var m = Ci();
  if (n.length === 0) {
    u.then(() => h([])).finally(m);
    return;
  }
  function d() {
    Promise.all(n.map((v) => /* @__PURE__ */ ml(v))).then(h).catch((v) => ot(v, a)).finally(m);
  }
  u ? u.then(() => {
    o(), d(), Dn();
  }) : d();
}
function gl() {
  var e = (
    /** @type {Effect} */
    C
  ), t = M, n = re, r = (
    /** @type {Batch} */
    T
  );
  return function(s = !0) {
    We(e), $e(t), Gt(n), s && (e.f & ve) === 0 && (r == null || r.activate(), r == null || r.apply());
  };
}
function Dn(e = !0) {
  We(null), $e(null), Gt(null), e && (T == null || T.deactivate());
}
function Ci() {
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
function on(e) {
  var t = Z | G;
  return C !== null && (C.f |= Ot), {
    ctx: re,
    deps: null,
    effects: null,
    equals: _i,
    f: t,
    fn: e,
    reactions: null,
    rv: 0,
    v: (
      /** @type {V} */
      V
    ),
    wv: 0,
    parent: C,
    ac: null
  };
}
const en = Symbol("obsolete");
// @__NO_SIDE_EFFECTS__
function ml(e, t, n) {
  let r = (
    /** @type {Effect | null} */
    C
  );
  r === null && Bs();
  var i = (
    /** @type {Promise<V>} */
    /** @type {unknown} */
    void 0
  ), s = Mt(
    /** @type {V} */
    V
  ), l = !M, a = /* @__PURE__ */ new Set();
  return Al(() => {
    var v, g;
    var o = (
      /** @type {Effect} */
      C
    ), u = di();
    i = u.promise;
    try {
      Promise.resolve(e()).then(u.resolve, (p) => {
        p !== qn && u.reject(p);
      }).finally(Dn);
    } catch (p) {
      u.reject(p), Dn();
    }
    var h = (
      /** @type {Batch} */
      T
    );
    if (l) {
      if ((o.f & At) !== 0)
        var m = Ci();
      if (
        // boundary can be null if the async derived is inside an $effect.root not connected to the component render tree
        (v = r.b) != null && v.is_rendered()
      )
        (g = h.async_deriveds.get(o)) == null || g.reject(en);
      else
        for (const p of a.values())
          p.reject(en);
      a.add(u), h.async_deriveds.set(o, u);
    }
    const d = (p, c = void 0) => {
      m == null || m(), a.delete(u), c !== en && (h.activate(), c ? (s.f |= ut, Kt(s, c)) : ((s.f & ut) !== 0 && (s.f ^= ut), Kt(s, p)), h.deactivate());
    };
    u.promise.then(d, (p) => d(null, p || "unknown"));
  }), Sr(() => {
    for (const o of a)
      o.reject(en);
  }), new Promise((o) => {
    function u(h) {
      function m() {
        h === i ? o(s) : u(i);
      }
      h.then(m, m);
    }
    u(i);
  });
}
// @__NO_SIDE_EFFECTS__
function Mi(e) {
  const t = /* @__PURE__ */ on(e);
  return Qi(t), t;
}
// @__NO_SIDE_EFFECTS__
function Ai(e) {
  const t = /* @__PURE__ */ on(e);
  return t.equals = bi, t;
}
function _l(e) {
  var t = e.effects;
  if (t !== null) {
    e.effects = null;
    for (var n = 0; n < t.length; n += 1)
      ie(
        /** @type {Effect} */
        t[n]
      );
  }
}
function wr(e) {
  var t, n = C, r = e.parent;
  if (!tt && r !== null && e.v !== V && // if it was never evaluated before, it's guaranteed to fail downstream, so we try to execute instead
  (r.f & (ve | ne)) !== 0)
    return nl(), e.v;
  We(r);
  try {
    e.f &= ~Ct, _l(e), t = rs(e);
  } finally {
    We(n);
  }
  return t;
}
function Oi(e) {
  var t = wr(e);
  if (!e.equals(t) && (e.wv = ts(), (!(T != null && T.is_fork) || e.deps === null) && (T !== null ? (T.capture(e, t, !0), sn == null || sn.capture(e, t, !0)) : e.v = t, e.deps === null))) {
    B(e, U);
    return;
  }
  tt || (J !== null ? (Tr() || T != null && T.is_fork) && J.set(e, t) : yr(e));
}
function bl(e) {
  var t, n;
  if (e.effects !== null)
    for (const r of e.effects)
      (r.teardown || r.ac) && ((t = r.teardown) == null || t.call(r), (n = r.ac) == null || n.abort(qn), r.fn !== null && (r.teardown = Ps), r.ac = null, fn(r, 0), Or(r));
}
function Ni(e) {
  if (e.effects !== null)
    for (const t of e.effects)
      t.teardown && t.fn !== null && Jt(t);
}
let Gn = null, Dt = null, T = null, sn = null, J = null, ir = null, ln = !1, Kn = !1, Pt = null, Tn = null;
var zr = 0;
let yl = 1;
var Ft, at, yt, Wt, Yt, qt, Ue, Bt, oe, vn, Ge, Se, De, zt, wt, D, sr, tn, lr, Ri, Ii, Lt, wl, nn;
const jn = class jn {
  constructor() {
    S(this, D);
    W(this, "id", yl++);
    /** True as soon as `#process` was called */
    S(this, Ft, !1);
    W(this, "linked", !0);
    /** @type {Batch | null} */
    S(this, at, null);
    /** @type {Batch | null} */
    S(this, yt, null);
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
    S(this, qt, 0);
    /**
     * Async effects that are currently in flight, _not_ inside a pending boundary
     * @type {Map<Effect, number>}
     */
    S(this, Ue, /* @__PURE__ */ new Map());
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
    S(this, oe, []);
    /**
     * Effects created while this batch was active.
     * @type {Effect[]}
     */
    S(this, vn, []);
    /**
     * Deferred effects (which run after async work has completed) that are DIRTY
     * @type {Set<Effect>}
     */
    S(this, Ge, /* @__PURE__ */ new Set());
    /**
     * Deferred effects that are MAYBE_DIRTY
     * @type {Set<Effect>}
     */
    S(this, Se, /* @__PURE__ */ new Set());
    /**
     * A map of branches that still exist, but will be destroyed when this batch
     * is committed — we skip over these during `process`.
     * The value contains child effects that were dirty/maybe_dirty before being reset,
     * so they can be rescheduled if the branch survives.
     * @type {Map<Effect, { d: Effect[], m: Effect[] }>}
     */
    S(this, De, /* @__PURE__ */ new Map());
    /**
     * Inverse of #skipped_branches which we need to tell prior batches to unskip them when committing
     * @type {Set<Effect>}
     */
    S(this, zt, /* @__PURE__ */ new Set());
    W(this, "is_fork", !1);
    S(this, wt, !1);
    Dt === null ? Gn = Dt = this : (k(Dt, yt, this), k(this, at, Dt)), Dt = this;
  }
  /**
   * Add an effect to the #skipped_branches map and reset its children
   * @param {Effect} effect
   */
  skip_effect(t) {
    f(this, De).has(t) || f(this, De).set(t, { d: [], m: [] }), f(this, zt).delete(t);
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
        B(i, G), n(i);
      for (i of r.m)
        B(i, Fe), n(i);
    }
    f(this, zt).add(t);
  }
  /**
   * Associate a change to a given source with the current
   * batch, noting its previous and current values
   * @param {Value} source
   * @param {any} value
   * @param {boolean} [is_derived]
   */
  capture(t, n, r = !1) {
    t.v !== V && !this.previous.has(t) && this.previous.set(t, t.v), (t.f & ut) === 0 && (this.current.set(t, [n, r]), J == null || J.set(t, n)), this.is_fork || (t.v = n);
  }
  activate() {
    T = this;
  }
  deactivate() {
    T = null, J = null;
  }
  flush() {
    try {
      Kn = !0, T = this, A(this, D, tn).call(this);
    } finally {
      zr = 0, ir = null, Pt = null, Tn = null, Kn = !1, T = null, J = null, kt.clear();
    }
  }
  discard() {
    var t;
    for (const n of f(this, Yt)) n(this);
    f(this, Yt).clear();
    for (const n of this.async_deriveds.values())
      n.reject(en);
    A(this, D, nn).call(this), (t = f(this, Bt)) == null || t.resolve();
  }
  /**
   * @param {Effect} effect
   */
  register_created_effect(t) {
    f(this, vn).push(t);
  }
  /**
   * @param {boolean} blocking
   * @param {Effect} effect
   */
  increment(t, n) {
    if (k(this, qt, f(this, qt) + 1), t) {
      let r = f(this, Ue).get(n) ?? 0;
      f(this, Ue).set(n, r + 1);
    }
  }
  /**
   * @param {boolean} blocking
   * @param {Effect} effect
   */
  decrement(t, n) {
    if (k(this, qt, f(this, qt) - 1), t) {
      let r = f(this, Ue).get(n) ?? 0;
      r === 1 ? f(this, Ue).delete(n) : f(this, Ue).set(n, r - 1);
    }
    f(this, wt) || (k(this, wt, !0), Ze(() => {
      k(this, wt, !1), this.linked && this.flush();
    }));
  }
  /**
   * @param {Set<Effect>} dirty_effects
   * @param {Set<Effect>} maybe_dirty_effects
   */
  transfer_effects(t, n) {
    for (const r of t)
      f(this, Ge).add(r);
    for (const r of n)
      f(this, Se).add(r);
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
    return (f(this, Bt) ?? k(this, Bt, di())).promise;
  }
  static ensure() {
    if (T === null) {
      const t = T = new jn();
      !Kn && !ln && Ze(() => {
        f(t, Ft) || t.flush();
      });
    }
    return T;
  }
  apply() {
    {
      J = null;
      return;
    }
  }
  /**
   *
   * @param {Effect} effect
   */
  schedule(t) {
    var i;
    if (ir = t, (i = t.b) != null && i.is_pending && (t.f & (Vt | Yn | hi)) !== 0 && (t.f & At) === 0) {
      t.b.defer_effect(t);
      return;
    }
    for (var n = t; n.parent !== null; ) {
      n = n.parent;
      var r = n.f;
      if (Pt !== null && n === C && (M === null || (M.f & Z) === 0))
        return;
      if ((r & (et | Ne)) !== 0) {
        if ((r & U) === 0)
          return;
        n.f ^= U;
      }
    }
    f(this, oe).push(n);
  }
};
Ft = new WeakMap(), at = new WeakMap(), yt = new WeakMap(), Wt = new WeakMap(), Yt = new WeakMap(), qt = new WeakMap(), Ue = new WeakMap(), Bt = new WeakMap(), oe = new WeakMap(), vn = new WeakMap(), Ge = new WeakMap(), Se = new WeakMap(), De = new WeakMap(), zt = new WeakMap(), wt = new WeakMap(), D = new WeakSet(), sr = function() {
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
}, tn = function() {
  var o, u, h, m;
  k(this, Ft, !0), zr++ > 1e3 && (A(this, D, nn).call(this), xl());
  for (const d of f(this, Ge))
    f(this, Se).delete(d), B(d, G), this.schedule(d);
  for (const d of f(this, Se))
    B(d, Fe), this.schedule(d);
  const t = f(this, oe);
  k(this, oe, []), this.apply();
  var n = Pt = [], r = [], i = Tn = [];
  for (const d of t)
    try {
      A(this, D, lr).call(this, d, n, r);
    } catch (v) {
      throw Pi(d), A(this, D, sr).call(this) || this.discard(), v;
    }
  if (T = null, i.length > 0) {
    var s = jn.ensure();
    for (const d of i)
      s.schedule(d);
  }
  if (Pt = null, Tn = null, A(this, D, sr).call(this)) {
    A(this, D, Lt).call(this, r), A(this, D, Lt).call(this, n);
    for (const [d, v] of f(this, De))
      Li(d, v);
    i.length > 0 && /** @type {unknown} */
    A(o = T, D, tn).call(o);
    return;
  }
  const l = A(this, D, Ri).call(this);
  if (l) {
    A(this, D, Lt).call(this, r), A(this, D, Lt).call(this, n), A(u = l, D, Ii).call(u, this);
    return;
  }
  f(this, Ge).clear(), f(this, Se).clear();
  for (const d of f(this, Wt)) d(this);
  f(this, Wt).clear(), sn = this, Xr(r), Xr(n), sn = null, (h = f(this, Bt)) == null || h.resolve();
  var a = (
    /** @type {Batch | null} */
    /** @type {unknown} */
    T
  );
  if (f(this, qt) === 0 && (f(this, oe).length === 0 || a !== null) && A(this, D, nn).call(this), f(this, oe).length > 0)
    if (a !== null) {
      const d = a;
      f(d, oe).push(...f(this, oe).filter((v) => !f(d, oe).includes(v)));
    } else
      a = this;
  a !== null && A(m = a, D, tn).call(m);
}, /**
 * Traverse the effect tree, executing effects or stashing
 * them for later execution as appropriate
 * @param {Effect} root
 * @param {Effect[]} effects
 * @param {Effect[]} render_effects
 */
lr = function(t, n, r) {
  t.f ^= U;
  for (var i = t.first; i !== null; ) {
    var s = i.f, l = (s & (Ne | et)) !== 0, a = l && (s & U) !== 0, o = a || (s & ne) !== 0 || f(this, De).has(i);
    if (!o && i.fn !== null) {
      l ? i.f ^= U : (s & Vt) !== 0 ? n.push(i) : _n(i) && ((s & Ae) !== 0 && f(this, Se).add(i), Jt(i));
      var u = i.first;
      if (u !== null) {
        i = u;
        continue;
      }
    }
    for (; i !== null; ) {
      var h = i.next;
      if (h !== null) {
        i = h;
        break;
      }
      i = i.parent;
    }
  }
}, Ri = function() {
  for (var t = f(this, at); t !== null; ) {
    if (!t.is_fork) {
      for (const [n, [, r]] of this.current)
        if (t.current.has(n) && !r)
          return t;
    }
    t = f(t, at);
  }
  return null;
}, /**
 * @param {Batch} batch
 */
Ii = function(t) {
  var r;
  for (const [i, s] of t.current)
    !this.previous.has(i) && t.previous.has(i) && this.previous.set(i, t.previous.get(i)), this.current.set(i, s);
  for (const [i, s] of t.async_deriveds) {
    const l = this.async_deriveds.get(i);
    l && s.promise.then(l.resolve).catch(l.reject);
  }
  t.async_deriveds.clear(), this.transfer_effects(f(t, Ge), f(t, Se));
  const n = (i) => {
    var s = i.reactions;
    if (s !== null)
      for (const o of s) {
        var l = o.f;
        if ((l & Z) !== 0)
          n(
            /** @type {Derived} */
            o
          );
        else {
          var a = (
            /** @type {Effect} */
            o
          );
          l & (Ht | Ae) && !this.async_deriveds.has(a) && (f(this, Se).delete(a), B(a, G), this.schedule(a));
        }
      }
  };
  for (const i of this.current.keys())
    n(i);
  this.oncommit(() => t.discard()), A(r = t, D, nn).call(r), T = this, A(this, D, tn).call(this);
}, /**
 * @param {Effect[]} effects
 */
Lt = function(t) {
  for (var n = 0; n < t.length; n += 1)
    $i(t[n], f(this, Ge), f(this, Se));
}, wl = function() {
  var m;
  for (let d = Gn; d !== null; d = f(d, yt)) {
    var t = d.id < this.id, n = [];
    for (const [v, [g, p]] of this.current) {
      if (d.current.has(v)) {
        var r = (
          /** @type {[any, boolean]} */
          d.current.get(v)[0]
        );
        if (t && g !== r)
          d.current.set(v, [g, p]);
        else
          continue;
      }
      n.push(v);
    }
    if (t)
      for (const [v, g] of this.async_deriveds) {
        const p = d.async_deriveds.get(v);
        p && g.promise.then(p.resolve).catch(p.reject);
      }
    var i = [...d.current.keys()].filter(
      (v) => !/** @type {[any, boolean]} */
      d.current.get(v)[1]
    );
    if (!(!f(d, Ft) || i.length === 0)) {
      var s = i.filter((v) => !this.current.has(v));
      if (s.length === 0)
        t && d.discard();
      else if (n.length > 0) {
        if (t)
          for (const v of f(this, zt))
            d.unskip_effect(v, (g) => {
              var p;
              (g.f & (Ae | Ht)) !== 0 ? d.schedule(g) : A(p = d, D, Lt).call(p, [g]);
            });
        d.activate();
        var l = /* @__PURE__ */ new Set(), a = /* @__PURE__ */ new Map();
        for (var o of n)
          Di(o, s, l, a);
        a = /* @__PURE__ */ new Map();
        var u = [...d.current].filter(([v, g]) => {
          const p = this.current.get(v);
          return p ? p[0] !== g[0] || p[1] !== g[1] : !0;
        }).map(([v]) => v);
        if (u.length > 0)
          for (const v of f(this, vn))
            (v.f & (ve | ne | Nn)) === 0 && xr(v, u, a) && ((v.f & (Ht | Ae)) !== 0 ? (B(v, G), d.schedule(v)) : f(d, Ge).add(v));
        if (f(d, oe).length > 0 && !f(d, wt)) {
          d.apply();
          for (var h of f(d, oe))
            A(m = d, D, lr).call(m, h, [], []);
          k(d, oe, []);
        }
        d.deactivate();
      }
    }
  }
}, nn = function() {
  if (this.linked) {
    var t = f(this, at), n = f(this, yt);
    t === null ? Gn = n : k(t, yt, n), n === null ? Dt = t : k(n, at, t), this.linked = !1;
  }
};
let ft = jn;
function N(e) {
  var t = ln;
  ln = !0;
  try {
    for (var n; ; ) {
      if (ol(), T === null)
        return (
          /** @type {T} */
          n
        );
      T.flush();
    }
  } finally {
    ln = t;
  }
}
function xl() {
  try {
    Gs();
  } catch (e) {
    ot(e, ir);
  }
}
let Te = null;
function Xr(e) {
  var t = e.length;
  if (t !== 0) {
    for (var n = 0; n < t; ) {
      var r = e[n++];
      if ((r.f & (ve | ne)) === 0 && _n(r) && (Te = /* @__PURE__ */ new Set(), Jt(r), r.deps === null && r.first === null && r.nodes === null && r.teardown === null && r.ac === null && Ki(r), (Te == null ? void 0 : Te.size) > 0)) {
        kt.clear();
        for (const i of Te) {
          if ((i.f & (ve | ne)) !== 0) continue;
          const s = [i];
          let l = i.parent;
          for (; l !== null; )
            Te.has(l) && (Te.delete(l), s.push(l)), l = l.parent;
          for (let a = s.length - 1; a >= 0; a--) {
            const o = s[a];
            (o.f & (ve | ne)) === 0 && Jt(o);
          }
        }
        Te.clear();
      }
    }
    Te = null;
  }
}
function Di(e, t, n, r) {
  if (!n.has(e) && (n.add(e), e.reactions !== null))
    for (const i of e.reactions) {
      const s = i.f;
      (s & Z) !== 0 ? Di(
        /** @type {Derived} */
        i,
        t,
        n,
        r
      ) : (s & (Ht | Ae)) !== 0 && (s & G) === 0 && xr(i, t, r) && (B(i, G), Er(
        /** @type {Effect} */
        i
      ));
    }
}
function xr(e, t, n) {
  const r = n.get(e);
  if (r !== void 0) return r;
  if (e.deps !== null)
    for (const i of e.deps) {
      if (Mn.call(t, i))
        return !0;
      if ((i.f & Z) !== 0 && xr(
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
function Er(e) {
  T.schedule(e);
}
function Li(e, t) {
  if (!((e.f & Ne) !== 0 && (e.f & U) !== 0)) {
    (e.f & G) !== 0 ? t.d.push(e) : (e.f & Fe) !== 0 && t.m.push(e), B(e, U);
    for (var n = e.first; n !== null; )
      Li(n, t), n = n.next;
  }
}
function Pi(e) {
  B(e, U);
  for (var t = e.first; t !== null; )
    Pi(t), t = t.next;
}
let Ln = /* @__PURE__ */ new Set();
const kt = /* @__PURE__ */ new Map();
let Hi = !1;
function Mt(e, t) {
  var n = {
    f: 0,
    // TODO ideally we could skip this altogether, but it causes type errors
    v: e,
    reactions: null,
    equals: _i,
    rv: 0,
    wv: 0
  };
  return n;
}
// @__NO_SIDE_EFFECTS__
function Ie(e, t) {
  const n = Mt(e);
  return Qi(n), n;
}
// @__NO_SIDE_EFFECTS__
function ji(e, t = !1, n = !0) {
  const r = Mt(e);
  return t || (r.equals = bi), r;
}
function Me(e, t, n = !1) {
  M !== null && // since we are untracking the function inside `$inspect.with` we need to add this check
  // to ensure we error if state is set inside an inspect effect
  (!Oe || (M.f & Nn) !== 0) && yi() && (M.f & (Z | Ae | Ht | Nn)) !== 0 && (je === null || !je.has(e)) && el();
  let r = n ? pt(t) : t;
  return Kt(e, r, Tn);
}
function Kt(e, t, n = null) {
  if (!e.equals(t)) {
    kt.set(e, tt ? t : e.v);
    var r = ft.ensure();
    if (r.capture(e, t), (e.f & Z) !== 0) {
      const i = (
        /** @type {Derived} */
        e
      );
      (e.f & G) !== 0 && wr(i), J === null && yr(i);
    }
    e.wv = ts(), Fi(e, G, n), C !== null && (C.f & U) !== 0 && (C.f & (Ne | et)) === 0 && (me === null ? Rl([e]) : me.push(e)), !r.is_fork && Ln.size > 0 && !Hi && El();
  }
  return t;
}
function El() {
  Hi = !1;
  for (const e of Ln) {
    (e.f & U) !== 0 && B(e, Fe);
    let t;
    try {
      t = _n(e);
    } catch {
      t = !0;
    }
    t && Jt(e);
  }
  Ln.clear();
}
function an(e) {
  Me(e, e.v + 1);
}
function Fi(e, t, n) {
  var r = e.reactions;
  if (r !== null)
    for (var i = r.length, s = 0; s < i; s++) {
      var l = r[s], a = l.f, o = (a & G) === 0;
      if (o && B(l, t), (a & Nn) !== 0)
        Ln.add(
          /** @type {Effect} */
          l
        );
      else if ((a & Z) !== 0) {
        var u = (
          /** @type {Derived} */
          l
        );
        J == null || J.delete(u), (a & Ct) === 0 && (a & xe && (C === null || (C.f & Rn) === 0) && (l.f |= Ct), Fi(u, Fe, n));
      } else if (o) {
        var h = (
          /** @type {Effect} */
          l
        );
        (a & Ae) !== 0 && Te !== null && Te.add(h), n !== null ? n.push(h) : Er(h);
      }
    }
}
function pt(e) {
  if (typeof e != "object" || e === null || $t in e)
    return e;
  const t = ci(e);
  if (t !== Ds && t !== Ls)
    return e;
  var n = /* @__PURE__ */ new Map(), r = _r(e), i = /* @__PURE__ */ Ie(0), s = St, l = (a) => {
    if (St === s)
      return a();
    var o = M, u = St;
    $e(null), Jr(s);
    var h = a();
    return $e(o), Jr(u), h;
  };
  return r && n.set("length", /* @__PURE__ */ Ie(
    /** @type {any[]} */
    e.length
  )), new Proxy(
    /** @type {any} */
    e,
    {
      defineProperty(a, o, u) {
        (!("value" in u) || u.configurable === !1 || u.enumerable === !1 || u.writable === !1) && Zs();
        var h = n.get(o);
        return h === void 0 ? l(() => {
          var m = /* @__PURE__ */ Ie(u.value);
          return n.set(o, m), m;
        }) : Me(h, u.value, !0), !0;
      },
      deleteProperty(a, o) {
        var u = n.get(o);
        if (u === void 0) {
          if (o in a) {
            const h = l(() => /* @__PURE__ */ Ie(V));
            n.set(o, h), an(i);
          }
        } else
          Me(u, V), an(i);
        return !0;
      },
      get(a, o, u) {
        var v;
        if (o === $t)
          return e;
        var h = n.get(o), m = o in a;
        if (h === void 0 && (!m || (v = Et(a, o)) != null && v.writable) && (h = l(() => {
          var g = pt(m ? a[o] : V), p = /* @__PURE__ */ Ie(g);
          return p;
        }), n.set(o, h)), h !== void 0) {
          var d = O(h);
          return d === V ? void 0 : d;
        }
        return Reflect.get(a, o, u);
      },
      getOwnPropertyDescriptor(a, o) {
        var u = Reflect.getOwnPropertyDescriptor(a, o);
        if (u && "value" in u) {
          var h = n.get(o);
          h && (u.value = O(h));
        } else if (u === void 0) {
          var m = n.get(o), d = m == null ? void 0 : m.v;
          if (m !== void 0 && d !== V)
            return {
              enumerable: !0,
              configurable: !0,
              value: d,
              writable: !0
            };
        }
        return u;
      },
      has(a, o) {
        var d;
        if (o === $t)
          return !0;
        var u = n.get(o), h = u !== void 0 && u.v !== V || Reflect.has(a, o);
        if (u !== void 0 || C !== null && (!h || (d = Et(a, o)) != null && d.writable)) {
          u === void 0 && (u = l(() => {
            var v = h ? pt(a[o]) : V, g = /* @__PURE__ */ Ie(v);
            return g;
          }), n.set(o, u));
          var m = O(u);
          if (m === V)
            return !1;
        }
        return h;
      },
      set(a, o, u, h) {
        var _;
        var m = n.get(o), d = o in a;
        if (r && o === "length")
          for (var v = u; v < /** @type {Source<number>} */
          m.v; v += 1) {
            var g = n.get(v + "");
            g !== void 0 ? Me(g, V) : v in a && (g = l(() => /* @__PURE__ */ Ie(V)), n.set(v + "", g));
          }
        if (m === void 0)
          (!d || (_ = Et(a, o)) != null && _.writable) && (m = l(() => /* @__PURE__ */ Ie(void 0)), Me(m, pt(u)), n.set(o, m));
        else {
          d = m.v !== V;
          var p = l(() => pt(u));
          Me(m, p);
        }
        var c = Reflect.getOwnPropertyDescriptor(a, o);
        if (c != null && c.set && c.set.call(h, u), !d) {
          if (r && typeof o == "string") {
            var b = (
              /** @type {Source<number>} */
              n.get("length")
            ), w = Number(o);
            Number.isInteger(w) && w >= b.v && Me(b, w + 1);
          }
          an(i);
        }
        return !0;
      },
      ownKeys(a) {
        O(i);
        var o = Reflect.ownKeys(a).filter((m) => {
          var d = n.get(m);
          return d === void 0 || d.v !== V;
        });
        for (var [u, h] of n)
          h.v !== V && !(u in a) && o.push(u);
        return o;
      },
      setPrototypeOf() {
        Qs();
      }
    }
  );
}
function Vr(e) {
  try {
    if (e !== null && typeof e == "object" && $t in e)
      return e[$t];
  } catch {
  }
  return e;
}
function $l(e, t) {
  return Object.is(Vr(e), Vr(t));
}
var Ur, Wi, Yi, qi;
function ar() {
  if (Ur === void 0) {
    Ur = window, Wi = /Firefox/.test(navigator.userAgent);
    var e = Element.prototype, t = Node.prototype, n = Text.prototype;
    Yi = Et(t, "firstChild").get, qi = Et(t, "nextSibling").get, Br(e) && (e[Ys] = void 0, e[En] = null, e[qs] = void 0, e.__e = void 0), Br(n) && (n[Qt] = void 0);
  }
}
function He(e = "") {
  return document.createTextNode(e);
}
// @__NO_SIDE_EFFECTS__
function un(e) {
  return (
    /** @type {TemplateNode | null} */
    Yi.call(e)
  );
}
// @__NO_SIDE_EFFECTS__
function it(e) {
  return (
    /** @type {TemplateNode | null} */
    qi.call(e)
  );
}
function X(e, t) {
  if (!P)
    return /* @__PURE__ */ un(e);
  var n = /* @__PURE__ */ un(H);
  if (n === null)
    n = H.appendChild(He());
  else if (t && n.nodeType !== br) {
    var r = He();
    return n == null || n.before(r), pe(r), r;
  }
  return t && Xi(
    /** @type {Text} */
    n
  ), pe(n), n;
}
function Ee(e, t = 1, n = !1) {
  let r = P ? H : e;
  for (var i; t--; )
    i = r, r = /** @type {TemplateNode} */
    /* @__PURE__ */ it(r);
  if (!P)
    return r;
  if (n) {
    if ((r == null ? void 0 : r.nodeType) !== br) {
      var s = He();
      return r === null ? i == null || i.after(s) : r.before(s), pe(s), s;
    }
    Xi(
      /** @type {Text} */
      r
    );
  }
  return pe(r), r;
}
function Bi(e) {
  e.textContent = "";
}
function zi() {
  return !1;
}
function $r(e, t, n) {
  return (
    /** @type {T extends keyof HTMLElementTagNameMap ? HTMLElementTagNameMap[T] : Element} */
    n ? document.createElement(e, { is: n }) : document.createElement(e)
  );
}
function Xi(e) {
  if (
    /** @type {string} */
    e.nodeValue.length < 65536
  )
    return;
  let t = e.nextSibling;
  for (; t !== null && t.nodeType === br; )
    t.remove(), e.nodeValue += /** @type {string} */
    t.nodeValue, t = e.nextSibling;
}
let Gr = !1;
function kl() {
  Gr || (Gr = !0, document.addEventListener(
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
            (t = n[pi]) == null || t.call(n);
      });
    },
    // In the capture phase to guarantee we get noticed of it (no possibility of stopPropagation)
    { capture: !0 }
  ));
}
function kr(e) {
  var t = M, n = C;
  $e(null), We(null);
  try {
    return e();
  } finally {
    $e(t), We(n);
  }
}
function Tl(e) {
  C === null && (M === null && Us(), Vs()), tt && Xs();
}
function Sl(e, t) {
  var n = t.last;
  n === null ? t.last = t.first = e : (n.next = e, e.prev = n, t.last = e);
}
function Ye(e, t) {
  var n = C;
  n !== null && (n.f & ne) !== 0 && (e |= ne);
  var r = {
    ctx: re,
    deps: null,
    nodes: null,
    f: e | G | xe,
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
    Pt !== null ? Pt.push(r) : ft.ensure().schedule(r);
  else if (t !== null) {
    try {
      Jt(r);
    } catch (l) {
      throw ie(r), l;
    }
    i.deps === null && i.teardown === null && i.nodes === null && i.first === i.last && // either `null`, or a singular child
    (i.f & Ot) === 0 && (i = i.first, (e & Ae) !== 0 && (e & Ut) !== 0 && i !== null && (i.f |= Ut));
  }
  if (i !== null && (i.parent = n, n !== null && Sl(i, n), M !== null && (M.f & Z) !== 0 && (e & et) === 0)) {
    var s = (
      /** @type {Derived} */
      M
    );
    (s.effects ?? (s.effects = [])).push(i);
  }
  return r;
}
function Tr() {
  return M !== null && !Oe;
}
function Sr(e) {
  const t = Ye(Yn, null);
  return B(t, U), t.teardown = e, t;
}
function Cr(e) {
  Tl();
  var t = (
    /** @type {Effect} */
    C.f
  ), n = !M && (t & Ne) !== 0 && re !== null && !re.i;
  if (n) {
    var r = (
      /** @type {ComponentContext} */
      re
    );
    (r.e ?? (r.e = [])).push(e);
  } else
    return Vi(e);
}
function Vi(e) {
  return Ye(Vt | Fs, e);
}
function Cl(e) {
  ft.ensure();
  const t = Ye(et | Ot, e);
  return () => {
    ie(t);
  };
}
function Ml(e) {
  ft.ensure();
  const t = Ye(et | Ot, e);
  return (n = {}) => new Promise((r) => {
    n.outro ? Tt(t, () => {
      ie(t), r(void 0);
    }) : (ie(t), r(void 0));
  });
}
function Ui(e) {
  return Ye(Vt, e);
}
function Al(e) {
  return Ye(Ht | Ot, e);
}
function Mr(e, t = 0) {
  return Ye(Yn | t, e);
}
function ge(e, t = [], n = [], r = []) {
  pl(r, t, n, (i) => {
    Ye(Yn, () => {
      e(...i.map(O));
    });
  });
}
function Ar(e, t = 0) {
  var n = Ye(Ae | t, e);
  return n;
}
function we(e) {
  return Ye(Ne | Ot, e);
}
function Gi(e) {
  var t = e.teardown;
  if (t !== null) {
    const n = tt, r = M;
    Kr(!0), $e(null);
    try {
      t.call(null);
    } finally {
      Kr(n), $e(r);
    }
  }
}
function Or(e, t = !1) {
  var n = e.first;
  for (e.first = e.last = null; n !== null; ) {
    const i = n.ac;
    i !== null && kr(() => {
      i.abort(qn);
    });
    var r = n.next;
    (n.f & et) !== 0 ? n.parent = null : ie(n, t), n = r;
  }
}
function Ol(e) {
  for (var t = e.first; t !== null; ) {
    var n = t.next;
    (t.f & Ne) === 0 && ie(t), t = n;
  }
}
function ie(e, t = !0) {
  var n = !1;
  (t || (e.f & js) !== 0) && e.nodes !== null && e.nodes.end !== null && (Nl(
    e.nodes.start,
    /** @type {TemplateNode} */
    e.nodes.end
  ), n = !0), e.f |= er, Or(e, t && !n), fn(e, 0);
  var r = e.nodes && e.nodes.t;
  if (r !== null)
    for (const s of r)
      s.stop();
  Gi(e), e.f ^= er, e.f |= ve;
  var i = e.parent;
  i !== null && i.first !== null && Ki(e), e.next = e.prev = e.teardown = e.ctx = e.deps = e.fn = e.nodes = e.ac = e.b = null;
}
function Nl(e, t) {
  for (; e !== null; ) {
    var n = e === t ? null : /* @__PURE__ */ it(e);
    e.remove(), e = n;
  }
}
function Ki(e) {
  var t = e.parent, n = e.prev, r = e.next;
  n !== null && (n.next = r), r !== null && (r.prev = n), t !== null && (t.first === e && (t.first = r), t.last === e && (t.last = n));
}
function Tt(e, t, n = !0) {
  var r = [];
  Ji(e, r, !0);
  var i = () => {
    n && ie(e), t && t();
  }, s = r.length;
  if (s > 0) {
    var l = () => --s || i();
    for (var a of r)
      a.out(l);
  } else
    i();
}
function Ji(e, t, n) {
  if ((e.f & ne) === 0) {
    e.f ^= ne;
    var r = e.nodes && e.nodes.t;
    if (r !== null)
      for (const a of r)
        (a.is_global || n) && t.push(a);
    for (var i = e.first; i !== null; ) {
      var s = i.next;
      if ((i.f & et) === 0) {
        var l = (i.f & Ut) !== 0 || // If this is a branch effect without a block effect parent,
        // it means the parent block effect was pruned. In that case,
        // transparency information was transferred to the branch effect.
        (i.f & Ne) !== 0 && (e.f & Ae) !== 0;
        Ji(i, t, l ? n : !1);
      }
      i = s;
    }
  }
}
function Pn(e) {
  Zi(e, !0);
}
function Zi(e, t) {
  if ((e.f & ne) !== 0) {
    e.f ^= ne, (e.f & U) === 0 && (B(e, G), ft.ensure().schedule(e));
    for (var n = e.first; n !== null; ) {
      var r = n.next, i = (n.f & Ut) !== 0 || (n.f & Ne) !== 0;
      Zi(n, i ? t : !1), n = r;
    }
    var s = e.nodes && e.nodes.t;
    if (s !== null)
      for (const l of s)
        (l.is_global || t) && l.in();
  }
}
function Nr(e, t) {
  if (e.nodes)
    for (var n = e.nodes.start, r = e.nodes.end; n !== null; ) {
      var i = n === r ? null : /* @__PURE__ */ it(n);
      t.append(n), n = i;
    }
}
let Sn = !1, tt = !1;
function Kr(e) {
  tt = e;
}
let M = null, Oe = !1;
function $e(e) {
  M = e;
}
let C = null;
function We(e) {
  C = e;
}
let je = null;
function Qi(e) {
  M !== null && (je ?? (je = /* @__PURE__ */ new Set())).add(e);
}
let ue = null, fe = 0, me = null;
function Rl(e) {
  me = e;
}
let es = 1, gt = 0, St = gt;
function Jr(e) {
  St = e;
}
function ts() {
  return ++es;
}
function _n(e) {
  var t = e.f;
  if ((t & G) !== 0)
    return !0;
  if (t & Z && (e.f &= ~Ct), (t & Fe) !== 0) {
    for (var n = (
      /** @type {Value[]} */
      e.deps
    ), r = n.length, i = 0; i < r; i++) {
      var s = n[i];
      if (_n(
        /** @type {Derived} */
        s
      ) && Oi(
        /** @type {Derived} */
        s
      ), s.wv > e.wv)
        return !0;
    }
    (t & xe) !== 0 && // During time traveling we don't want to reset the status so that
    // traversal of the graph in the other batches still happens
    J === null && B(e, U);
  }
  return !1;
}
function ns(e, t, n = !0) {
  var r = e.reactions;
  if (r !== null && !(je !== null && je.has(e)))
    for (var i = 0; i < r.length; i++) {
      var s = r[i];
      (s.f & Z) !== 0 ? ns(
        /** @type {Derived} */
        s,
        t,
        !1
      ) : t === s && (n ? B(s, G) : (s.f & U) !== 0 && B(s, Fe), Er(
        /** @type {Effect} */
        s
      ));
    }
}
function rs(e) {
  var p;
  var t = ue, n = fe, r = me, i = M, s = je, l = re, a = Oe, o = St, u = e.f;
  ue = /** @type {null | Value[]} */
  null, fe = 0, me = null, M = (u & (Ne | et)) === 0 ? e : null, je = null, Gt(e.ctx), Oe = !1, St = ++gt, e.ac !== null && (kr(() => {
    e.ac.abort(qn);
  }), e.ac = null);
  try {
    e.f |= Rn;
    var h = (
      /** @type {Function} */
      e.fn
    ), m = h();
    e.f |= At;
    var d = e.deps, v = T == null ? void 0 : T.is_fork;
    if (ue !== null) {
      var g;
      if (v || fn(e, fe), d !== null && fe > 0)
        for (d.length = fe + ue.length, g = 0; g < ue.length; g++)
          d[fe + g] = ue[g];
      else
        e.deps = d = ue;
      if (Tr() && (e.f & xe) !== 0)
        for (g = fe; g < d.length; g++)
          ((p = d[g]).reactions ?? (p.reactions = [])).push(e);
    } else !v && d !== null && fe < d.length && (fn(e, fe), d.length = fe);
    if (yi() && me !== null && !Oe && d !== null && (e.f & (Z | Fe | G)) === 0)
      for (g = 0; g < /** @type {Source[]} */
      me.length; g++)
        ns(
          me[g],
          /** @type {Effect} */
          e
        );
    if (i !== null && i !== e) {
      if (gt++, i.deps !== null)
        for (let c = 0; c < n; c += 1)
          i.deps[c].rv = gt;
      if (t !== null)
        for (const c of t)
          c.rv = gt;
      me !== null && (r === null ? r = me : r.push(.../** @type {Source[]} */
      me));
    }
    return (e.f & ut) !== 0 && (e.f ^= ut), m;
  } catch (c) {
    return xi(c);
  } finally {
    e.f ^= Rn, ue = t, fe = n, me = r, M = i, je = s, Gt(l), Oe = a, St = o;
  }
}
function Il(e, t) {
  let n = t.reactions;
  if (n !== null) {
    var r = Rs.call(n, e);
    if (r !== -1) {
      var i = n.length - 1;
      i === 0 ? n = t.reactions = null : (n[r] = n[i], n.pop());
    }
  }
  if (n === null && (t.f & Z) !== 0 && // Destroying a child effect while updating a parent effect can cause a dependency to appear
  // to be unused, when in fact it is used by the currently-updating parent. Checking `new_deps`
  // allows us to skip the expensive work of disconnecting and immediately reconnecting it
  (ue === null || !Mn.call(ue, t))) {
    var s = (
      /** @type {Derived} */
      t
    );
    (s.f & xe) !== 0 && (s.f ^= xe, s.f &= ~Ct), s.v !== V && yr(s), bl(s), fn(s, 0);
  }
}
function fn(e, t) {
  var n = e.deps;
  if (n !== null)
    for (var r = t; r < n.length; r++)
      Il(e, n[r]);
}
function Jt(e) {
  var t = e.f;
  if ((t & ve) === 0) {
    B(e, U);
    var n = C, r = Sn;
    C = e, Sn = !0;
    try {
      (t & (Ae | hi)) !== 0 ? Ol(e) : Or(e), Gi(e);
      var i = rs(e);
      e.teardown = typeof i == "function" ? i : null, e.wv = es;
      var s;
      fi && al && (e.f & G) !== 0 && e.deps;
    } finally {
      Sn = r, C = n;
    }
  }
}
function O(e) {
  var t = e.f, n = (t & Z) !== 0;
  if (M !== null && !Oe) {
    var r = C !== null && (C.f & ve) !== 0;
    if (!r && (je === null || !je.has(e))) {
      var i = M.deps;
      if ((M.f & Rn) !== 0)
        e.rv < gt && (e.rv = gt, ue === null && i !== null && i[fe] === e ? fe++ : ue === null ? ue = [e] : ue.push(e));
      else {
        M.deps ?? (M.deps = []), Mn.call(M.deps, e) || M.deps.push(e);
        var s = e.reactions;
        s === null ? e.reactions = [M] : Mn.call(s, M) || s.push(M);
      }
    }
  }
  if (tt && kt.has(e))
    return kt.get(e);
  if (n) {
    var l = (
      /** @type {Derived} */
      e
    );
    if (tt) {
      var a = l.v;
      return ((l.f & U) === 0 && l.reactions !== null || ss(l)) && (a = wr(l)), kt.set(l, a), a;
    }
    var o = (l.f & xe) === 0 && !Oe && M !== null && (Sn || (M.f & xe) !== 0), u = (l.f & At) === 0;
    _n(l) && (o && (l.f |= xe), Oi(l)), o && !u && (Ni(l), is(l));
  }
  if (J != null && J.has(e))
    return J.get(e);
  if ((e.f & ut) !== 0)
    throw e.v;
  return e.v;
}
function is(e) {
  if (e.f |= xe, e.deps !== null)
    for (const t of e.deps)
      (t.reactions ?? (t.reactions = [])).push(e), (t.f & Z) !== 0 && (t.f & xe) === 0 && (Ni(
        /** @type {Derived} */
        t
      ), is(
        /** @type {Derived} */
        t
      ));
}
function ss(e) {
  if (e.v === V) return !0;
  if (e.deps === null) return !1;
  for (const t of e.deps)
    if (kt.has(t) || (t.f & Z) !== 0 && ss(
      /** @type {Derived} */
      t
    ))
      return !0;
  return !1;
}
function Rr(e) {
  var t = Oe;
  try {
    return Oe = !0, e();
  } finally {
    Oe = t;
  }
}
const mt = Symbol("events"), ls = /* @__PURE__ */ new Set(), or = /* @__PURE__ */ new Set();
function Dl(e, t, n, r = {}) {
  function i(s) {
    if (r.capture || ur.call(t, s), !s.cancelBubble)
      return kr(() => n == null ? void 0 : n.call(this, s));
  }
  return Ze(() => {
    t.addEventListener(e, i, r);
  }), i;
}
function as(e, t, n, r, i) {
  var s = { capture: r, passive: i }, l = Dl(e, t, n, s);
  (t === document.body || // @ts-ignore
  t === window || // @ts-ignore
  t === document || // Firefox has quirky behavior, it can happen that we still get "canplay" events when the element is already removed
  t instanceof HTMLMediaElement) && Sr(() => {
    t.removeEventListener(e, l, s);
  });
}
function Q(e, t, n) {
  (t[mt] ?? (t[mt] = {}))[e] = n;
}
function Nt(e) {
  for (var t = 0; t < e.length; t++)
    ls.add(e[t]);
  for (var n of or)
    n(e);
}
let Zr = null;
function ur(e) {
  var p, c;
  var t = this, n = (
    /** @type {Node} */
    t.ownerDocument
  ), r = e.type, i = ((p = e.composedPath) == null ? void 0 : p.call(e)) || [], s = (
    /** @type {null | Element} */
    i[0] || e.target
  );
  Zr = e;
  var l = 0, a = Zr === e && e[mt];
  if (a) {
    var o = i.indexOf(a);
    if (o !== -1 && (t === document || t === /** @type {any} */
    window)) {
      e[mt] = t;
      return;
    }
    var u = i.indexOf(t);
    if (u === -1)
      return;
    o <= u && (l = o);
  }
  if (s = /** @type {Element} */
  i[l] || e.target, s !== t) {
    On(e, "currentTarget", {
      configurable: !0,
      get() {
        return s || n;
      }
    });
    var h = M, m = C;
    $e(null), We(null);
    try {
      for (var d, v = []; s !== null && s !== t; ) {
        try {
          var g = (c = s[mt]) == null ? void 0 : c[r];
          g != null && (!/** @type {any} */
          s.disabled || // DOM could've been updated already by the time this is reached, so we check this as well
          // -> the target could not have been disabled because it emits the event in the first place
          e.target === s) && g.call(s, e);
        } catch (b) {
          d ? v.push(b) : d = b;
        }
        if (e.cancelBubble) break;
        l++, s = l < i.length ? (
          /** @type {Element} */
          i[l]
        ) : null;
      }
      if (d) {
        for (let b of v)
          queueMicrotask(() => {
            throw b;
          });
        throw d;
      }
    } finally {
      e[mt] = t, delete e.currentTarget, $e(h), We(m);
    }
  }
}
var ai;
const Jn = (
  // We gotta write it like this because after downleveling the pure comment may end up in the wrong location
  ((ai = globalThis == null ? void 0 : globalThis.window) == null ? void 0 : ai.trustedTypes) && /* @__PURE__ */ globalThis.window.trustedTypes.createPolicy("svelte-trusted-html", {
    /** @param {string} html */
    createHTML: (e) => e
  })
);
function Ll(e) {
  return (
    /** @type {string} */
    (Jn == null ? void 0 : Jn.createHTML(e)) ?? e
  );
}
function Pl(e) {
  var t = $r("template");
  return t.innerHTML = Ll(e.replaceAll("<!>", "<!---->")), t.content;
}
function fr(e, t) {
  var n = (
    /** @type {Effect} */
    C
  );
  n.nodes === null && (n.nodes = { start: e, end: t, a: null, t: null });
}
// @__NO_SIDE_EFFECTS__
function se(e, t) {
  var n = (t & Os) !== 0, r, i = !e.startsWith("<!>");
  return () => {
    if (P)
      return fr(H, null), H;
    r === void 0 && (r = Pl(i ? e : "<!>" + e), r = /** @type {TemplateNode} */
    /* @__PURE__ */ un(r));
    var s = (
      /** @type {TemplateNode} */
      n || Wi ? document.importNode(r, !0) : r.cloneNode(!0)
    );
    return fr(s, s), s;
  };
}
function ee(e, t) {
  if (P) {
    var n = (
      /** @type {Effect & { nodes: EffectNodes }} */
      C
    );
    ((n.f & At) === 0 || n.nodes.end === null) && (n.nodes.end = H), zn();
    return;
  }
  e !== null && e.before(
    /** @type {Node} */
    t
  );
}
const Hl = ["touchstart", "touchmove"];
function jl(e) {
  return Hl.includes(e);
}
function Re(e, t) {
  var n = t == null ? "" : typeof t == "object" ? `${t}` : t;
  n !== /** @type {any} */
  (e[Qt] ?? (e[Qt] = e.nodeValue)) && (e[Qt] = n, e.nodeValue = `${n}`);
}
function os(e, t) {
  return us(e, t);
}
function Fl(e, t) {
  ar(), t.intro = t.intro ?? !1;
  const n = t.target, r = P, i = H;
  try {
    for (var s = /* @__PURE__ */ un(n); s && (s.nodeType !== mn || /** @type {Comment} */
    s.data !== ui); )
      s = /* @__PURE__ */ it(s);
    if (!s)
      throw Xt;
    Je(!0), pe(
      /** @type {Comment} */
      s
    );
    const l = us(e, { ...t, anchor: s });
    return Je(!1), /**  @type {Exports} */
    l;
  } catch (l) {
    if (l instanceof Error && l.message.split(`
`).some((a) => a.startsWith("https://svelte.dev/e/")))
      throw l;
    return l !== Xt && console.warn("Failed to hydrate: ", l), t.recover === !1 && Ks(), ar(), Bi(n), Je(!1), os(e, t);
  } finally {
    Je(r), pe(i);
  }
}
const xn = /* @__PURE__ */ new Map();
function us(e, { target: t, anchor: n, props: r = {}, events: i, context: s, intro: l = !0, transformError: a }) {
  ar();
  var o = void 0, u = Ml(() => {
    var h = n ?? t.appendChild(He());
    hl(
      /** @type {TemplateNode} */
      h,
      {
        pending: () => {
        }
      },
      (v) => {
        nt({});
        var g = (
          /** @type {ComponentContext} */
          re
        );
        if (s && (g.c = s), i && (r.$$events = i), P && fr(
          /** @type {TemplateNode} */
          v,
          null
        ), o = e(v, r) || {}, P && (C.nodes.end = H, H === null || H.nodeType !== mn || /** @type {Comment} */
        H.data !== mr))
          throw Bn(), Xt;
        rt();
      },
      a
    );
    var m = /* @__PURE__ */ new Set(), d = (v) => {
      for (var g = 0; g < v.length; g++) {
        var p = v[g];
        if (!m.has(p)) {
          m.add(p);
          var c = jl(p);
          for (const _ of [t, document]) {
            var b = xn.get(_);
            b === void 0 && (b = /* @__PURE__ */ new Map(), xn.set(_, b));
            var w = b.get(p);
            w === void 0 ? (_.addEventListener(p, ur, { passive: c }), b.set(p, 1)) : b.set(p, w + 1);
          }
        }
      }
    };
    return d(Wn(ls)), or.add(d), () => {
      var c;
      for (var v of m)
        for (const b of [t, document]) {
          var g = (
            /** @type {Map<string, number>} */
            xn.get(b)
          ), p = (
            /** @type {number} */
            g.get(v)
          );
          --p == 0 ? (b.removeEventListener(v, ur), g.delete(v), g.size === 0 && xn.delete(b)) : g.set(v, p);
        }
      or.delete(d), h !== n && ((c = h.parentNode) == null || c.removeChild(h));
    };
  });
  return cr.set(o, u), o;
}
let cr = /* @__PURE__ */ new WeakMap();
function Wl(e, t) {
  const n = cr.get(e);
  return n ? (cr.delete(e), n(t)) : Promise.resolve();
}
var Ce, Le, he, xt, pn, gn, Fn;
class Yl {
  /**
   * @param {TemplateNode} anchor
   * @param {boolean} transition
   */
  constructor(t, n = !0) {
    /** @type {TemplateNode} */
    W(this, "anchor");
    /** @type {Map<Batch, Key>} */
    S(this, Ce, /* @__PURE__ */ new Map());
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
    S(this, Le, /* @__PURE__ */ new Map());
    /**
     * Similar to #onscreen with respect to the keys, but contains branches that are not yet
     * in the DOM, because their insertion is deferred.
     * @type {Map<Key, Branch>}
     */
    S(this, he, /* @__PURE__ */ new Map());
    /**
     * Keys of effects that are currently outroing
     * @type {Set<Key>}
     */
    S(this, xt, /* @__PURE__ */ new Set());
    /**
     * Whether to pause (i.e. outro) on change, or destroy immediately.
     * This is necessary for `<svelte:element>`
     */
    S(this, pn, !0);
    /**
     * @param {Batch} batch
     */
    S(this, gn, (t) => {
      if (f(this, Ce).has(t)) {
        var n = (
          /** @type {Key} */
          f(this, Ce).get(t)
        ), r = f(this, Le).get(n);
        if (r)
          Pn(r), f(this, xt).delete(n);
        else {
          var i = f(this, he).get(n);
          i && (Pn(i.effect), f(this, Le).set(n, i.effect), f(this, he).delete(n), i.fragment.lastChild.remove(), this.anchor.before(i.fragment), r = i.effect);
        }
        for (const [s, l] of f(this, Ce)) {
          if (f(this, Ce).delete(s), s === t)
            break;
          const a = f(this, he).get(l);
          a && (ie(a.effect), f(this, he).delete(l));
        }
        for (const [s, l] of f(this, Le)) {
          if (s === n || f(this, xt).has(s)) continue;
          const a = () => {
            if (Array.from(f(this, Ce).values()).includes(s)) {
              var u = document.createDocumentFragment();
              Nr(l, u), u.append(He()), f(this, he).set(s, { effect: l, fragment: u });
            } else
              ie(l);
            f(this, xt).delete(s), f(this, Le).delete(s);
          };
          f(this, pn) || !r ? (f(this, xt).add(s), Tt(l, a, !1)) : a();
        }
      }
    });
    /**
     * @param {Batch} batch
     */
    S(this, Fn, (t) => {
      f(this, Ce).delete(t);
      const n = Array.from(f(this, Ce).values());
      for (const [r, i] of f(this, he))
        n.includes(r) || (ie(i.effect), f(this, he).delete(r));
    });
    this.anchor = t, k(this, pn, n);
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
    ), i = zi();
    if (n && !f(this, Le).has(t) && !f(this, he).has(t))
      if (i) {
        var s = document.createDocumentFragment(), l = He();
        s.append(l), f(this, he).set(t, {
          effect: we(() => n(l)),
          fragment: s
        });
      } else
        f(this, Le).set(
          t,
          we(() => n(this.anchor))
        );
    if (f(this, Ce).set(r, t), i) {
      for (const [a, o] of f(this, Le))
        a === t ? r.unskip_effect(o) : r.skip_effect(o);
      for (const [a, o] of f(this, he))
        a === t ? r.unskip_effect(o.effect) : r.skip_effect(o.effect);
      r.oncommit(f(this, gn)), r.ondiscard(f(this, Fn));
    } else
      P && (this.anchor = H), f(this, gn).call(this, r);
  }
}
Ce = new WeakMap(), Le = new WeakMap(), he = new WeakMap(), xt = new WeakMap(), pn = new WeakMap(), gn = new WeakMap(), Fn = new WeakMap();
function bn(e, t, n = !1) {
  var r;
  P && (r = H, zn());
  var i = new Yl(e), s = n ? Ut : 0;
  function l(a, o) {
    if (P) {
      var u = mi(
        /** @type {TemplateNode} */
        r
      );
      if (a !== parseInt(u.substring(1))) {
        var h = In();
        pe(h), i.anchor = h, Je(!1), i.ensure(a, o), Je(!0);
        return;
      }
    }
    i.ensure(a, o);
  }
  Ar(() => {
    var a = !1;
    t((o, u = 0) => {
      a = !0, l(u, o);
    }), a || l(-1, null);
  }, s);
}
function fs(e, t) {
  return t;
}
function ql(e, t, n) {
  for (var r = [], i = t.length, s, l = t.length, a = 0; a < i; a++) {
    let m = t[a];
    Tt(
      m,
      () => {
        if (s) {
          if (s.pending.delete(m), s.done.add(m), s.pending.size === 0) {
            var d = (
              /** @type {Set<EachOutroGroup>} */
              e.outrogroups
            );
            dr(e, Wn(s.done)), d.delete(s), d.size === 0 && (e.outrogroups = null);
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
      ), h = (
        /** @type {Element} */
        u.parentNode
      );
      Bi(h), h.append(u), e.items.clear();
    }
    dr(e, t, !o);
  } else
    s = {
      pending: new Set(t),
      done: /* @__PURE__ */ new Set()
    }, (e.outrogroups ?? (e.outrogroups = /* @__PURE__ */ new Set())).add(s);
}
function dr(e, t, n = !0) {
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
      s.f |= Pe;
      const l = document.createDocumentFragment();
      Nr(s, l);
    } else
      ie(t[i], n);
  }
}
var Qr;
function cs(e, t, n, r, i, s = null) {
  var l = e, a = /* @__PURE__ */ new Map(), o = (t & oi) !== 0;
  if (o) {
    var u = (
      /** @type {Element} */
      e
    );
    l = P ? pe(/* @__PURE__ */ un(u)) : u.appendChild(He());
  }
  P && zn();
  var h = null, m = /* @__PURE__ */ Ai(() => {
    var _ = n();
    return (
      /** @type {V[]} */
      _r(_) ? _ : _ == null ? [] : Wn(_)
    );
  }), d, v = /* @__PURE__ */ new Map(), g = !0;
  function p(_) {
    (w.effect.f & ve) === 0 && (w.pending.delete(_), w.fallback = h, Bl(w, d, l, t, r), h !== null && (d.length === 0 ? (h.f & Pe) === 0 ? Pn(h) : (h.f ^= Pe, rn(h, null, l)) : Tt(h, () => {
      h = null;
    })));
  }
  function c(_) {
    w.pending.delete(_);
  }
  var b = Ar(() => {
    d = /** @type {V[]} */
    O(m);
    var _ = d.length;
    let E = !1;
    if (P) {
      var y = mi(l) === gr;
      y !== (_ === 0) && (l = In(), pe(l), Je(!1), E = !0);
    }
    for (var $ = /* @__PURE__ */ new Set(), R = (
      /** @type {Batch} */
      T
    ), Y = zi(), L = 0; L < _; L += 1) {
      P && H.nodeType === mn && /** @type {Comment} */
      H.data === mr && (l = /** @type {Comment} */
      H, E = !0, Je(!1));
      var j = d[L], q = r(j, L), K = g ? null : a.get(q);
      K ? (K.v && Kt(K.v, j), K.i && Kt(K.i, L), Y && R.unskip_effect(K.e)) : (K = zl(
        a,
        g ? l : Qr ?? (Qr = He()),
        j,
        q,
        L,
        i,
        t,
        n
      ), g || (K.e.f |= Pe), a.set(q, K)), $.add(q);
    }
    if (_ === 0 && s && !h && (g ? h = we(() => s(l)) : (h = we(() => s(Qr ?? (Qr = He()))), h.f |= Pe)), _ > $.size && zs(), P && _ > 0 && pe(In()), !g)
      if (v.set(R, $), Y) {
        for (const [ht, It] of a)
          $.has(ht) || R.skip_effect(It.e);
        R.oncommit(p), R.ondiscard(c);
      } else
        p(R);
    E && Je(!0), O(m);
  }), w = { effect: b, items: a, pending: v, outrogroups: null, fallback: h };
  g = !1, P && (l = H);
}
function Zt(e) {
  for (; e !== null && (e.f & Ne) === 0; )
    e = e.next;
  return e;
}
function Bl(e, t, n, r, i) {
  var j, q, K, ht, It, Be, x, le, Fr;
  var s = (r & ks) !== 0, l = t.length, a = e.items, o = Zt(e.effect.first), u, h = null, m, d = [], v = [], g, p, c, b;
  if (s)
    for (b = 0; b < l; b += 1)
      g = t[b], p = i(g, b), c = /** @type {EachItem} */
      a.get(p).e, (c.f & Pe) === 0 && ((q = (j = c.nodes) == null ? void 0 : j.a) == null || q.measure(), (m ?? (m = /* @__PURE__ */ new Set())).add(c));
  for (b = 0; b < l; b += 1) {
    if (g = t[b], p = i(g, b), c = /** @type {EachItem} */
    a.get(p).e, e.outrogroups !== null)
      for (const ze of e.outrogroups)
        ze.pending.delete(c), ze.done.delete(c);
    if ((c.f & ne) !== 0 && (Pn(c), s && ((ht = (K = c.nodes) == null ? void 0 : K.a) == null || ht.unfix(), (m ?? (m = /* @__PURE__ */ new Set())).delete(c))), (c.f & Pe) !== 0)
      if (c.f ^= Pe, c === o)
        rn(c, null, n);
      else {
        var w = h ? h.next : o;
        c === e.effect.last && (e.effect.last = c.prev), c.prev && (c.prev.next = c.next), c.next && (c.next.prev = c.prev), st(e, h, c), st(e, c, w), rn(c, w, n), h = c, d = [], v = [], o = Zt(h.next);
        continue;
      }
    if (c !== o) {
      if (u !== void 0 && u.has(c)) {
        if (d.length < v.length) {
          var _ = v[0], E;
          h = _.prev;
          var y = d[0], $ = d[d.length - 1];
          for (E = 0; E < d.length; E += 1)
            rn(d[E], _, n);
          for (E = 0; E < v.length; E += 1)
            u.delete(v[E]);
          st(e, y.prev, $.next), st(e, h, y), st(e, $, _), o = _, h = $, b -= 1, d = [], v = [];
        } else
          u.delete(c), rn(c, o, n), st(e, c.prev, c.next), st(e, c, h === null ? e.effect.first : h.next), st(e, h, c), h = c;
        continue;
      }
      for (d = [], v = []; o !== null && o !== c; )
        (u ?? (u = /* @__PURE__ */ new Set())).add(o), v.push(o), o = Zt(o.next);
      if (o === null)
        continue;
    }
    (c.f & Pe) === 0 && d.push(c), h = c, o = Zt(c.next);
  }
  if (e.outrogroups !== null) {
    for (const ze of e.outrogroups)
      ze.pending.size === 0 && (dr(e, Wn(ze.done)), (It = e.outrogroups) == null || It.delete(ze));
    e.outrogroups.size === 0 && (e.outrogroups = null);
  }
  if (o !== null || u !== void 0) {
    var R = [];
    if (u !== void 0)
      for (c of u)
        (c.f & ne) === 0 && R.push(c);
    for (; o !== null; )
      (o.f & ne) === 0 && o !== e.fallback && R.push(o), o = Zt(o.next);
    var Y = R.length;
    if (Y > 0) {
      var L = (r & oi) !== 0 && l === 0 ? n : null;
      if (s) {
        for (b = 0; b < Y; b += 1)
          (x = (Be = R[b].nodes) == null ? void 0 : Be.a) == null || x.measure();
        for (b = 0; b < Y; b += 1)
          (Fr = (le = R[b].nodes) == null ? void 0 : le.a) == null || Fr.fix();
      }
      ql(e, R, L);
    }
  }
  s && Ze(() => {
    var ze, Wr;
    if (m !== void 0)
      for (c of m)
        (Wr = (ze = c.nodes) == null ? void 0 : ze.a) == null || Wr.apply();
  });
}
function zl(e, t, n, r, i, s, l, a) {
  var o = (l & Es) !== 0 ? (l & Ts) === 0 ? /* @__PURE__ */ ji(n, !1, !1) : Mt(n) : null, u = (l & $s) !== 0 ? Mt(i) : null;
  return {
    v: o,
    i: u,
    e: we(() => (s(t, o ?? n, u ?? i, a), () => {
      e.delete(r);
    }))
  };
}
function rn(e, t, n) {
  if (e.nodes)
    for (var r = e.nodes.start, i = e.nodes.end, s = t && (t.f & Pe) === 0 ? (
      /** @type {EffectNodes} */
      t.nodes.start
    ) : n; r !== null; ) {
      var l = (
        /** @type {TemplateNode} */
        /* @__PURE__ */ it(r)
      );
      if (s.before(r), r === i)
        return;
      r = l;
    }
}
function st(e, t, n) {
  t === null ? e.effect.first = n : t.next = n, n === null ? e.effect.last = t : n.prev = t;
}
function ct(e, t) {
  Ui(() => {
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
      const i = $r("style");
      i.id = t.hash, i.textContent = t.code, r.appendChild(i);
    }
  });
}
function ds(e, t, n = !1) {
  if (e.multiple) {
    if (t == null)
      return;
    if (!_r(t))
      return rl();
    for (var r of e.options)
      r.selected = t.includes(ei(r));
    return;
  }
  for (r of e.options) {
    var i = ei(r);
    if ($l(i, t)) {
      r.selected = !0;
      return;
    }
  }
  (!n || t !== void 0) && (e.selectedIndex = -1);
}
function Xl(e) {
  var t = new MutationObserver(() => {
    ds(e, e.__value);
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
  }), Sr(() => {
    t.disconnect();
  });
}
function ei(e) {
  return "__value" in e ? e.__value : e.value;
}
const Vl = Symbol("is custom element"), Ul = Symbol("is html"), Gl = gi ? "link" : "LINK", Kl = gi ? "progress" : "PROGRESS";
function Xn(e) {
  if (P) {
    var t = !1, n = () => {
      if (!t) {
        if (t = !0, e.hasAttribute("value")) {
          var r = e.value;
          Qe(e, "value", null), e.value = r;
        }
        if (e.hasAttribute("checked")) {
          var i = e.checked;
          Qe(e, "checked", null), e.checked = i;
        }
      }
    };
    e[pi] = n, Ze(n), kl();
  }
}
function Ir(e, t) {
  var n = Dr(e);
  n.value === (n.value = // treat null and undefined the same for the initial value
  t ?? void 0) || // @ts-expect-error
  // `progress` elements always need their value set when it's `0`
  e.value === t && (t !== 0 || e.nodeName !== Kl) || (e.value = t ?? "");
}
function hs(e, t) {
  var n = Dr(e);
  n.checked !== (n.checked = // treat null and undefined the same for the initial value
  t ?? void 0) && (e.checked = t);
}
function Jl(e, t) {
  t ? e.hasAttribute("selected") || e.setAttribute("selected", "") : e.removeAttribute("selected");
}
function Qe(e, t, n, r) {
  var i = Dr(e);
  P && (i[t] = e.getAttribute(t), t === "src" || t === "srcset" || t === "href" && e.nodeName === Gl) || i[t] !== (i[t] = n) && (t === "loading" && (e[Ws] = n), n == null ? e.removeAttribute(t) : typeof n != "string" && Zl(e).includes(t) ? e[t] = n : e.setAttribute(t, n));
}
function Dr(e) {
  return (
    /** @type {Record<string | symbol, unknown>} **/
    /** @type {any} */
    e[En] ?? (e[En] = {
      [Vl]: e.nodeName.includes("-"),
      [Ul]: e.namespaceURI === Ns
    })
  );
}
var ti = /* @__PURE__ */ new Map();
function Zl(e) {
  var t = e.getAttribute("is") || e.nodeName, n = ti.get(t);
  if (n) return n;
  ti.set(t, n = []);
  for (var r, i = e, s = Element.prototype; s !== i; ) {
    r = Is(i);
    for (var l in r)
      r[l].set && // better safe than sorry, we don't want spread attributes to mess with HTML content
      l !== "innerHTML" && l !== "textContent" && l !== "innerText" && n.push(l);
    i = ci(i);
  }
  return n;
}
function Zn(e, t) {
  return e === t || (e == null ? void 0 : e[$t]) === t;
}
function Lr(e = {}, t, n, r) {
  var i = (
    /** @type {ComponentContext} */
    re.r
  ), s = (
    /** @type {Effect} */
    C
  );
  return Ui(() => {
    var l, a;
    return Mr(() => {
      l = a, a = [], Rr(() => {
        Zn(n(...a), e) || (t(e, ...a), l && Zn(n(...l), e) && t(null, ...l));
      });
    }), () => {
      let o = s;
      for (; o !== i && o.parent !== null && o.parent.f & er; )
        o = o.parent;
      const u = () => {
        a && Zn(n(...a), e) && t(null, ...a);
      }, h = o.teardown;
      o.teardown = () => {
        u(), h == null || h();
      };
    };
  }), e;
}
function I(e, t, n, r) {
  var E;
  var i = !0, s = (n & Ms) !== 0, l = (n & As) !== 0, a = (
    /** @type {V} */
    r
  ), o = !0, u = (
    /** @type {Derived<V> | undefined} */
    void 0
  ), h = () => l && i ? (u ?? (u = /* @__PURE__ */ on(
    /** @type {() => V} */
    r
  )), O(u)) : (o && (o = !1, a = l ? Rr(
    /** @type {() => V} */
    r
  ) : (
    /** @type {V} */
    r
  )), a);
  let m;
  if (s) {
    var d = $t in e || vi in e;
    m = ((E = Et(e, t)) == null ? void 0 : E.set) ?? (d && t in e ? (y) => e[t] = y : void 0);
  }
  var v, g = !1;
  s ? [v, g] = fl(() => (
    /** @type {V} */
    e[t]
  )) : v = /** @type {V} */
  e[t], v === void 0 && r !== void 0 && (v = h(), m && (Js(), m(v)));
  var p;
  if (p = () => {
    var y = (
      /** @type {V} */
      e[t]
    );
    return y === void 0 ? h() : (o = !0, y);
  }, (n & Cs) === 0)
    return p;
  if (m) {
    var c = e.$$legacy;
    return (
      /** @type {() => V} */
      (function(y, $) {
        return arguments.length > 0 ? ((!$ || c || g) && m($ ? p() : y), y) : p();
      })
    );
  }
  var b = !1, w = ((n & Ss) !== 0 ? on : Ai)(() => (b = !1, p()));
  s && O(w);
  var _ = (
    /** @type {Effect} */
    C
  );
  return (
    /** @type {() => V} */
    (function(y, $) {
      if (arguments.length > 0) {
        const R = $ ? O(w) : s ? pt(y) : y;
        return Me(w, R), b = !0, a !== void 0 && (a = R), y;
      }
      return tt && b || (_.f & ve) !== 0 ? w.v : O(w);
    })
  );
}
function Ql(e) {
  return new ea(e);
}
var Ke, ye;
class ea {
  /**
   * @param {ComponentConstructorOptions & {
   *  component: any;
   * }} options
   */
  constructor(t) {
    /** @type {any} */
    S(this, Ke);
    /** @type {Record<string, any>} */
    S(this, ye);
    var s;
    var n = /* @__PURE__ */ new Map(), r = (l, a) => {
      var o = /* @__PURE__ */ ji(a, !1, !1);
      return n.set(l, o), o;
    };
    const i = new Proxy(
      { ...t.props || {}, $$events: {} },
      {
        get(l, a) {
          return O(n.get(a) ?? r(a, Reflect.get(l, a)));
        },
        has(l, a) {
          return a === vi ? !0 : (O(n.get(a) ?? r(a, Reflect.get(l, a))), Reflect.has(l, a));
        },
        set(l, a, o) {
          return Me(n.get(a) ?? r(a, o), o), Reflect.set(l, a, o);
        }
      }
    );
    k(this, ye, (t.hydrate ? Fl : os)(t.component, {
      target: t.target,
      anchor: t.anchor,
      props: i,
      context: t.context,
      intro: t.intro ?? !1,
      recover: t.recover,
      transformError: t.transformError
    })), (!((s = t == null ? void 0 : t.props) != null && s.$$host) || t.sync === !1) && N(), k(this, Ke, i.$$events);
    for (const l of Object.keys(f(this, ye)))
      l === "$set" || l === "$destroy" || l === "$on" || On(this, l, {
        get() {
          return f(this, ye)[l];
        },
        /** @param {any} value */
        set(a) {
          f(this, ye)[l] = a;
        },
        enumerable: !0
      });
    f(this, ye).$set = /** @param {Record<string, any>} next */
    (l) => {
      Object.assign(i, l);
    }, f(this, ye).$destroy = () => {
      Wl(f(this, ye));
    };
  }
  /** @param {Record<string, any>} props */
  $set(t) {
    f(this, ye).$set(t);
  }
  /**
   * @param {string} event
   * @param {(...args: any[]) => any} callback
   * @returns {any}
   */
  $on(t, n) {
    f(this, Ke)[t] = f(this, Ke)[t] || [];
    const r = (...i) => n.call(this, ...i);
    return f(this, Ke)[t].push(r), () => {
      f(this, Ke)[t] = f(this, Ke)[t].filter(
        /** @param {any} fn */
        (i) => i !== r
      );
    };
  }
  $destroy() {
    f(this, ye).$destroy();
  }
}
Ke = new WeakMap(), ye = new WeakMap();
let vs;
typeof HTMLElement == "function" && (vs = class extends HTMLElement {
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
          const l = $r("slot");
          i !== "default" && (l.name = i), ee(s, l);
        };
      };
      if (await Promise.resolve(), !this.$$cn || this.$$c)
        return;
      const n = {}, r = ta(this);
      for (const i of this.$$s)
        i in r && (i === "default" && !this.$$d.children ? (this.$$d.children = t(i), n.default = !0) : n[i] = t(i));
      for (const i of this.attributes) {
        const s = this.$$g_p(i.name);
        s in this.$$d || (this.$$d[s] = Cn(s, i.value, this.$$p_d, "toProp"));
      }
      for (const i in this.$$p_d)
        !(i in this.$$d) && this[i] !== void 0 && (this.$$d[i] = this[i], delete this[i]);
      this.$$c = Ql({
        component: this.$$ctor,
        target: this.$$shadowRoot || this,
        props: {
          ...this.$$d,
          $$slots: n,
          $$host: this
        }
      }), this.$$me = Cl(() => {
        Mr(() => {
          var i;
          this.$$r = !0;
          for (const s of An(this.$$c)) {
            if (!((i = this.$$p_d[s]) != null && i.reflect)) continue;
            this.$$d[s] = this.$$c[s];
            const l = Cn(
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
    this.$$r || (t = this.$$g_p(t), this.$$d[t] = Cn(t, r, this.$$p_d, "toProp"), (i = this.$$c) == null || i.$set({ [t]: this.$$d[t] }));
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
    return An(this.$$p_d).find(
      (n) => this.$$p_d[n].attribute === t || !this.$$p_d[n].attribute && n.toLowerCase() === t
    ) || t;
  }
});
function Cn(e, t, n, r) {
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
function ta(e) {
  const t = {};
  return e.childNodes.forEach((n) => {
    t[
      /** @type {Element} node */
      n.slot || "default"
    ] = !0;
  }), t;
}
function dt(e, t, n, r, i, s) {
  let l = class extends vs {
    constructor() {
      super(e, n, i), this.$$p_d = t;
    }
    static get observedAttributes() {
      return An(t).map(
        (a) => (t[a].attribute || a).toLowerCase()
      );
    }
  };
  return An(t).forEach((a) => {
    On(l.prototype, a, {
      get() {
        return this.$$c && a in this.$$c ? this.$$c[a] : this.$$d[a];
      },
      set(o) {
        var m;
        o = Cn(a, o, t), this.$$d[a] = o;
        var u = this.$$c;
        if (u) {
          var h = (m = Et(u, a)) == null ? void 0 : m.get;
          h ? u[a] = o : u.$set({ [a]: o });
        }
      }
    });
  }), r.forEach((a) => {
    On(l.prototype, a, {
      get() {
        var o;
        return (o = this.$$c) == null ? void 0 : o[a];
      }
    });
  }), e.element = /** @type {any} */
  l, l;
}
var na = /* @__PURE__ */ se('<span class="lbl"> </span>'), ra = /* @__PURE__ */ se('<label class="xi-slider svelte-8xo3l7"><!> <input type="range" class="svelte-8xo3l7"/> <span class="val svelte-8xo3l7"> </span></label>');
const ia = {
  hash: "svelte-8xo3l7",
  code: '.xi-slider.svelte-8xo3l7 {display:inline-flex;align-items:center;gap:0.5rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}input[type="range"].svelte-8xo3l7 {accent-color:var(--xi-accent, #3b82f6);vertical-align:middle;}.val.svelte-8xo3l7 {min-width:2.5em;text-align:right;font-variant-numeric:tabular-nums;}'
};
function sa(e, t) {
  nt(t, !0), ct(e, ia);
  let n = I(t, "value", 15, 0), r = I(t, "min", 7, 0), i = I(t, "max", 7, 100), s = I(t, "step", 7, 1), l = I(t, "label", 7, ""), a = I(t, "disabled", 7, !1);
  const o = t.$$host, u = (_) => o.dispatchEvent(new CustomEvent(_, { detail: { value: n() }, bubbles: !0, composed: !0 }));
  function h(_) {
    n(Number(_.target.value)), u("input");
  }
  function m(_) {
    n(Number(_.target.value)), u("change");
  }
  var d = {
    get value() {
      return n();
    },
    set value(_ = 0) {
      n(_), N();
    },
    get min() {
      return r();
    },
    set min(_ = 0) {
      r(_), N();
    },
    get max() {
      return i();
    },
    set max(_ = 100) {
      i(_), N();
    },
    get step() {
      return s();
    },
    set step(_ = 1) {
      s(_), N();
    },
    get label() {
      return l();
    },
    set label(_ = "") {
      l(_), N();
    },
    get disabled() {
      return a();
    },
    set disabled(_ = !1) {
      a(_), N();
    }
  }, v = ra(), g = X(v);
  {
    var p = (_) => {
      var E = na(), y = X(E, !0);
      z(E), ge(() => Re(y, l())), ee(_, E);
    };
    bn(g, (_) => {
      l() && _(p);
    });
  }
  var c = Ee(g, 2);
  Xn(c);
  var b = Ee(c, 2), w = X(b, !0);
  return z(b), z(v), ge(() => {
    Qe(c, "min", r()), Qe(c, "max", i()), Qe(c, "step", s()), Ir(c, n()), c.disabled = a(), Re(w, n());
  }), Q("input", c, h), Q("change", c, m), ee(e, v), rt(d);
}
Nt(["input", "change"]);
customElements.define("xi-slider", dt(
  sa,
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
var la = /* @__PURE__ */ se('<span class="lbl"> </span>'), aa = /* @__PURE__ */ se('<label class="xi-number svelte-1f6ykwb"><!> <input type="number" class="svelte-1f6ykwb"/></label>');
const oa = {
  hash: "svelte-1f6ykwb",
  code: ".xi-number.svelte-1f6ykwb {display:inline-flex;align-items:center;gap:0.5rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}input.svelte-1f6ykwb {width:6em;padding:0.15rem 0.3rem;accent-color:var(--xi-accent, #3b82f6);}"
};
function ua(e, t) {
  nt(t, !0), ct(e, oa);
  let n = I(t, "value", 15, 0), r = I(t, "min", 7), i = I(t, "max", 7), s = I(t, "step", 7, 1), l = I(t, "label", 7, ""), a = I(t, "disabled", 7, !1);
  const o = t.$$host, u = (w) => o.dispatchEvent(new CustomEvent(w, { detail: { value: n() }, bubbles: !0, composed: !0 })), h = (w) => w.target.value === "" ? null : Number(w.target.value);
  function m(w) {
    n(h(w)), u("input");
  }
  function d(w) {
    n(h(w)), u("change");
  }
  var v = {
    get value() {
      return n();
    },
    set value(w = 0) {
      n(w), N();
    },
    get min() {
      return r();
    },
    set min(w) {
      r(w), N();
    },
    get max() {
      return i();
    },
    set max(w) {
      i(w), N();
    },
    get step() {
      return s();
    },
    set step(w = 1) {
      s(w), N();
    },
    get label() {
      return l();
    },
    set label(w = "") {
      l(w), N();
    },
    get disabled() {
      return a();
    },
    set disabled(w = !1) {
      a(w), N();
    }
  }, g = aa(), p = X(g);
  {
    var c = (w) => {
      var _ = la(), E = X(_, !0);
      z(_), ge(() => Re(E, l())), ee(w, _);
    };
    bn(p, (w) => {
      l() && w(c);
    });
  }
  var b = Ee(p, 2);
  return Xn(b), z(g), ge(() => {
    Qe(b, "min", r()), Qe(b, "max", i()), Qe(b, "step", s()), Ir(b, n()), b.disabled = a();
  }), Q("input", b, m), Q("change", b, d), ee(e, g), rt(v);
}
Nt(["input", "change"]);
customElements.define("xi-number", dt(
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
var fa = /* @__PURE__ */ se('<span class="lbl"> </span>'), ca = /* @__PURE__ */ se('<label class="xi-toggle svelte-141rfru"><input type="checkbox" class="svelte-141rfru"/> <!></label>');
const da = {
  hash: "svelte-141rfru",
  code: ".xi-toggle.svelte-141rfru {display:inline-flex;align-items:center;gap:0.4rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);cursor:pointer;}input.svelte-141rfru {accent-color:var(--xi-accent, #3b82f6);}"
};
function ha(e, t) {
  nt(t, !0), ct(e, da);
  let n = I(t, "value", 15, !1), r = I(t, "label", 7, ""), i = I(t, "disabled", 7, !1);
  const s = t.$$host;
  function l(d) {
    n(d.target.checked), s.dispatchEvent(new CustomEvent("change", { detail: { value: n() }, bubbles: !0, composed: !0 }));
  }
  var a = {
    get value() {
      return n();
    },
    set value(d = !1) {
      n(d), N();
    },
    get label() {
      return r();
    },
    set label(d = "") {
      r(d), N();
    },
    get disabled() {
      return i();
    },
    set disabled(d = !1) {
      i(d), N();
    }
  }, o = ca(), u = X(o);
  Xn(u);
  var h = Ee(u, 2);
  {
    var m = (d) => {
      var v = fa(), g = X(v, !0);
      z(v), ge(() => Re(g, r())), ee(d, v);
    };
    bn(h, (d) => {
      r() && d(m);
    });
  }
  return z(o), ge(() => {
    hs(u, n()), u.disabled = i();
  }), Q("change", u, l), ee(e, o), rt(a);
}
Nt(["change"]);
customElements.define("xi-toggle", dt(
  ha,
  {
    value: { reflect: !0, type: "Boolean" },
    label: {},
    disabled: {}
  },
  [],
  [],
  { mode: "open" }
));
function ps(e) {
  let t = e;
  if (typeof e == "string")
    try {
      t = JSON.parse(e);
    } catch {
      t = [];
    }
  return Array.isArray(t) ? t.map((n) => n && typeof n == "object" ? { value: n.value, label: n.label ?? String(n.value) } : { value: n, label: String(n) }) : [];
}
var va = /* @__PURE__ */ se('<span class="lbl"> </span>'), pa = /* @__PURE__ */ se('<label class="opt svelte-mtcxej"><input type="radio" class="svelte-mtcxej"/> <span> </span></label>'), ga = /* @__PURE__ */ se('<div class="xi-radio svelte-mtcxej" role="radiogroup"><!> <!></div>');
const ma = {
  hash: "svelte-mtcxej",
  code: ".xi-radio.svelte-mtcxej {display:inline-flex;align-items:center;gap:0.75rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}.opt.svelte-mtcxej {display:inline-flex;align-items:center;gap:0.25rem;cursor:pointer;}input.svelte-mtcxej {accent-color:var(--xi-accent, #3b82f6);}"
};
function _a(e, t) {
  nt(t, !0), ct(e, ma);
  let n = I(t, "value", 15, ""), r = I(t, "options", 23, () => []), i = I(t, "label", 7, ""), s = I(t, "disabled", 7, !1), l = I(t, "name", 7, "xi-radio");
  const a = t.$$host, o = /* @__PURE__ */ Mi(() => ps(r()));
  function u(p) {
    n(p), a.dispatchEvent(new CustomEvent("change", { detail: { value: n() }, bubbles: !0, composed: !0 }));
  }
  var h = {
    get value() {
      return n();
    },
    set value(p = "") {
      n(p), N();
    },
    get options() {
      return r();
    },
    set options(p = []) {
      r(p), N();
    },
    get label() {
      return i();
    },
    set label(p = "") {
      i(p), N();
    },
    get disabled() {
      return s();
    },
    set disabled(p = !1) {
      s(p), N();
    },
    get name() {
      return l();
    },
    set name(p = "xi-radio") {
      l(p), N();
    }
  }, m = ga(), d = X(m);
  {
    var v = (p) => {
      var c = va(), b = X(c, !0);
      z(c), ge(() => Re(b, i())), ee(p, c);
    };
    bn(d, (p) => {
      i() && p(v);
    });
  }
  var g = Ee(d, 2);
  return cs(g, 17, () => O(o), fs, (p, c) => {
    var b = pa(), w = X(b);
    Xn(w);
    var _ = Ee(w, 2), E = X(_, !0);
    z(_), z(b), ge(() => {
      Qe(w, "name", l()), Ir(w, O(c).value), hs(w, O(c).value === n()), w.disabled = s(), Re(E, O(c).label);
    }), Q("change", w, () => u(O(c).value)), ee(p, b);
  }), z(m), ee(e, m), rt(h);
}
Nt(["change"]);
customElements.define("xi-radio", dt(
  _a,
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
var ba = /* @__PURE__ */ se('<span class="lbl"> </span>'), ya = /* @__PURE__ */ se("<option> </option>"), wa = /* @__PURE__ */ se('<label class="xi-dropdown svelte-1wd9iqr"><!> <select class="svelte-1wd9iqr"></select></label>');
const xa = {
  hash: "svelte-1wd9iqr",
  code: ".xi-dropdown.svelte-1wd9iqr {display:inline-flex;align-items:center;gap:0.5rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}select.svelte-1wd9iqr {padding:0.15rem 0.3rem;}"
};
function Ea(e, t) {
  nt(t, !0), ct(e, xa);
  let n = I(t, "value", 15, ""), r = I(t, "options", 23, () => []), i = I(t, "label", 7, ""), s = I(t, "disabled", 7, !1);
  const l = t.$$host, a = /* @__PURE__ */ Mi(() => ps(r()));
  function o(p) {
    n(p.target.value), l.dispatchEvent(new CustomEvent("change", { detail: { value: n() }, bubbles: !0, composed: !0 }));
  }
  var u = {
    get value() {
      return n();
    },
    set value(p = "") {
      n(p), N();
    },
    get options() {
      return r();
    },
    set options(p = []) {
      r(p), N();
    },
    get label() {
      return i();
    },
    set label(p = "") {
      i(p), N();
    },
    get disabled() {
      return s();
    },
    set disabled(p = !1) {
      s(p), N();
    }
  }, h = wa(), m = X(h);
  {
    var d = (p) => {
      var c = ba(), b = X(c, !0);
      z(c), ge(() => Re(b, i())), ee(p, c);
    };
    bn(m, (p) => {
      i() && p(d);
    });
  }
  var v = Ee(m, 2);
  cs(v, 21, () => O(a), fs, (p, c) => {
    var b = ya(), w = X(b, !0);
    z(b);
    var _ = {};
    ge(() => {
      Jl(b, O(c).value === n()), Re(w, O(c).label), _ !== (_ = O(c).value) && (b.value = (b.__value = O(c).value) ?? "");
    }), ee(p, b);
  }), z(v);
  var g;
  return Xl(v), z(h), ge(() => {
    v.disabled = s(), g !== (g = n()) && (v.value = (v.__value = n()) ?? "", ds(v, n()));
  }), Q("change", v, o), ee(e, h), rt(u);
}
Nt(["change"]);
customElements.define("xi-dropdown", dt(
  Ea,
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
var $a = /* @__PURE__ */ se('<div class="xi-trace svelte-1m1z2sd"><span class="lbl"> </span> <canvas class="svelte-1m1z2sd"></canvas> <span class="val svelte-1m1z2sd"> </span></div>');
const ka = {
  hash: "svelte-1m1z2sd",
  code: ".xi-trace.svelte-1m1z2sd {display:inline-flex;align-items:center;gap:0.5rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}canvas.svelte-1m1z2sd {width:120px;height:28px;display:block;}.val.svelte-1m1z2sd {min-width:3em;text-align:right;font-variant-numeric:tabular-nums;}"
};
function Ta(e, t) {
  nt(t, !0), ct(e, ka);
  let n = I(t, "key", 7, ""), r = I(t, "label", 7, ""), i = I(t, "max", 7, 60);
  const s = t.$$host;
  let l, a = /* @__PURE__ */ Ie(null), o = /* @__PURE__ */ Ie(pt([]));
  function u() {
    if (!l) return;
    const _ = l.getContext && l.getContext("2d");
    if (!_) return;
    const E = l.width = l.clientWidth || 120, y = l.height = l.clientHeight || 28;
    if (_.clearRect(0, 0, E, y), O(o).length < 2) return;
    const $ = Math.min(...O(o)), R = Math.max(...O(o)), Y = R - $ || 1;
    _.beginPath(), O(o).forEach((L, j) => {
      const q = j / (O(o).length - 1) * (E - 2) + 1, K = y - 2 - (L - $) / Y * (y - 4);
      j ? _.lineTo(q, K) : _.moveTo(q, K);
    }), _.strokeStyle = getComputedStyle(s).getPropertyValue("--xi-accent") || "#3b82f6", _.lineWidth = 1.5, _.stroke();
  }
  function h(_) {
    const E = _ && _[n()];
    E && (Me(a, E.value, !0), typeof E.value == "number" && Number.isFinite(E.value) && (Me(o, [...O(o), E.value].slice(-i()), !0), u()), s.dispatchEvent(new CustomEvent("sample", { detail: { value: E.value }, bubbles: !0, composed: !0 })));
  }
  Cr(() => {
    s.update = h, Object.defineProperty(s, "latest", { get: () => O(a), configurable: !0 }), Object.defineProperty(s, "history", { get: () => O(o).slice(), configurable: !0 }), u();
  });
  const m = (_) => _ == null ? "—" : typeof _ == "number" ? Number.isInteger(_) ? _ : _.toFixed(3) : String(_);
  var d = {
    get key() {
      return n();
    },
    set key(_ = "") {
      n(_), N();
    },
    get label() {
      return r();
    },
    set label(_ = "") {
      r(_), N();
    },
    get max() {
      return i();
    },
    set max(_ = 60) {
      i(_), N();
    }
  }, v = $a(), g = X(v), p = X(g, !0);
  z(g);
  var c = Ee(g, 2);
  Lr(c, (_) => l = _, () => l);
  var b = Ee(c, 2), w = X(b, !0);
  return z(b), z(v), ge(
    (_) => {
      Re(p, r() || n()), Re(w, _);
    },
    [() => m(O(a))]
  ), ee(e, v), rt(d);
}
customElements.define("xi-trace", dt(Ta, { key: {}, label: {}, max: { type: "Number" } }, [], [], { mode: "open" }));
function gs() {
  return { scale: 1, panX: 0, panY: 0, imgW: 0, imgH: 0, viewW: 0, viewH: 0 };
}
function Pr(e, t, n) {
  return { x: (t - e.panX) / e.scale, y: (n - e.panY) / e.scale };
}
function Sa(e, t, n) {
  return { x: e.panX + t * e.scale, y: e.panY + n * e.scale };
}
const Ca = 0.05, Ma = 64, Aa = (e) => Math.max(Ca, Math.min(Ma, e));
function hr(e) {
  return !e.imgW || !e.imgH || !e.viewW || !e.viewH || (e.scale = Math.min(e.viewW / e.imgW, e.viewH / e.imgH) * 0.95, e.panX = (e.viewW - e.imgW * e.scale) / 2, e.panY = (e.viewH - e.imgH * e.scale) / 2), e;
}
function Oa(e) {
  return e.scale = 1, e.panX = (e.viewW - e.imgW) / 2, e.panY = (e.viewH - e.imgH) / 2, e;
}
function ms(e, t, n, r) {
  const { x: i, y: s } = Pr(e, t, n);
  return e.scale = Aa(e.scale * r), e.panX = t - i * e.scale, e.panY = n - s * e.scale, e;
}
function Na(e, t, n) {
  return e.panX += t, e.panY += n, e;
}
var Ra = /* @__PURE__ */ se('<canvas class="svelte-1yjweo0"></canvas>');
const Ia = {
  hash: "svelte-1yjweo0",
  code: ":host {display:block;width:100%;height:100%;}canvas.svelte-1yjweo0 {width:100%;height:100%;display:block;background:var(--xi-viewer-bg, #111);cursor:grab;touch-action:none;image-rendering:pixelated;}canvas.svelte-1yjweo0:active {cursor:grabbing;}"
};
function Da(e, t) {
  nt(t, !0), ct(e, Ia);
  const n = t.$$host;
  let r;
  const i = gs();
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
  function h(y) {
    return !!y && typeof y != "string" && !("dataUrl" in y) && (typeof HTMLImageElement < "u" && y instanceof HTMLImageElement || typeof HTMLCanvasElement < "u" && y instanceof HTMLCanvasElement || typeof OffscreenCanvas < "u" && y instanceof OffscreenCanvas || typeof ImageBitmap < "u" && y instanceof ImageBitmap);
  }
  function m(y) {
    if (h(y)) {
      d(y);
      return;
    }
    const $ = new Image();
    $.onload = () => d($), $.src = typeof y == "string" ? y : y.dataUrl;
  }
  function d(y) {
    const $ = !i.imgW;
    s = y, i.imgW = y.naturalWidth || y.width, i.imgH = y.naturalHeight || y.height, l = document.createElement("canvas"), l.width = i.imgW, l.height = i.imgH, l.getContext("2d").drawImage(y, 0, 0), $ && hr(i), a();
  }
  function v(y) {
    if (!s) return;
    y.preventDefault();
    const $ = r.getBoundingClientRect();
    ms(i, y.clientX - $.left, y.clientY - $.top, y.deltaY < 0 ? 1.15 : 1 / 1.15), a(), u("viewchange", { scale: i.scale });
  }
  let g = null, p = !1;
  function c(y) {
    var $;
    s && (g = { x: y.clientX, y: y.clientY }, p = !1, ($ = r.setPointerCapture) == null || $.call(r, y.pointerId));
  }
  function b(y) {
    if (!g) return;
    const $ = y.clientX - g.x, R = y.clientY - g.y;
    ($ || R) && (p = !0), Na(i, $, R), g = { x: y.clientX, y: y.clientY }, a();
  }
  function w(y) {
    g && !p && _(y), g = null;
  }
  function _(y) {
    if (!s || !l) return;
    const $ = r.getBoundingClientRect(), R = Pr(i, y.clientX - $.left, y.clientY - $.top), Y = Math.floor(R.x), L = Math.floor(R.y);
    let j = null;
    if (Y >= 0 && L >= 0 && Y < i.imgW && L < i.imgH) {
      const q = l.getContext("2d").getImageData(Y, L, 1, 1).data;
      j = [q[0], q[1], q[2]];
    }
    u("pixelpick", { x: Y, y: L, rgb: j });
  }
  Cr(() => {
    n.setFrame = m, n.fit = () => {
      hr(i), a(), u("viewchange", { scale: i.scale });
    }, n.oneToOne = () => {
      Oa(i), a(), u("viewchange", { scale: i.scale });
    }, o();
    const y = new ResizeObserver(o);
    return y.observe(r), () => y.disconnect();
  });
  var E = Ra();
  Lr(E, (y) => r = y, () => r), as("wheel", E, v), Q("pointerdown", E, c), Q("pointermove", E, b), Q("pointerup", E, w), ee(e, E), rt();
}
Nt(["pointerdown", "pointermove", "pointerup"]);
customElements.define("xi-image-viewer", dt(Da, {}, [], [], { mode: "open" }));
function La() {
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
function Pa() {
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
function Ha() {
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
const vr = { point: La, rect: Pa, polygon: Ha };
function no(e, t) {
  vr[e] = t;
}
function ni(e) {
  return vr[e] ? vr[e]() : null;
}
var ja = /* @__PURE__ */ se('<div class="xi-editor svelte-1qrk1oj"><div class="bar svelte-1qrk1oj"><span class="lbl"> </span> <span class="spacer svelte-1qrk1oj"></span> <button class="cancel svelte-1qrk1oj">Cancel</button> <button class="commit svelte-1qrk1oj">Commit</button></div> <canvas class="svelte-1qrk1oj"></canvas></div>');
const Fa = {
  hash: "svelte-1qrk1oj",
  code: ":host {display:block;width:100%;height:100%;}.xi-editor.svelte-1qrk1oj {display:flex;flex-direction:column;height:100%;}.bar.svelte-1qrk1oj {display:flex;align-items:center;gap:0.5rem;padding:0.35rem 0.5rem;font:13px system-ui, sans-serif;background:var(--xi-bar-bg, #1f2937);color:#e5e7eb;}.spacer.svelte-1qrk1oj {flex:1;}button.svelte-1qrk1oj {padding:0.2rem 0.7rem;border:0;border-radius:4px;cursor:pointer;font:inherit;}.commit.svelte-1qrk1oj {background:var(--xi-accent, #6366f1);color:#fff;}.cancel.svelte-1qrk1oj {background:#374151;color:#e5e7eb;}canvas.svelte-1qrk1oj {flex:1;width:100%;display:block;background:var(--xi-viewer-bg, #111);cursor:crosshair;touch-action:none;image-rendering:pixelated;}"
};
function Wa(e, t) {
  nt(t, !0), ct(e, Fa);
  let n = I(t, "tool", 7, "rect"), r = I(t, "label", 7, "");
  const i = t.$$host;
  let s;
  const l = gs();
  let a = null, o = ni(n());
  const u = (x) => Sa(l, x.x, x.y);
  function h() {
    if (!s) return;
    const x = s.getContext("2d");
    x && (x.imageSmoothingEnabled = !1, x.setTransform(1, 0, 0, 1, 0, 0), x.clearRect(0, 0, s.width, s.height), a && (x.setTransform(l.scale, 0, 0, l.scale, l.panX, l.panY), x.drawImage(a, 0, 0), x.setTransform(1, 0, 0, 1, 0, 0)), o && o.draw(x, u));
  }
  function m() {
    if (!s) return;
    const x = s.getBoundingClientRect();
    s.width = Math.max(1, Math.round(x.width)), s.height = Math.max(1, Math.round(x.height)), l.viewW = s.width, l.viewH = s.height, h();
  }
  function d(x) {
    return !!x && typeof x != "string" && !("dataUrl" in x) && (typeof HTMLImageElement < "u" && x instanceof HTMLImageElement || typeof HTMLCanvasElement < "u" && x instanceof HTMLCanvasElement || typeof OffscreenCanvas < "u" && x instanceof OffscreenCanvas || typeof ImageBitmap < "u" && x instanceof ImageBitmap);
  }
  function v(x) {
    if (d(x)) {
      g(x);
      return;
    }
    const le = new Image();
    le.onload = () => g(le), le.src = typeof x == "string" ? x : x.dataUrl;
  }
  function g(x) {
    const le = !l.imgW;
    a = x, l.imgW = x.naturalWidth || x.width, l.imgH = x.naturalHeight || x.height, le && hr(l), h();
  }
  function p(x) {
    o = ni(x) || o, h();
  }
  const c = (x) => {
    const le = s.getBoundingClientRect();
    return Pr(l, x.clientX - le.left, x.clientY - le.top);
  };
  function b(x) {
    o && (o.onDown(c(x)), h());
  }
  function w(x) {
    o && x.buttons && (o.onMove(c(x)), h());
  }
  function _(x) {
    o && (o.onUp(c(x)), h());
  }
  function E(x) {
    o && (o.onDbl(c(x)), h());
  }
  function y(x) {
    if (!a) return;
    x.preventDefault();
    const le = s.getBoundingClientRect();
    ms(l, x.clientX - le.left, x.clientY - le.top, x.deltaY < 0 ? 1.15 : 1 / 1.15), h();
  }
  function $() {
    !o || !o.done() || i.dispatchEvent(new CustomEvent("commit", {
      detail: { tool: o.type, result: o.result() },
      bubbles: !0,
      composed: !0
    }));
  }
  function R() {
    i.dispatchEvent(new CustomEvent("cancel", { detail: {}, bubbles: !0, composed: !0 }));
  }
  Cr(() => {
    i.setFrame = v, i.setTool = p, i.getResult = () => o && o.done() ? o.result() : null, m();
    const x = new ResizeObserver(m);
    return x.observe(s), () => x.disconnect();
  });
  var Y = {
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
  }, L = ja(), j = X(L), q = X(j), K = X(q, !0);
  z(q);
  var ht = Ee(q, 4), It = Ee(ht, 2);
  z(j);
  var Be = Ee(j, 2);
  return Lr(Be, (x) => s = x, () => s), z(L), ge(() => Re(K, r() || n())), Q("click", ht, R), Q("click", It, $), Q("pointerdown", Be, b), Q("pointermove", Be, w), Q("pointerup", Be, _), Q("dblclick", Be, E), as("wheel", Be, y), ee(e, L), rt(Y);
}
Nt([
  "click",
  "pointerdown",
  "pointermove",
  "pointerup",
  "dblclick"
]);
customElements.define("xi-image-editor", dt(Wa, { tool: {}, label: {} }, [], [], { mode: "open" }));
const Ya = 4003;
function ri(e, t) {
  const n = e && typeof e.code == "number" ? e.code : null, r = e && e.reason || "";
  return t && t.busy ? { busy: !0, code: n, reason: "single-client-busy" } : n === Ya || /single-client-busy/i.test(r) ? { busy: !0, code: n, reason: r || "single-client-busy" } : { busy: !1, code: n, reason: r };
}
class ro {
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
      typeof i.on == "function" && i.on("unexpected-response", (u, h) => {
        const m = h && h.headers && h.headers["x-xi-reason"];
        l = {
          statusCode: h && h.statusCode,
          reason: m,
          busy: h && h.statusCode === 503 && m === "single-client-busy"
        };
      }), i.onmessage = (u) => this._onMessage(u);
      let a = !1;
      const o = (u) => {
        if (a) return;
        a = !0;
        const h = ri(u, l);
        this._emit("close", h);
        const m = new Error(h.busy ? "single-client-busy: another client owns the backend" : "connection failed before open");
        m.busy = h.busy, m.reason = h.reason, m.code = h.code, r(m);
      };
      i.onerror = () => {
        for (const { reject: u } of this._pending.values()) u(new Error("socket error"));
        this._pending.clear(), s || o(null);
      }, i.onclose = (u) => {
        for (const { reject: h } of this._pending.values()) h(new Error("socket closed"));
        if (this._pending.clear(), !s) {
          o(u);
          return;
        }
        this._emit("close", ri(u, l));
      }, i.onopen = async () => {
        s = !0, this._emit("open", { url: this.url });
        try {
          if (t.checkVersion) {
            const u = await this.cmd("version"), h = u && u.version;
            if (!(typeof t.checkVersion == "function" ? t.checkVersion(u) : t.checkVersion instanceof RegExp ? t.checkVersion.test(h) : h === t.checkVersion)) {
              r(new Error(`backend version mismatch: got ${h}`)), i.close();
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
const qa = {
  slider: "xi-slider",
  number: "xi-number",
  toggle: "xi-toggle",
  radio: "xi-radio",
  dropdown: "xi-dropdown"
};
function Ba(e, { section: t = "Config", tag: n = "control" } = {}) {
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
async function io(e, t) {
  const { client: n, instance: r, sectionFilter: i } = t, s = e.ownerDocument || globalThis.document, l = await n.getInstanceDef(r) || {}, a = { ...l }, o = t.descriptor && t.descriptor.length ? t.descriptor : Ba(l), u = [];
  e.innerHTML = "";
  for (const h of o) {
    if (i && !i(h)) continue;
    const m = s.createElement("section");
    if (m.className = "xi-section", m.dataset.tag = h.tag || "control", h.section) {
      const d = s.createElement("h3");
      d.className = "xi-section-title", d.textContent = h.section, m.appendChild(d);
    }
    for (const d of h.controls || []) {
      const v = qa[d.type] || "xi-number", g = s.createElement(v);
      d.label && g.setAttribute("label", d.label);
      for (const c of ["min", "max", "step"]) d[c] != null && g.setAttribute(c, String(d[c]));
      const p = s.createElement("div");
      p.className = "xi-control", p.appendChild(g), m.appendChild(p), d.options != null && (g.options = d.options), d.key in a && (g.value = a[d.key]), g.addEventListener("change", async (c) => {
        a[d.key] = c.detail.value;
        try {
          await n.setInstanceDef(r, { ...a });
        } catch {
        }
        e.dispatchEvent(new CustomEvent("xi-change", { detail: { key: d.key, value: c.detail.value }, bubbles: !0 }));
      }), u.push({ el: g, key: d.key });
    }
    e.appendChild(m);
  }
  return {
    // Re-read the def from the backend and push values back into the controls.
    async refresh() {
      const h = await n.getInstanceDef(r) || {};
      Object.assign(a, h);
      for (const { el: m, key: d } of u) d in a && (m.value = a[d]);
    },
    destroy() {
      e.innerHTML = "";
    }
  };
}
const za = `
  :host { display:block; height:100%; box-sizing:border-box; background:#1e1e1e;
          border:1px solid #333; border-radius:6px; color:#ddd; overflow:hidden;
          font:13px/1.4 system-ui,sans-serif; }
  .hd { padding:4px 8px; font-size:11px; letter-spacing:.04em; text-transform:uppercase;
        color:#888; border-bottom:1px solid #2a2a2a; }
  .body { padding:8px; height:calc(100% - 24px); box-sizing:border-box; }
`;
function Vn(e, t) {
  return e.attachShadow({ mode: "open" }), e.shadowRoot.innerHTML = `<style>${za}</style>
    <div class="hd">${t || ""}</div><div class="body"></div>`, e.shadowRoot.querySelector(".body");
}
const Xa = (e, t) => e.config && e.config.title || t;
function _s(e) {
  return e == null ? { kind: "none", label: "—", color: "#bbb" } : e <= -99e4 ? { kind: "sys", label: "SYS", color: "#c084fc" } : e > 0 ? { kind: "ok", label: e > 1 ? `OK${e}` : "OK", color: "#3ad17a" } : e < 0 ? { kind: "ng", label: e < -1 ? `NG${-e}` : "NG", color: "#ff5b5b" } : { kind: "na", label: "NA", color: "#ffb454" };
}
class Va extends HTMLElement {
  connectedCallback() {
    this.body = Vn(this, Xa(this, "Verdict")), this.body.style.cssText = "display:flex;flex-direction:column;align-items:center;justify-content:center;gap:6px;text-align:center", this.big = document.createElement("div"), this.big.style.cssText = "font-weight:800;font-size:clamp(20px,7vw,72px);line-height:1", this.sub = document.createElement("div"), this.sub.style.cssText = "font-size:12px;color:#888;max-width:96%;overflow:hidden;text-overflow:ellipsis;white-space:nowrap", this.body.append(this.big, this.sub);
  }
  feed(t) {
    const n = t.result, r = _s(n ? n.code : null);
    this.big.textContent = r.label, this.big.style.color = r.color, this.sub.textContent = n && n.msg ? n.msg : "";
  }
}
class Ua extends HTMLElement {
  connectedCallback() {
    var t, n;
    this.body = Vn(this, ((t = this.config) == null ? void 0 : t.title) || "Throughput"), this.windowSec = ((n = this.config) == null ? void 0 : n.windowSec) || 60, this.stamps = [], this.lastResult = -1, this.lastCompute = null, this.body.style.cssText = "display:flex;flex-direction:column;align-items:center;justify-content:center;gap:4px", this.big = document.createElement("div"), this.big.style.cssText = "font-size:clamp(18px,6vw,44px);font-weight:700;color:#9ad", this.sub = document.createElement("div"), this.sub.style.cssText = "font-size:12px;color:#888", this.body.append(this.big, this.sub), this.timer = setInterval(() => this.render(), 1e3);
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
class Ga extends HTMLElement {
  connectedCallback() {
    var t;
    this.body = Vn(this, ((t = this.config) == null ? void 0 : t.title) || "Yield"), this.ok = 0, this.ng = 0, this.last = -1, this.body.style.cssText = "display:flex;flex-direction:column;align-items:center;justify-content:center;gap:4px", this.big = document.createElement("div"), this.big.style.cssText = "font-size:clamp(18px,6vw,44px);font-weight:700", this.sub = document.createElement("div"), this.sub.style.cssText = "font-size:12px;color:#888", this.body.append(this.big, this.sub);
  }
  feed(t) {
    var s;
    const n = t.result;
    if (n && n.run_id != null && n.run_id !== this.last) {
      this.last = n.run_id;
      const l = _s(n.code);
      l.kind === "ok" ? this.ok++ : l.kind === "ng" ? this.ng++ : l.kind === "na" && (this.na = (this.na || 0) + 1);
    }
    const r = this.ok + this.ng, i = r ? 100 * this.ok / r : 0;
    this.big.textContent = `${i.toFixed(1)}%`, this.big.style.color = i >= (((s = this.config) == null ? void 0 : s.warn) ?? 95) ? "#3ad17a" : "#ffb454", this.sub.textContent = `OK ${this.ok} / NG ${this.ng}` + (this.na ? ` · NA ${this.na}` : "");
  }
}
class Ka extends HTMLElement {
  connectedCallback() {
    var t;
    this.body = Vn(this, ((t = this.config) == null ? void 0 : t.title) || "Dispatch groups"), this.body.style.cssText = "display:flex;flex-direction:column;gap:10px;padding:10px;overflow:auto", this.peak = {}, this.rows = {};
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
        const h = document.createElement("div");
        h.style.cssText = "display:flex;gap:3px;height:18px", l.append(a, h), this.body.appendChild(l), this.rows[r.name] = l = { row: l, name: o, meta: u, bar: h, cells: [] };
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
const bs = {
  verdict: Va,
  throughput: Ua,
  yield: Ga,
  groups: Ka
};
for (const [e, t] of Object.entries(bs)) customElements.define(`xi-card-${e}`, t);
const Hr = (e) => !!(e && e.card), Rt = (e) => !!(e && (e.dir === "row" || e.dir === "col") && Array.isArray(e.children) && e.children.length >= 1), qe = (e) => !!(e && Array.isArray(e.tabs) && e.tabs.length >= 1 && e.tabs.every((t) => t && t.child)), yn = () => ({ type: "verdict", bind: {}, config: { title: "(empty)" } });
function jr(e) {
  const t = e.children.length;
  return (Array.isArray(e.weights) && e.weights.length === t ? e.weights.slice() : Array(t).fill(1)).map((r) => typeof r == "number" && r > 0 ? r : 1);
}
function Ja(e) {
  const t = jr(e), n = t.reduce((r, i) => r + i, 0) || 1;
  return t.map((r) => r / n);
}
function ys(e, t) {
  return qe(e) ? e.tabs[t].child : e.children[t];
}
function Za(e, t, n) {
  if (qe(e)) {
    const i = e.tabs.slice();
    return i[t] = { ...i[t], child: n }, { ...e, tabs: i };
  }
  const r = e.children.slice();
  return r[t] = n, { ...e, children: r };
}
function pr(e, t, n = []) {
  if (Hr(e)) {
    t(e.card, n);
    return;
  }
  Rt(e) ? e.children.forEach((r, i) => pr(r, t, [...n, i])) : qe(e) && e.tabs.forEach((r, i) => pr(r.child, t, [...n, i]));
}
function so(e) {
  let t = 0;
  return pr(e, () => t++), t;
}
function Qa(e, t) {
  let n = e;
  for (const r of t)
    if (Rt(n) || qe(n)) n = ys(n, r);
    else return;
  return n;
}
function ke(e, t, n) {
  if (t.length === 0) return n(e);
  const [r, ...i] = t;
  return Za(e, r, ke(ys(e, r), i, n));
}
function lo(e, t, n, r = yn()) {
  return ke(e, t, (i) => ({ dir: n === "col" ? "col" : "row", children: [i, { card: r }], weights: [1, 1] }));
}
function ao(e, t, n, r = yn()) {
  if (n = n === "col" ? "col" : "row", t.length === 0) return { dir: n, children: [e, { card: r }], weights: [1, 1] };
  const i = t.slice(0, -1), s = t[t.length - 1], l = Qa(e, i);
  return Rt(l) && l.dir === n ? ke(e, i, (a) => {
    const o = a.children.slice();
    o.splice(s + 1, 0, { card: r });
    const u = jr(a);
    return u.splice(s + 1, 0, u[s]), { ...a, children: o, weights: u };
  }) : ke(e, t, (a) => ({ dir: n, children: [a, { card: r }], weights: [1, 1] }));
}
function oo(e, t) {
  if (t.length === 0) return { card: yn() };
  const n = t.slice(0, -1), r = t[t.length - 1];
  return ke(e, n, (i) => {
    if (!Rt(i)) return i;
    const s = i.children.filter((a, o) => o !== r), l = jr(i).filter((a, o) => o !== r);
    return s.length === 1 ? s[0] : { ...i, children: s, weights: l };
  });
}
function uo(e, t, n) {
  return ke(e, t, () => ({ card: n }));
}
function fo(e, t, n) {
  return ke(e, t, (r) => Rt(r) ? { ...r, weights: n.slice() } : r);
}
function co(e, t) {
  return ke(e, t, (n) => ({ tabs: [{ name: "Page 1", child: n }, { name: "Page 2", child: { card: yn() } }], active: 0 }));
}
function ho(e, t, n, r = { card: yn() }) {
  return ke(e, t, (i) => qe(i) ? { ...i, tabs: [...i.tabs, { name: n || `Page ${i.tabs.length + 1}`, child: r }], active: i.tabs.length } : i);
}
function vo(e, t, n) {
  return ke(e, t, (r) => {
    if (!qe(r)) return r;
    const i = r.tabs.filter((s, l) => l !== n);
    return i.length === 1 ? i[0].child : { ...r, tabs: i, active: Math.max(0, Math.min(r.active || 0, i.length - 1)) };
  });
}
function po(e, t, n, r) {
  return ke(e, t, (i) => qe(i) ? { ...i, tabs: i.tabs.map((s, l) => l === n ? { ...s, name: r } : s) } : i);
}
function go(e, t, n) {
  return ke(e, t, (r) => qe(r) ? { ...r, active: n } : r);
}
function ii(e, t = "root") {
  return Hr(e) ? e.card.type ? [] : [`${t}: leaf has no card.type`] : Rt(e) ? e.children.flatMap((n, r) => ii(n, `${t}.${r}`)) : qe(e) ? e.tabs.flatMap((n, r) => ii(n.child, `${t}.${n.name || r}`)) : [`${t}: node is not a leaf {card}, split {dir,children}, or tabs {tabs}`];
}
function mo(e, { client: t, dashboard: n, pollStatsMs: r = 200 }) {
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
  function h(c) {
    const b = bs[c.type], w = i.createElement(b ? `xi-card-${c.type}` : "div");
    return b || (w.textContent = `unknown card: ${c.type}`, w.style.cssText = "color:#f88;padding:8px"), w.binding = c.bind || {}, w.config = c.config || {}, w.style.minWidth = "0", w.style.minHeight = "0", w.style.overflow = "hidden", b && a.push(w), w;
  }
  function m(c) {
    let b = Math.min(c.active || 0, c.tabs.length - 1);
    const w = i.createElement("div");
    w.style.cssText = "display:flex;flex-direction:column;min-width:0;min-height:0;width:100%;height:100%";
    const _ = i.createElement("div");
    _.style.cssText = "display:flex;gap:2px;flex:0 0 auto;align-items:center;padding:2px 2px 0;overflow:auto";
    const E = i.createElement("div");
    E.style.cssText = "flex:1 1 0;min-width:0;min-height:0;position:relative";
    const y = [], $ = [], R = () => {
      y.forEach((Y, L) => {
        const j = L === b;
        Y.style.background = j ? "#1e1e1e" : "#121212", Y.style.color = j ? "#ddd" : "#888";
      }), $.forEach((Y, L) => {
        Y.style.display = L === b ? "" : "none";
      });
    };
    return c.tabs.forEach((Y, L) => {
      const j = i.createElement("div");
      j.style.cssText = "padding:4px 11px;cursor:pointer;border-radius:6px 6px 0 0;border:1px solid #333;border-bottom:none;font:12px system-ui,sans-serif", j.textContent = Y.name || `Page ${L + 1}`, j.onclick = () => {
        b = L, R();
      }, y.push(j), _.appendChild(j);
      const q = d(Y.child);
      q.style.cssText += ";position:absolute;inset:0;min-width:0;min-height:0;border:1px solid #333;border-radius:0 6px 6px 6px;overflow:hidden", $.push(q), E.appendChild(q);
    }), R(), w.append(_, E), w;
  }
  function d(c) {
    if (Hr(c)) return h(c.card);
    if (qe(c)) return m(c);
    if (!Rt(c)) {
      const E = i.createElement("div");
      return E.textContent = "bad layout node", E.style.color = "#f88", E;
    }
    const b = c.dir === "col", w = i.createElement("div");
    w.style.cssText = `display:flex;flex-direction:${b ? "column" : "row"};min-width:0;min-height:0;width:100%;height:100%`;
    const _ = Ja(c);
    return c.children.forEach((E, y) => {
      const $ = d(E);
      $.style.flex = `${_[y]} 1 0`, $.style.minWidth = "0", $.style.minHeight = "0", w.appendChild($);
    }), w;
  }
  function v() {
    a = [], e.replaceChildren(), e.style.cssText += ";display:flex;min-width:0;min-height:0";
    const c = n && n.layout;
    if (!c) return;
    const b = d(c);
    b.style.flex = "1 1 0", b.style.minWidth = "0", b.style.minHeight = "0", e.appendChild(b), u();
  }
  const g = [
    t.onEvent((c) => {
      c.name === "run_finished" && c.data ? (typeof c.data.run_id == "number" && (l.run_id = c.data.run_id), typeof c.data.inspect_compute_us == "number" ? l.compute_ms = c.data.inspect_compute_us / 1e3 : typeof c.data.ms == "number" && (l.compute_ms = c.data.ms), u()) : c.name === "run_result" && c.data ? (l.result = c.data, u()) : c.name === "status" && (l.status = c.data, u());
    })
  ], p = setInterval(() => {
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
      g.forEach((c) => c()), clearInterval(p), e.replaceChildren();
    }
  };
}
const _o = [
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
  Ya as BUSY_CLOSE_CODE,
  bs as CARDS,
  qa as CONTROL_TAGS,
  vr as TOOLS,
  _o as XI_COMPONENTS,
  ro as XiClient,
  ao as addSibling,
  ho as addTab,
  so as countLeaves,
  pr as eachLeaf,
  yn as emptyCard,
  Qa as getNode,
  Ba as inferDescriptor,
  Hr as isLeaf,
  Rt as isSplit,
  qe as isTabs,
  ni as makeTool,
  mo as mountDashboard,
  io as mountPanel,
  no as registerTool,
  oo as removePane,
  vo as removeTab,
  po as renameTab,
  go as setActive,
  uo as setCard,
  fo as setWeights,
  lo as splitLeaf,
  ii as validate,
  Ja as weightsOf,
  co as wrapInTabs
};
