var ys = Object.defineProperty;
var Yr = (e) => {
  throw TypeError(e);
};
var ws = (e, t, n) => t in e ? ys(e, t, { enumerable: !0, configurable: !0, writable: !0, value: n }) : e[t] = n;
var W = (e, t, n) => ws(e, typeof t != "symbol" ? t + "" : t, n), Un = (e, t, n) => t.has(e) || Yr("Cannot " + n);
var f = (e, t, n) => (Un(e, t, "read from private field"), n ? n.call(e) : t.get(e)), S = (e, t, n) => t.has(e) ? Yr("Cannot add the same private member more than once") : t instanceof WeakSet ? t.add(e) : t.set(e, n), k = (e, t, n, r) => (Un(e, t, "write to private field"), r ? r.call(e, n) : t.set(e, n), n), A = (e, t, n) => (Un(e, t, "access private method"), n);
var ii;
typeof window < "u" && ((ii = window.__svelte ?? (window.__svelte = {})).v ?? (ii.v = /* @__PURE__ */ new Set())).add("5");
const xs = 1, Es = 2, ai = 4, $s = 8, ks = 16, Ts = 1, Ss = 4, Cs = 8, Ms = 16, As = 2, oi = "[", gr = "[!", qr = "[?", mr = "]", Xt = {}, V = Symbol("uninitialized"), Ns = "http://www.w3.org/1999/xhtml", fi = !1;
var _r = Array.isArray, Os = Array.prototype.indexOf, Mn = Array.prototype.includes, Wn = Array.from, An = Object.keys, Nn = Object.defineProperty, Et = Object.getOwnPropertyDescriptor, Rs = Object.getOwnPropertyDescriptors, Is = Object.prototype, Ds = Array.prototype, ui = Object.getPrototypeOf, zr = Object.isExtensible;
const Ls = () => {
};
function Ps(e) {
  for (var t = 0; t < e.length; t++)
    e[t]();
}
function ci() {
  var e, t, n = new Promise((r, i) => {
    e = r, t = i;
  });
  return { promise: n, resolve: e, reject: t };
}
const Z = 2, Vt = 4, Yn = 8, di = 1 << 24, Ae = 16, Oe = 32, et = 64, Qn = 128, xe = 512, U = 1024, G = 2048, Fe = 4096, ne = 8192, ve = 16384, At = 32768, er = 1 << 25, Ut = 65536, On = 1 << 17, Hs = 1 << 18, Nt = 1 << 19, js = 1 << 20, Pe = 1 << 25, Ct = 65536, Rn = 1 << 21, Ht = 1 << 22, ft = 1 << 23, $t = Symbol("$state"), hi = Symbol("legacy props"), Fs = Symbol(""), En = Symbol("attributes"), Ws = Symbol("class"), Ys = Symbol("style"), Qt = Symbol("text"), vi = Symbol("form reset"), qn = new class extends Error {
  constructor() {
    super(...arguments);
    W(this, "name", "StaleReactionError");
    W(this, "message", "The reaction that called `getAbortSignal()` was re-run or destroyed");
  }
}();
var si;
const pi = (
  // We gotta write it like this because after downleveling the pure comment may end up in the wrong location
  !!((si = globalThis.document) != null && si.contentType) && /* @__PURE__ */ globalThis.document.contentType.includes("xml")
), br = 3, mn = 8;
function qs() {
  throw new Error("https://svelte.dev/e/async_derived_orphan");
}
function zs(e, t, n) {
  throw new Error("https://svelte.dev/e/each_key_duplicate");
}
function Bs(e) {
  throw new Error("https://svelte.dev/e/effect_in_teardown");
}
function Xs() {
  throw new Error("https://svelte.dev/e/effect_in_unowned_derived");
}
function Vs(e) {
  throw new Error("https://svelte.dev/e/effect_orphan");
}
function Us() {
  throw new Error("https://svelte.dev/e/effect_update_depth_exceeded");
}
function Gs() {
  throw new Error("https://svelte.dev/e/hydration_failed");
}
function Ks(e) {
  throw new Error("https://svelte.dev/e/props_invalid_value");
}
function Js() {
  throw new Error("https://svelte.dev/e/state_descriptors_fixed");
}
function Zs() {
  throw new Error("https://svelte.dev/e/state_prototype_fixed");
}
function Qs() {
  throw new Error("https://svelte.dev/e/state_unsafe_mutation");
}
function el() {
  throw new Error("https://svelte.dev/e/svelte_boundary_reset_onerror");
}
function tl() {
  console.warn("https://svelte.dev/e/derived_inert");
}
function zn(e) {
  console.warn("https://svelte.dev/e/hydration_mismatch");
}
function nl() {
  console.warn("https://svelte.dev/e/select_multiple_invalid_value");
}
function rl() {
  console.warn("https://svelte.dev/e/svelte_boundary_reset_noop");
}
let P = !1;
function Je(e) {
  P = e;
}
let H;
function pe(e) {
  if (e === null)
    throw zn(), Xt;
  return H = e;
}
function Bn() {
  return pe(/* @__PURE__ */ it(H));
}
function B(e) {
  if (P) {
    if (/* @__PURE__ */ it(H) !== null)
      throw zn(), Xt;
    H = e;
  }
}
function il(e = 1) {
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
      } else (r === oi || r === gr || // "[1", "[2", etc. for if blocks
      r[0] === "[" && !isNaN(Number(r.slice(1)))) && (t += 1);
    }
    var i = (
      /** @type {TemplateNode} */
      /* @__PURE__ */ it(n)
    );
    e && n.remove(), n = i;
  }
}
function gi(e) {
  if (!e || e.nodeType !== mn)
    throw zn(), Xt;
  return (
    /** @type {Comment} */
    e.data
  );
}
function mi(e) {
  return e === this.v;
}
function sl(e, t) {
  return e != e ? t == t : e !== t || e !== null && typeof e == "object" || typeof e == "function";
}
function _i(e) {
  return !sl(e, this.v);
}
let ll = !1, re = null;
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
      Xi(r);
  }
  return e !== void 0 && (t.x = e), t.i = !0, re = t.p, e ?? /** @type {T} */
  {};
}
function bi() {
  return !0;
}
let vt = [];
function yi() {
  var e = vt;
  vt = [], Ps(e);
}
function Ze(e) {
  if (vt.length === 0 && !ln) {
    var t = vt;
    queueMicrotask(() => {
      t === vt && yi();
    });
  }
  vt.push(e);
}
function al() {
  for (; vt.length > 0; )
    yi();
}
function wi(e) {
  var t = C;
  if (t === null)
    return M.f |= ft, e;
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
const ol = -7169;
function z(e, t) {
  e.f = e.f & ol | t;
}
function yr(e) {
  (e.f & xe) !== 0 || e.deps === null ? z(e, U) : z(e, Fe);
}
function xi(e) {
  if (e !== null)
    for (const t of e)
      (t.f & Z) === 0 || (t.f & Ct) === 0 || (t.f ^= Ct, xi(
        /** @type {Derived} */
        t.deps
      ));
}
function Ei(e, t, n) {
  (e.f & G) !== 0 ? t.add(e) : (e.f & Fe) !== 0 && n.add(e), xi(e.deps), z(e, U);
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
function ul(e) {
  let t = 0, n = Mt(0), r;
  return () => {
    Tr() && (N(n), Mr(() => (t === 0 && (r = Rr(() => e(() => an(n)))), t += 1, () => {
      Ze(() => {
        t -= 1, t === 0 && (r == null || r(), r = void 0, an(n));
      });
    })));
  };
}
var cl = Ut | Nt;
function dl(e, t, n, r) {
  new hl(e, t, n, r);
}
var ce, cn, _e, _t, ae, be, te, de, Xe, bt, lt, jt, dn, hn, Ve, Hn, F, $i, ki, Ti, tr, $n, kn, nr, rr;
class hl {
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
    S(this, Hn, ul(() => (k(this, Ve, Mt(f(this, bt))), () => {
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
        Bn();
        const a = l.data === gr;
        if (l.data.startsWith(qr)) {
          const c = JSON.parse(l.data.slice(qr.length));
          A(this, F, ki).call(this, c);
        } else a ? A(this, F, Ti).call(this) : A(this, F, $i).call(this);
      } else
        A(this, F, tr).call(this);
    }, cl)), P && k(this, ce, H);
  }
  /**
   * Defer an effect inside a pending boundary until the boundary resolves
   * @param {Effect} effect
   */
  defer_effect(t) {
    Ei(t, f(this, dn), f(this, hn));
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
    return f(this, Hn).call(this), N(
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
ce = new WeakMap(), cn = new WeakMap(), _e = new WeakMap(), _t = new WeakMap(), ae = new WeakMap(), be = new WeakMap(), te = new WeakMap(), de = new WeakMap(), Xe = new WeakMap(), bt = new WeakMap(), lt = new WeakMap(), jt = new WeakMap(), dn = new WeakMap(), hn = new WeakMap(), Ve = new WeakMap(), Hn = new WeakMap(), F = new WeakSet(), $i = function() {
  try {
    k(this, be, we(() => f(this, _t).call(this, f(this, ce))));
  } catch (t) {
    this.error(t);
  }
}, /**
 * @param {unknown} error The deserialized error from the server's hydration comment
 */
ki = function(t) {
  const n = f(this, _e).failed;
  n && k(this, de, we(() => {
    n(
      f(this, ce),
      () => t,
      () => () => {
      }
    );
  }));
}, Ti = function() {
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
      Or(f(this, be), t);
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
    return ut.ensure(), t();
  } catch (s) {
    return wi(s), null;
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
  ), il(), pe(In()));
  var n = f(this, _e).onerror;
  let r = f(this, _e).failed;
  var i = !1, s = !1;
  const l = () => {
    if (i) {
      rl();
      return;
    }
    i = !0, s && el(), f(this, de) !== null && Tt(f(this, de), () => {
      k(this, de, null);
    }), A(this, F, kn).call(this, () => {
      A(this, F, tr).call(this);
    });
  }, a = (o) => {
    try {
      s = !0, n == null || n(o, l), s = !1;
    } catch (c) {
      ot(c, f(this, ae) && f(this, ae).parent);
    }
    r && k(this, de, A(this, F, kn).call(this, () => {
      try {
        return we(() => {
          var c = (
            /** @type {Effect} */
            C
          );
          c.b = this, c.f |= Qn, r(
            f(this, ce),
            () => o,
            () => l
          );
        });
      } catch (c) {
        return ot(
          c,
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
    } catch (c) {
      ot(c, f(this, ae) && f(this, ae).parent);
      return;
    }
    o !== null && typeof o == "object" && typeof /** @type {any} */
    o.then == "function" ? o.then(
      a,
      /** @param {unknown} e */
      (c) => ot(c, f(this, ae) && f(this, ae).parent)
    ) : a(o);
  });
};
function vl(e, t, n, r) {
  const i = on;
  var s = e.filter((v) => !v.settled), l = t.map(i);
  if (n.length === 0 && s.length === 0) {
    r(l);
    return;
  }
  var a = (
    /** @type {Effect} */
    C
  ), o = pl(), c = s.length === 1 ? s[0].promise : s.length > 1 ? Promise.all(s.map((v) => v.promise)) : null;
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
  var _ = Si();
  if (n.length === 0) {
    c.then(() => h([])).finally(_);
    return;
  }
  function d() {
    Promise.all(n.map((v) => /* @__PURE__ */ gl(v))).then(h).catch((v) => ot(v, a)).finally(_);
  }
  c ? c.then(() => {
    o(), d(), Dn();
  }) : d();
}
function pl() {
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
function Si() {
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
  return C !== null && (C.f |= Nt), {
    ctx: re,
    deps: null,
    effects: null,
    equals: mi,
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
function gl(e, t, n) {
  let r = (
    /** @type {Effect | null} */
    C
  );
  r === null && qs();
  var i = (
    /** @type {Promise<V>} */
    /** @type {unknown} */
    void 0
  ), s = Mt(
    /** @type {V} */
    V
  ), l = !M, a = /* @__PURE__ */ new Set();
  return Ml(() => {
    var v, g;
    var o = (
      /** @type {Effect} */
      C
    ), c = ci();
    i = c.promise;
    try {
      Promise.resolve(e()).then(c.resolve, (p) => {
        p !== qn && c.reject(p);
      }).finally(Dn);
    } catch (p) {
      c.reject(p), Dn();
    }
    var h = (
      /** @type {Batch} */
      T
    );
    if (l) {
      if ((o.f & At) !== 0)
        var _ = Si();
      if (
        // boundary can be null if the async derived is inside an $effect.root not connected to the component render tree
        (v = r.b) != null && v.is_rendered()
      )
        (g = h.async_deriveds.get(o)) == null || g.reject(en);
      else
        for (const p of a.values())
          p.reject(en);
      a.add(c), h.async_deriveds.set(o, c);
    }
    const d = (p, u = void 0) => {
      _ == null || _(), a.delete(c), u !== en && (h.activate(), u ? (s.f |= ft, Kt(s, u)) : ((s.f & ft) !== 0 && (s.f ^= ft), Kt(s, p)), h.deactivate());
    };
    c.promise.then(d, (p) => d(null, p || "unknown"));
  }), Sr(() => {
    for (const o of a)
      o.reject(en);
  }), new Promise((o) => {
    function c(h) {
      function _() {
        h === i ? o(s) : c(i);
      }
      h.then(_, _);
    }
    c(i);
  });
}
// @__NO_SIDE_EFFECTS__
function Ci(e) {
  const t = /* @__PURE__ */ on(e);
  return Zi(t), t;
}
// @__NO_SIDE_EFFECTS__
function Mi(e) {
  const t = /* @__PURE__ */ on(e);
  return t.equals = _i, t;
}
function ml(e) {
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
    return tl(), e.v;
  We(r);
  try {
    e.f &= ~Ct, ml(e), t = ns(e);
  } finally {
    We(n);
  }
  return t;
}
function Ai(e) {
  var t = wr(e);
  if (!e.equals(t) && (e.wv = es(), (!(T != null && T.is_fork) || e.deps === null) && (T !== null ? (T.capture(e, t, !0), sn == null || sn.capture(e, t, !0)) : e.v = t, e.deps === null))) {
    z(e, U);
    return;
  }
  tt || (J !== null ? (Tr() || T != null && T.is_fork) && J.set(e, t) : yr(e));
}
function _l(e) {
  var t, n;
  if (e.effects !== null)
    for (const r of e.effects)
      (r.teardown || r.ac) && ((t = r.teardown) == null || t.call(r), (n = r.ac) == null || n.abort(qn), r.fn !== null && (r.teardown = Ls), r.ac = null, un(r, 0), Nr(r));
}
function Ni(e) {
  if (e.effects !== null)
    for (const t of e.effects)
      t.teardown && t.fn !== null && Jt(t);
}
let Gn = null, Dt = null, T = null, sn = null, J = null, ir = null, ln = !1, Kn = !1, Pt = null, Tn = null;
var Br = 0;
let bl = 1;
var Ft, at, yt, Wt, Yt, qt, Ue, zt, oe, vn, Ge, Se, De, Bt, wt, D, sr, tn, lr, Oi, Ri, Lt, yl, nn;
const jn = class jn {
  constructor() {
    S(this, D);
    W(this, "id", bl++);
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
    S(this, zt, null);
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
    S(this, Bt, /* @__PURE__ */ new Set());
    W(this, "is_fork", !1);
    S(this, wt, !1);
    Dt === null ? Gn = Dt = this : (k(Dt, yt, this), k(this, at, Dt)), Dt = this;
  }
  /**
   * Add an effect to the #skipped_branches map and reset its children
   * @param {Effect} effect
   */
  skip_effect(t) {
    f(this, De).has(t) || f(this, De).set(t, { d: [], m: [] }), f(this, Bt).delete(t);
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
        z(i, G), n(i);
      for (i of r.m)
        z(i, Fe), n(i);
    }
    f(this, Bt).add(t);
  }
  /**
   * Associate a change to a given source with the current
   * batch, noting its previous and current values
   * @param {Value} source
   * @param {any} value
   * @param {boolean} [is_derived]
   */
  capture(t, n, r = !1) {
    t.v !== V && !this.previous.has(t) && this.previous.set(t, t.v), (t.f & ft) === 0 && (this.current.set(t, [n, r]), J == null || J.set(t, n)), this.is_fork || (t.v = n);
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
      Br = 0, ir = null, Pt = null, Tn = null, Kn = !1, T = null, J = null, kt.clear();
    }
  }
  discard() {
    var t;
    for (const n of f(this, Yt)) n(this);
    f(this, Yt).clear();
    for (const n of this.async_deriveds.values())
      n.reject(en);
    A(this, D, nn).call(this), (t = f(this, zt)) == null || t.resolve();
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
    return (f(this, zt) ?? k(this, zt, ci())).promise;
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
    if (ir = t, (i = t.b) != null && i.is_pending && (t.f & (Vt | Yn | di)) !== 0 && (t.f & At) === 0) {
      t.b.defer_effect(t);
      return;
    }
    for (var n = t; n.parent !== null; ) {
      n = n.parent;
      var r = n.f;
      if (Pt !== null && n === C && (M === null || (M.f & Z) === 0))
        return;
      if ((r & (et | Oe)) !== 0) {
        if ((r & U) === 0)
          return;
        n.f ^= U;
      }
    }
    f(this, oe).push(n);
  }
};
Ft = new WeakMap(), at = new WeakMap(), yt = new WeakMap(), Wt = new WeakMap(), Yt = new WeakMap(), qt = new WeakMap(), Ue = new WeakMap(), zt = new WeakMap(), oe = new WeakMap(), vn = new WeakMap(), Ge = new WeakMap(), Se = new WeakMap(), De = new WeakMap(), Bt = new WeakMap(), wt = new WeakMap(), D = new WeakSet(), sr = function() {
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
  var o, c, h, _;
  k(this, Ft, !0), Br++ > 1e3 && (A(this, D, nn).call(this), wl());
  for (const d of f(this, Ge))
    f(this, Se).delete(d), z(d, G), this.schedule(d);
  for (const d of f(this, Se))
    z(d, Fe), this.schedule(d);
  const t = f(this, oe);
  k(this, oe, []), this.apply();
  var n = Pt = [], r = [], i = Tn = [];
  for (const d of t)
    try {
      A(this, D, lr).call(this, d, n, r);
    } catch (v) {
      throw Li(d), A(this, D, sr).call(this) || this.discard(), v;
    }
  if (T = null, i.length > 0) {
    var s = jn.ensure();
    for (const d of i)
      s.schedule(d);
  }
  if (Pt = null, Tn = null, A(this, D, sr).call(this)) {
    A(this, D, Lt).call(this, r), A(this, D, Lt).call(this, n);
    for (const [d, v] of f(this, De))
      Di(d, v);
    i.length > 0 && /** @type {unknown} */
    A(o = T, D, tn).call(o);
    return;
  }
  const l = A(this, D, Oi).call(this);
  if (l) {
    A(this, D, Lt).call(this, r), A(this, D, Lt).call(this, n), A(c = l, D, Ri).call(c, this);
    return;
  }
  f(this, Ge).clear(), f(this, Se).clear();
  for (const d of f(this, Wt)) d(this);
  f(this, Wt).clear(), sn = this, Xr(r), Xr(n), sn = null, (h = f(this, zt)) == null || h.resolve();
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
  a !== null && A(_ = a, D, tn).call(_);
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
    var s = i.f, l = (s & (Oe | et)) !== 0, a = l && (s & U) !== 0, o = a || (s & ne) !== 0 || f(this, De).has(i);
    if (!o && i.fn !== null) {
      l ? i.f ^= U : (s & Vt) !== 0 ? n.push(i) : _n(i) && ((s & Ae) !== 0 && f(this, Se).add(i), Jt(i));
      var c = i.first;
      if (c !== null) {
        i = c;
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
}, Oi = function() {
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
Ri = function(t) {
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
          l & (Ht | Ae) && !this.async_deriveds.has(a) && (f(this, Se).delete(a), z(a, G), this.schedule(a));
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
    Ei(t[n], f(this, Ge), f(this, Se));
}, yl = function() {
  var _;
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
          for (const v of f(this, Bt))
            d.unskip_effect(v, (g) => {
              var p;
              (g.f & (Ae | Ht)) !== 0 ? d.schedule(g) : A(p = d, D, Lt).call(p, [g]);
            });
        d.activate();
        var l = /* @__PURE__ */ new Set(), a = /* @__PURE__ */ new Map();
        for (var o of n)
          Ii(o, s, l, a);
        a = /* @__PURE__ */ new Map();
        var c = [...d.current].filter(([v, g]) => {
          const p = this.current.get(v);
          return p ? p[0] !== g[0] || p[1] !== g[1] : !0;
        }).map(([v]) => v);
        if (c.length > 0)
          for (const v of f(this, vn))
            (v.f & (ve | ne | On)) === 0 && xr(v, c, a) && ((v.f & (Ht | Ae)) !== 0 ? (z(v, G), d.schedule(v)) : f(d, Ge).add(v));
        if (f(d, oe).length > 0 && !f(d, wt)) {
          d.apply();
          for (var h of f(d, oe))
            A(_ = d, D, lr).call(_, h, [], []);
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
let ut = jn;
function O(e) {
  var t = ln;
  ln = !0;
  try {
    for (var n; ; ) {
      if (al(), T === null)
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
function wl() {
  try {
    Us();
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
      if ((r.f & (ve | ne)) === 0 && _n(r) && (Te = /* @__PURE__ */ new Set(), Jt(r), r.deps === null && r.first === null && r.nodes === null && r.teardown === null && r.ac === null && Gi(r), (Te == null ? void 0 : Te.size) > 0)) {
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
function Ii(e, t, n, r) {
  if (!n.has(e) && (n.add(e), e.reactions !== null))
    for (const i of e.reactions) {
      const s = i.f;
      (s & Z) !== 0 ? Ii(
        /** @type {Derived} */
        i,
        t,
        n,
        r
      ) : (s & (Ht | Ae)) !== 0 && (s & G) === 0 && xr(i, t, r) && (z(i, G), Er(
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
function Di(e, t) {
  if (!((e.f & Oe) !== 0 && (e.f & U) !== 0)) {
    (e.f & G) !== 0 ? t.d.push(e) : (e.f & Fe) !== 0 && t.m.push(e), z(e, U);
    for (var n = e.first; n !== null; )
      Di(n, t), n = n.next;
  }
}
function Li(e) {
  z(e, U);
  for (var t = e.first; t !== null; )
    Li(t), t = t.next;
}
let Ln = /* @__PURE__ */ new Set();
const kt = /* @__PURE__ */ new Map();
let Pi = !1;
function Mt(e, t) {
  var n = {
    f: 0,
    // TODO ideally we could skip this altogether, but it causes type errors
    v: e,
    reactions: null,
    equals: mi,
    rv: 0,
    wv: 0
  };
  return n;
}
// @__NO_SIDE_EFFECTS__
function Ie(e, t) {
  const n = Mt(e);
  return Zi(n), n;
}
// @__NO_SIDE_EFFECTS__
function Hi(e, t = !1, n = !0) {
  const r = Mt(e);
  return t || (r.equals = _i), r;
}
function Me(e, t, n = !1) {
  M !== null && // since we are untracking the function inside `$inspect.with` we need to add this check
  // to ensure we error if state is set inside an inspect effect
  (!Ne || (M.f & On) !== 0) && bi() && (M.f & (Z | Ae | Ht | On)) !== 0 && (je === null || !je.has(e)) && Qs();
  let r = n ? pt(t) : t;
  return Kt(e, r, Tn);
}
function Kt(e, t, n = null) {
  if (!e.equals(t)) {
    kt.set(e, tt ? t : e.v);
    var r = ut.ensure();
    if (r.capture(e, t), (e.f & Z) !== 0) {
      const i = (
        /** @type {Derived} */
        e
      );
      (e.f & G) !== 0 && wr(i), J === null && yr(i);
    }
    e.wv = es(), ji(e, G, n), C !== null && (C.f & U) !== 0 && (C.f & (Oe | et)) === 0 && (me === null ? Ol([e]) : me.push(e)), !r.is_fork && Ln.size > 0 && !Pi && xl();
  }
  return t;
}
function xl() {
  Pi = !1;
  for (const e of Ln) {
    (e.f & U) !== 0 && z(e, Fe);
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
function ji(e, t, n) {
  var r = e.reactions;
  if (r !== null)
    for (var i = r.length, s = 0; s < i; s++) {
      var l = r[s], a = l.f, o = (a & G) === 0;
      if (o && z(l, t), (a & On) !== 0)
        Ln.add(
          /** @type {Effect} */
          l
        );
      else if ((a & Z) !== 0) {
        var c = (
          /** @type {Derived} */
          l
        );
        J == null || J.delete(c), (a & Ct) === 0 && (a & xe && (C === null || (C.f & Rn) === 0) && (l.f |= Ct), ji(c, Fe, n));
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
  const t = ui(e);
  if (t !== Is && t !== Ds)
    return e;
  var n = /* @__PURE__ */ new Map(), r = _r(e), i = /* @__PURE__ */ Ie(0), s = St, l = (a) => {
    if (St === s)
      return a();
    var o = M, c = St;
    $e(null), Jr(s);
    var h = a();
    return $e(o), Jr(c), h;
  };
  return r && n.set("length", /* @__PURE__ */ Ie(
    /** @type {any[]} */
    e.length
  )), new Proxy(
    /** @type {any} */
    e,
    {
      defineProperty(a, o, c) {
        (!("value" in c) || c.configurable === !1 || c.enumerable === !1 || c.writable === !1) && Js();
        var h = n.get(o);
        return h === void 0 ? l(() => {
          var _ = /* @__PURE__ */ Ie(c.value);
          return n.set(o, _), _;
        }) : Me(h, c.value, !0), !0;
      },
      deleteProperty(a, o) {
        var c = n.get(o);
        if (c === void 0) {
          if (o in a) {
            const h = l(() => /* @__PURE__ */ Ie(V));
            n.set(o, h), an(i);
          }
        } else
          Me(c, V), an(i);
        return !0;
      },
      get(a, o, c) {
        var v;
        if (o === $t)
          return e;
        var h = n.get(o), _ = o in a;
        if (h === void 0 && (!_ || (v = Et(a, o)) != null && v.writable) && (h = l(() => {
          var g = pt(_ ? a[o] : V), p = /* @__PURE__ */ Ie(g);
          return p;
        }), n.set(o, h)), h !== void 0) {
          var d = N(h);
          return d === V ? void 0 : d;
        }
        return Reflect.get(a, o, c);
      },
      getOwnPropertyDescriptor(a, o) {
        var c = Reflect.getOwnPropertyDescriptor(a, o);
        if (c && "value" in c) {
          var h = n.get(o);
          h && (c.value = N(h));
        } else if (c === void 0) {
          var _ = n.get(o), d = _ == null ? void 0 : _.v;
          if (_ !== void 0 && d !== V)
            return {
              enumerable: !0,
              configurable: !0,
              value: d,
              writable: !0
            };
        }
        return c;
      },
      has(a, o) {
        var d;
        if (o === $t)
          return !0;
        var c = n.get(o), h = c !== void 0 && c.v !== V || Reflect.has(a, o);
        if (c !== void 0 || C !== null && (!h || (d = Et(a, o)) != null && d.writable)) {
          c === void 0 && (c = l(() => {
            var v = h ? pt(a[o]) : V, g = /* @__PURE__ */ Ie(v);
            return g;
          }), n.set(o, c));
          var _ = N(c);
          if (_ === V)
            return !1;
        }
        return h;
      },
      set(a, o, c, h) {
        var m;
        var _ = n.get(o), d = o in a;
        if (r && o === "length")
          for (var v = c; v < /** @type {Source<number>} */
          _.v; v += 1) {
            var g = n.get(v + "");
            g !== void 0 ? Me(g, V) : v in a && (g = l(() => /* @__PURE__ */ Ie(V)), n.set(v + "", g));
          }
        if (_ === void 0)
          (!d || (m = Et(a, o)) != null && m.writable) && (_ = l(() => /* @__PURE__ */ Ie(void 0)), Me(_, pt(c)), n.set(o, _));
        else {
          d = _.v !== V;
          var p = l(() => pt(c));
          Me(_, p);
        }
        var u = Reflect.getOwnPropertyDescriptor(a, o);
        if (u != null && u.set && u.set.call(h, c), !d) {
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
        N(i);
        var o = Reflect.ownKeys(a).filter((_) => {
          var d = n.get(_);
          return d === void 0 || d.v !== V;
        });
        for (var [c, h] of n)
          h.v !== V && !(c in a) && o.push(c);
        return o;
      },
      setPrototypeOf() {
        Zs();
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
function El(e, t) {
  return Object.is(Vr(e), Vr(t));
}
var Ur, Fi, Wi, Yi;
function ar() {
  if (Ur === void 0) {
    Ur = window, Fi = /Firefox/.test(navigator.userAgent);
    var e = Element.prototype, t = Node.prototype, n = Text.prototype;
    Wi = Et(t, "firstChild").get, Yi = Et(t, "nextSibling").get, zr(e) && (e[Ws] = void 0, e[En] = null, e[Ys] = void 0, e.__e = void 0), zr(n) && (n[Qt] = void 0);
  }
}
function He(e = "") {
  return document.createTextNode(e);
}
// @__NO_SIDE_EFFECTS__
function fn(e) {
  return (
    /** @type {TemplateNode | null} */
    Wi.call(e)
  );
}
// @__NO_SIDE_EFFECTS__
function it(e) {
  return (
    /** @type {TemplateNode | null} */
    Yi.call(e)
  );
}
function X(e, t) {
  if (!P)
    return /* @__PURE__ */ fn(e);
  var n = /* @__PURE__ */ fn(H);
  if (n === null)
    n = H.appendChild(He());
  else if (t && n.nodeType !== br) {
    var r = He();
    return n == null || n.before(r), pe(r), r;
  }
  return t && Bi(
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
    Bi(
      /** @type {Text} */
      r
    );
  }
  return pe(r), r;
}
function qi(e) {
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
function Bi(e) {
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
function $l() {
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
            (t = n[vi]) == null || t.call(n);
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
function kl(e) {
  C === null && (M === null && Vs(), Xs()), tt && Bs();
}
function Tl(e, t) {
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
    Pt !== null ? Pt.push(r) : ut.ensure().schedule(r);
  else if (t !== null) {
    try {
      Jt(r);
    } catch (l) {
      throw ie(r), l;
    }
    i.deps === null && i.teardown === null && i.nodes === null && i.first === i.last && // either `null`, or a singular child
    (i.f & Nt) === 0 && (i = i.first, (e & Ae) !== 0 && (e & Ut) !== 0 && i !== null && (i.f |= Ut));
  }
  if (i !== null && (i.parent = n, n !== null && Tl(i, n), M !== null && (M.f & Z) !== 0 && (e & et) === 0)) {
    var s = (
      /** @type {Derived} */
      M
    );
    (s.effects ?? (s.effects = [])).push(i);
  }
  return r;
}
function Tr() {
  return M !== null && !Ne;
}
function Sr(e) {
  const t = Ye(Yn, null);
  return z(t, U), t.teardown = e, t;
}
function Cr(e) {
  kl();
  var t = (
    /** @type {Effect} */
    C.f
  ), n = !M && (t & Oe) !== 0 && re !== null && !re.i;
  if (n) {
    var r = (
      /** @type {ComponentContext} */
      re
    );
    (r.e ?? (r.e = [])).push(e);
  } else
    return Xi(e);
}
function Xi(e) {
  return Ye(Vt | js, e);
}
function Sl(e) {
  ut.ensure();
  const t = Ye(et | Nt, e);
  return () => {
    ie(t);
  };
}
function Cl(e) {
  ut.ensure();
  const t = Ye(et | Nt, e);
  return (n = {}) => new Promise((r) => {
    n.outro ? Tt(t, () => {
      ie(t), r(void 0);
    }) : (ie(t), r(void 0));
  });
}
function Vi(e) {
  return Ye(Vt, e);
}
function Ml(e) {
  return Ye(Ht | Nt, e);
}
function Mr(e, t = 0) {
  return Ye(Yn | t, e);
}
function ge(e, t = [], n = [], r = []) {
  vl(r, t, n, (i) => {
    Ye(Yn, () => {
      e(...i.map(N));
    });
  });
}
function Ar(e, t = 0) {
  var n = Ye(Ae | t, e);
  return n;
}
function we(e) {
  return Ye(Oe | Nt, e);
}
function Ui(e) {
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
function Nr(e, t = !1) {
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
function Al(e) {
  for (var t = e.first; t !== null; ) {
    var n = t.next;
    (t.f & Oe) === 0 && ie(t), t = n;
  }
}
function ie(e, t = !0) {
  var n = !1;
  (t || (e.f & Hs) !== 0) && e.nodes !== null && e.nodes.end !== null && (Nl(
    e.nodes.start,
    /** @type {TemplateNode} */
    e.nodes.end
  ), n = !0), e.f |= er, Nr(e, t && !n), un(e, 0);
  var r = e.nodes && e.nodes.t;
  if (r !== null)
    for (const s of r)
      s.stop();
  Ui(e), e.f ^= er, e.f |= ve;
  var i = e.parent;
  i !== null && i.first !== null && Gi(e), e.next = e.prev = e.teardown = e.ctx = e.deps = e.fn = e.nodes = e.ac = e.b = null;
}
function Nl(e, t) {
  for (; e !== null; ) {
    var n = e === t ? null : /* @__PURE__ */ it(e);
    e.remove(), e = n;
  }
}
function Gi(e) {
  var t = e.parent, n = e.prev, r = e.next;
  n !== null && (n.next = r), r !== null && (r.prev = n), t !== null && (t.first === e && (t.first = r), t.last === e && (t.last = n));
}
function Tt(e, t, n = !0) {
  var r = [];
  Ki(e, r, !0);
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
function Ki(e, t, n) {
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
        (i.f & Oe) !== 0 && (e.f & Ae) !== 0;
        Ki(i, t, l ? n : !1);
      }
      i = s;
    }
  }
}
function Pn(e) {
  Ji(e, !0);
}
function Ji(e, t) {
  if ((e.f & ne) !== 0) {
    e.f ^= ne, (e.f & U) === 0 && (z(e, G), ut.ensure().schedule(e));
    for (var n = e.first; n !== null; ) {
      var r = n.next, i = (n.f & Ut) !== 0 || (n.f & Oe) !== 0;
      Ji(n, i ? t : !1), n = r;
    }
    var s = e.nodes && e.nodes.t;
    if (s !== null)
      for (const l of s)
        (l.is_global || t) && l.in();
  }
}
function Or(e, t) {
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
let M = null, Ne = !1;
function $e(e) {
  M = e;
}
let C = null;
function We(e) {
  C = e;
}
let je = null;
function Zi(e) {
  M !== null && (je ?? (je = /* @__PURE__ */ new Set())).add(e);
}
let fe = null, ue = 0, me = null;
function Ol(e) {
  me = e;
}
let Qi = 1, gt = 0, St = gt;
function Jr(e) {
  St = e;
}
function es() {
  return ++Qi;
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
      ) && Ai(
        /** @type {Derived} */
        s
      ), s.wv > e.wv)
        return !0;
    }
    (t & xe) !== 0 && // During time traveling we don't want to reset the status so that
    // traversal of the graph in the other batches still happens
    J === null && z(e, U);
  }
  return !1;
}
function ts(e, t, n = !0) {
  var r = e.reactions;
  if (r !== null && !(je !== null && je.has(e)))
    for (var i = 0; i < r.length; i++) {
      var s = r[i];
      (s.f & Z) !== 0 ? ts(
        /** @type {Derived} */
        s,
        t,
        !1
      ) : t === s && (n ? z(s, G) : (s.f & U) !== 0 && z(s, Fe), Er(
        /** @type {Effect} */
        s
      ));
    }
}
function ns(e) {
  var p;
  var t = fe, n = ue, r = me, i = M, s = je, l = re, a = Ne, o = St, c = e.f;
  fe = /** @type {null | Value[]} */
  null, ue = 0, me = null, M = (c & (Oe | et)) === 0 ? e : null, je = null, Gt(e.ctx), Ne = !1, St = ++gt, e.ac !== null && (kr(() => {
    e.ac.abort(qn);
  }), e.ac = null);
  try {
    e.f |= Rn;
    var h = (
      /** @type {Function} */
      e.fn
    ), _ = h();
    e.f |= At;
    var d = e.deps, v = T == null ? void 0 : T.is_fork;
    if (fe !== null) {
      var g;
      if (v || un(e, ue), d !== null && ue > 0)
        for (d.length = ue + fe.length, g = 0; g < fe.length; g++)
          d[ue + g] = fe[g];
      else
        e.deps = d = fe;
      if (Tr() && (e.f & xe) !== 0)
        for (g = ue; g < d.length; g++)
          ((p = d[g]).reactions ?? (p.reactions = [])).push(e);
    } else !v && d !== null && ue < d.length && (un(e, ue), d.length = ue);
    if (bi() && me !== null && !Ne && d !== null && (e.f & (Z | Fe | G)) === 0)
      for (g = 0; g < /** @type {Source[]} */
      me.length; g++)
        ts(
          me[g],
          /** @type {Effect} */
          e
        );
    if (i !== null && i !== e) {
      if (gt++, i.deps !== null)
        for (let u = 0; u < n; u += 1)
          i.deps[u].rv = gt;
      if (t !== null)
        for (const u of t)
          u.rv = gt;
      me !== null && (r === null ? r = me : r.push(.../** @type {Source[]} */
      me));
    }
    return (e.f & ft) !== 0 && (e.f ^= ft), _;
  } catch (u) {
    return wi(u);
  } finally {
    e.f ^= Rn, fe = t, ue = n, me = r, M = i, je = s, Gt(l), Ne = a, St = o;
  }
}
function Rl(e, t) {
  let n = t.reactions;
  if (n !== null) {
    var r = Os.call(n, e);
    if (r !== -1) {
      var i = n.length - 1;
      i === 0 ? n = t.reactions = null : (n[r] = n[i], n.pop());
    }
  }
  if (n === null && (t.f & Z) !== 0 && // Destroying a child effect while updating a parent effect can cause a dependency to appear
  // to be unused, when in fact it is used by the currently-updating parent. Checking `new_deps`
  // allows us to skip the expensive work of disconnecting and immediately reconnecting it
  (fe === null || !Mn.call(fe, t))) {
    var s = (
      /** @type {Derived} */
      t
    );
    (s.f & xe) !== 0 && (s.f ^= xe, s.f &= ~Ct), s.v !== V && yr(s), _l(s), un(s, 0);
  }
}
function un(e, t) {
  var n = e.deps;
  if (n !== null)
    for (var r = t; r < n.length; r++)
      Rl(e, n[r]);
}
function Jt(e) {
  var t = e.f;
  if ((t & ve) === 0) {
    z(e, U);
    var n = C, r = Sn;
    C = e, Sn = !0;
    try {
      (t & (Ae | di)) !== 0 ? Al(e) : Nr(e), Ui(e);
      var i = ns(e);
      e.teardown = typeof i == "function" ? i : null, e.wv = Qi;
      var s;
      fi && ll && (e.f & G) !== 0 && e.deps;
    } finally {
      Sn = r, C = n;
    }
  }
}
function N(e) {
  var t = e.f, n = (t & Z) !== 0;
  if (M !== null && !Ne) {
    var r = C !== null && (C.f & ve) !== 0;
    if (!r && (je === null || !je.has(e))) {
      var i = M.deps;
      if ((M.f & Rn) !== 0)
        e.rv < gt && (e.rv = gt, fe === null && i !== null && i[ue] === e ? ue++ : fe === null ? fe = [e] : fe.push(e));
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
      return ((l.f & U) === 0 && l.reactions !== null || is(l)) && (a = wr(l)), kt.set(l, a), a;
    }
    var o = (l.f & xe) === 0 && !Ne && M !== null && (Sn || (M.f & xe) !== 0), c = (l.f & At) === 0;
    _n(l) && (o && (l.f |= xe), Ai(l)), o && !c && (Ni(l), rs(l));
  }
  if (J != null && J.has(e))
    return J.get(e);
  if ((e.f & ft) !== 0)
    throw e.v;
  return e.v;
}
function rs(e) {
  if (e.f |= xe, e.deps !== null)
    for (const t of e.deps)
      (t.reactions ?? (t.reactions = [])).push(e), (t.f & Z) !== 0 && (t.f & xe) === 0 && (Ni(
        /** @type {Derived} */
        t
      ), rs(
        /** @type {Derived} */
        t
      ));
}
function is(e) {
  if (e.v === V) return !0;
  if (e.deps === null) return !1;
  for (const t of e.deps)
    if (kt.has(t) || (t.f & Z) !== 0 && is(
      /** @type {Derived} */
      t
    ))
      return !0;
  return !1;
}
function Rr(e) {
  var t = Ne;
  try {
    return Ne = !0, e();
  } finally {
    Ne = t;
  }
}
const mt = Symbol("events"), ss = /* @__PURE__ */ new Set(), or = /* @__PURE__ */ new Set();
function Il(e, t, n, r = {}) {
  function i(s) {
    if (r.capture || fr.call(t, s), !s.cancelBubble)
      return kr(() => n == null ? void 0 : n.call(this, s));
  }
  return Ze(() => {
    t.addEventListener(e, i, r);
  }), i;
}
function ls(e, t, n, r, i) {
  var s = { capture: r, passive: i }, l = Il(e, t, n, s);
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
function Ot(e) {
  for (var t = 0; t < e.length; t++)
    ss.add(e[t]);
  for (var n of or)
    n(e);
}
let Zr = null;
function fr(e) {
  var p, u;
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
    var c = i.indexOf(t);
    if (c === -1)
      return;
    o <= c && (l = o);
  }
  if (s = /** @type {Element} */
  i[l] || e.target, s !== t) {
    Nn(e, "currentTarget", {
      configurable: !0,
      get() {
        return s || n;
      }
    });
    var h = M, _ = C;
    $e(null), We(null);
    try {
      for (var d, v = []; s !== null && s !== t; ) {
        try {
          var g = (u = s[mt]) == null ? void 0 : u[r];
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
      e[mt] = t, delete e.currentTarget, $e(h), We(_);
    }
  }
}
var li;
const Jn = (
  // We gotta write it like this because after downleveling the pure comment may end up in the wrong location
  ((li = globalThis == null ? void 0 : globalThis.window) == null ? void 0 : li.trustedTypes) && /* @__PURE__ */ globalThis.window.trustedTypes.createPolicy("svelte-trusted-html", {
    /** @param {string} html */
    createHTML: (e) => e
  })
);
function Dl(e) {
  return (
    /** @type {string} */
    (Jn == null ? void 0 : Jn.createHTML(e)) ?? e
  );
}
function Ll(e) {
  var t = $r("template");
  return t.innerHTML = Dl(e.replaceAll("<!>", "<!---->")), t.content;
}
function ur(e, t) {
  var n = (
    /** @type {Effect} */
    C
  );
  n.nodes === null && (n.nodes = { start: e, end: t, a: null, t: null });
}
// @__NO_SIDE_EFFECTS__
function se(e, t) {
  var n = (t & As) !== 0, r, i = !e.startsWith("<!>");
  return () => {
    if (P)
      return ur(H, null), H;
    r === void 0 && (r = Ll(i ? e : "<!>" + e), r = /** @type {TemplateNode} */
    /* @__PURE__ */ fn(r));
    var s = (
      /** @type {TemplateNode} */
      n || Fi ? document.importNode(r, !0) : r.cloneNode(!0)
    );
    return ur(s, s), s;
  };
}
function ee(e, t) {
  if (P) {
    var n = (
      /** @type {Effect & { nodes: EffectNodes }} */
      C
    );
    ((n.f & At) === 0 || n.nodes.end === null) && (n.nodes.end = H), Bn();
    return;
  }
  e !== null && e.before(
    /** @type {Node} */
    t
  );
}
const Pl = ["touchstart", "touchmove"];
function Hl(e) {
  return Pl.includes(e);
}
function Re(e, t) {
  var n = t == null ? "" : typeof t == "object" ? `${t}` : t;
  n !== /** @type {any} */
  (e[Qt] ?? (e[Qt] = e.nodeValue)) && (e[Qt] = n, e.nodeValue = `${n}`);
}
function as(e, t) {
  return os(e, t);
}
function jl(e, t) {
  ar(), t.intro = t.intro ?? !1;
  const n = t.target, r = P, i = H;
  try {
    for (var s = /* @__PURE__ */ fn(n); s && (s.nodeType !== mn || /** @type {Comment} */
    s.data !== oi); )
      s = /* @__PURE__ */ it(s);
    if (!s)
      throw Xt;
    Je(!0), pe(
      /** @type {Comment} */
      s
    );
    const l = os(e, { ...t, anchor: s });
    return Je(!1), /**  @type {Exports} */
    l;
  } catch (l) {
    if (l instanceof Error && l.message.split(`
`).some((a) => a.startsWith("https://svelte.dev/e/")))
      throw l;
    return l !== Xt && console.warn("Failed to hydrate: ", l), t.recover === !1 && Gs(), ar(), qi(n), Je(!1), as(e, t);
  } finally {
    Je(r), pe(i);
  }
}
const xn = /* @__PURE__ */ new Map();
function os(e, { target: t, anchor: n, props: r = {}, events: i, context: s, intro: l = !0, transformError: a }) {
  ar();
  var o = void 0, c = Cl(() => {
    var h = n ?? t.appendChild(He());
    dl(
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
        if (s && (g.c = s), i && (r.$$events = i), P && ur(
          /** @type {TemplateNode} */
          v,
          null
        ), o = e(v, r) || {}, P && (C.nodes.end = H, H === null || H.nodeType !== mn || /** @type {Comment} */
        H.data !== mr))
          throw zn(), Xt;
        rt();
      },
      a
    );
    var _ = /* @__PURE__ */ new Set(), d = (v) => {
      for (var g = 0; g < v.length; g++) {
        var p = v[g];
        if (!_.has(p)) {
          _.add(p);
          var u = Hl(p);
          for (const m of [t, document]) {
            var b = xn.get(m);
            b === void 0 && (b = /* @__PURE__ */ new Map(), xn.set(m, b));
            var w = b.get(p);
            w === void 0 ? (m.addEventListener(p, fr, { passive: u }), b.set(p, 1)) : b.set(p, w + 1);
          }
        }
      }
    };
    return d(Wn(ss)), or.add(d), () => {
      var u;
      for (var v of _)
        for (const b of [t, document]) {
          var g = (
            /** @type {Map<string, number>} */
            xn.get(b)
          ), p = (
            /** @type {number} */
            g.get(v)
          );
          --p == 0 ? (b.removeEventListener(v, fr), g.delete(v), g.size === 0 && xn.delete(b)) : g.set(v, p);
        }
      or.delete(d), h !== n && ((u = h.parentNode) == null || u.removeChild(h));
    };
  });
  return cr.set(o, c), o;
}
let cr = /* @__PURE__ */ new WeakMap();
function Fl(e, t) {
  const n = cr.get(e);
  return n ? (cr.delete(e), n(t)) : Promise.resolve();
}
var Ce, Le, he, xt, pn, gn, Fn;
class Wl {
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
              var c = document.createDocumentFragment();
              Or(l, c), c.append(He()), f(this, he).set(s, { effect: l, fragment: c });
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
  P && (r = H, Bn());
  var i = new Wl(e), s = n ? Ut : 0;
  function l(a, o) {
    if (P) {
      var c = gi(
        /** @type {TemplateNode} */
        r
      );
      if (a !== parseInt(c.substring(1))) {
        var h = In();
        pe(h), i.anchor = h, Je(!1), i.ensure(a, o), Je(!0);
        return;
      }
    }
    i.ensure(a, o);
  }
  Ar(() => {
    var a = !1;
    t((o, c = 0) => {
      a = !0, l(c, o);
    }), a || l(-1, null);
  }, s);
}
function fs(e, t) {
  return t;
}
function Yl(e, t, n) {
  for (var r = [], i = t.length, s, l = t.length, a = 0; a < i; a++) {
    let _ = t[a];
    Tt(
      _,
      () => {
        if (s) {
          if (s.pending.delete(_), s.done.add(_), s.pending.size === 0) {
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
      var c = (
        /** @type {Element} */
        n
      ), h = (
        /** @type {Element} */
        c.parentNode
      );
      qi(h), h.append(c), e.items.clear();
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
      Or(s, l);
    } else
      ie(t[i], n);
  }
}
var Qr;
function us(e, t, n, r, i, s = null) {
  var l = e, a = /* @__PURE__ */ new Map(), o = (t & ai) !== 0;
  if (o) {
    var c = (
      /** @type {Element} */
      e
    );
    l = P ? pe(/* @__PURE__ */ fn(c)) : c.appendChild(He());
  }
  P && Bn();
  var h = null, _ = /* @__PURE__ */ Mi(() => {
    var m = n();
    return (
      /** @type {V[]} */
      _r(m) ? m : m == null ? [] : Wn(m)
    );
  }), d, v = /* @__PURE__ */ new Map(), g = !0;
  function p(m) {
    (w.effect.f & ve) === 0 && (w.pending.delete(m), w.fallback = h, ql(w, d, l, t, r), h !== null && (d.length === 0 ? (h.f & Pe) === 0 ? Pn(h) : (h.f ^= Pe, rn(h, null, l)) : Tt(h, () => {
      h = null;
    })));
  }
  function u(m) {
    w.pending.delete(m);
  }
  var b = Ar(() => {
    d = /** @type {V[]} */
    N(_);
    var m = d.length;
    let E = !1;
    if (P) {
      var y = gi(l) === gr;
      y !== (m === 0) && (l = In(), pe(l), Je(!1), E = !0);
    }
    for (var $ = /* @__PURE__ */ new Set(), R = (
      /** @type {Batch} */
      T
    ), Y = zi(), L = 0; L < m; L += 1) {
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
    if (m === 0 && s && !h && (g ? h = we(() => s(l)) : (h = we(() => s(Qr ?? (Qr = He()))), h.f |= Pe)), m > $.size && zs(), P && m > 0 && pe(In()), !g)
      if (v.set(R, $), Y) {
        for (const [ht, It] of a)
          $.has(ht) || R.skip_effect(It.e);
        R.oncommit(p), R.ondiscard(u);
      } else
        p(R);
    E && Je(!0), N(_);
  }), w = { effect: b, items: a, pending: v, outrogroups: null, fallback: h };
  g = !1, P && (l = H);
}
function Zt(e) {
  for (; e !== null && (e.f & Oe) === 0; )
    e = e.next;
  return e;
}
function ql(e, t, n, r, i) {
  var j, q, K, ht, It, ze, x, le, Fr;
  var s = (r & $s) !== 0, l = t.length, a = e.items, o = Zt(e.effect.first), c, h = null, _, d = [], v = [], g, p, u, b;
  if (s)
    for (b = 0; b < l; b += 1)
      g = t[b], p = i(g, b), u = /** @type {EachItem} */
      a.get(p).e, (u.f & Pe) === 0 && ((q = (j = u.nodes) == null ? void 0 : j.a) == null || q.measure(), (_ ?? (_ = /* @__PURE__ */ new Set())).add(u));
  for (b = 0; b < l; b += 1) {
    if (g = t[b], p = i(g, b), u = /** @type {EachItem} */
    a.get(p).e, e.outrogroups !== null)
      for (const Be of e.outrogroups)
        Be.pending.delete(u), Be.done.delete(u);
    if ((u.f & ne) !== 0 && (Pn(u), s && ((ht = (K = u.nodes) == null ? void 0 : K.a) == null || ht.unfix(), (_ ?? (_ = /* @__PURE__ */ new Set())).delete(u))), (u.f & Pe) !== 0)
      if (u.f ^= Pe, u === o)
        rn(u, null, n);
      else {
        var w = h ? h.next : o;
        u === e.effect.last && (e.effect.last = u.prev), u.prev && (u.prev.next = u.next), u.next && (u.next.prev = u.prev), st(e, h, u), st(e, u, w), rn(u, w, n), h = u, d = [], v = [], o = Zt(h.next);
        continue;
      }
    if (u !== o) {
      if (c !== void 0 && c.has(u)) {
        if (d.length < v.length) {
          var m = v[0], E;
          h = m.prev;
          var y = d[0], $ = d[d.length - 1];
          for (E = 0; E < d.length; E += 1)
            rn(d[E], m, n);
          for (E = 0; E < v.length; E += 1)
            c.delete(v[E]);
          st(e, y.prev, $.next), st(e, h, y), st(e, $, m), o = m, h = $, b -= 1, d = [], v = [];
        } else
          c.delete(u), rn(u, o, n), st(e, u.prev, u.next), st(e, u, h === null ? e.effect.first : h.next), st(e, h, u), h = u;
        continue;
      }
      for (d = [], v = []; o !== null && o !== u; )
        (c ?? (c = /* @__PURE__ */ new Set())).add(o), v.push(o), o = Zt(o.next);
      if (o === null)
        continue;
    }
    (u.f & Pe) === 0 && d.push(u), h = u, o = Zt(u.next);
  }
  if (e.outrogroups !== null) {
    for (const Be of e.outrogroups)
      Be.pending.size === 0 && (dr(e, Wn(Be.done)), (It = e.outrogroups) == null || It.delete(Be));
    e.outrogroups.size === 0 && (e.outrogroups = null);
  }
  if (o !== null || c !== void 0) {
    var R = [];
    if (c !== void 0)
      for (u of c)
        (u.f & ne) === 0 && R.push(u);
    for (; o !== null; )
      (o.f & ne) === 0 && o !== e.fallback && R.push(o), o = Zt(o.next);
    var Y = R.length;
    if (Y > 0) {
      var L = (r & ai) !== 0 && l === 0 ? n : null;
      if (s) {
        for (b = 0; b < Y; b += 1)
          (x = (ze = R[b].nodes) == null ? void 0 : ze.a) == null || x.measure();
        for (b = 0; b < Y; b += 1)
          (Fr = (le = R[b].nodes) == null ? void 0 : le.a) == null || Fr.fix();
      }
      Yl(e, R, L);
    }
  }
  s && Ze(() => {
    var Be, Wr;
    if (_ !== void 0)
      for (u of _)
        (Wr = (Be = u.nodes) == null ? void 0 : Be.a) == null || Wr.apply();
  });
}
function zl(e, t, n, r, i, s, l, a) {
  var o = (l & xs) !== 0 ? (l & ks) === 0 ? /* @__PURE__ */ Hi(n, !1, !1) : Mt(n) : null, c = (l & Es) !== 0 ? Mt(i) : null;
  return {
    v: o,
    i: c,
    e: we(() => (s(t, o ?? n, c ?? i, a), () => {
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
  Vi(() => {
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
function cs(e, t, n = !1) {
  if (e.multiple) {
    if (t == null)
      return;
    if (!_r(t))
      return nl();
    for (var r of e.options)
      r.selected = t.includes(ei(r));
    return;
  }
  for (r of e.options) {
    var i = ei(r);
    if (El(i, t)) {
      r.selected = !0;
      return;
    }
  }
  (!n || t !== void 0) && (e.selectedIndex = -1);
}
function Bl(e) {
  var t = new MutationObserver(() => {
    cs(e, e.__value);
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
const Xl = Symbol("is custom element"), Vl = Symbol("is html"), Ul = pi ? "link" : "LINK", Gl = pi ? "progress" : "PROGRESS";
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
    e[vi] = n, Ze(n), $l();
  }
}
function Ir(e, t) {
  var n = Dr(e);
  n.value === (n.value = // treat null and undefined the same for the initial value
  t ?? void 0) || // @ts-expect-error
  // `progress` elements always need their value set when it's `0`
  e.value === t && (t !== 0 || e.nodeName !== Gl) || (e.value = t ?? "");
}
function ds(e, t) {
  var n = Dr(e);
  n.checked !== (n.checked = // treat null and undefined the same for the initial value
  t ?? void 0) && (e.checked = t);
}
function Kl(e, t) {
  t ? e.hasAttribute("selected") || e.setAttribute("selected", "") : e.removeAttribute("selected");
}
function Qe(e, t, n, r) {
  var i = Dr(e);
  P && (i[t] = e.getAttribute(t), t === "src" || t === "srcset" || t === "href" && e.nodeName === Ul) || i[t] !== (i[t] = n) && (t === "loading" && (e[Fs] = n), n == null ? e.removeAttribute(t) : typeof n != "string" && Jl(e).includes(t) ? e[t] = n : e.setAttribute(t, n));
}
function Dr(e) {
  return (
    /** @type {Record<string | symbol, unknown>} **/
    /** @type {any} */
    e[En] ?? (e[En] = {
      [Xl]: e.nodeName.includes("-"),
      [Vl]: e.namespaceURI === Ns
    })
  );
}
var ti = /* @__PURE__ */ new Map();
function Jl(e) {
  var t = e.getAttribute("is") || e.nodeName, n = ti.get(t);
  if (n) return n;
  ti.set(t, n = []);
  for (var r, i = e, s = Element.prototype; s !== i; ) {
    r = Rs(i);
    for (var l in r)
      r[l].set && // better safe than sorry, we don't want spread attributes to mess with HTML content
      l !== "innerHTML" && l !== "textContent" && l !== "innerText" && n.push(l);
    i = ui(i);
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
  return Vi(() => {
    var l, a;
    return Mr(() => {
      l = a, a = [], Rr(() => {
        Zn(n(...a), e) || (t(e, ...a), l && Zn(n(...l), e) && t(null, ...l));
      });
    }), () => {
      let o = s;
      for (; o !== i && o.parent !== null && o.parent.f & er; )
        o = o.parent;
      const c = () => {
        a && Zn(n(...a), e) && t(null, ...a);
      }, h = o.teardown;
      o.teardown = () => {
        c(), h == null || h();
      };
    };
  }), e;
}
function I(e, t, n, r) {
  var E;
  var i = !0, s = (n & Cs) !== 0, l = (n & Ms) !== 0, a = (
    /** @type {V} */
    r
  ), o = !0, c = (
    /** @type {Derived<V> | undefined} */
    void 0
  ), h = () => l && i ? (c ?? (c = /* @__PURE__ */ on(
    /** @type {() => V} */
    r
  )), N(c)) : (o && (o = !1, a = l ? Rr(
    /** @type {() => V} */
    r
  ) : (
    /** @type {V} */
    r
  )), a);
  let _;
  if (s) {
    var d = $t in e || hi in e;
    _ = ((E = Et(e, t)) == null ? void 0 : E.set) ?? (d && t in e ? (y) => e[t] = y : void 0);
  }
  var v, g = !1;
  s ? [v, g] = fl(() => (
    /** @type {V} */
    e[t]
  )) : v = /** @type {V} */
  e[t], v === void 0 && r !== void 0 && (v = h(), _ && (Ks(), _(v)));
  var p;
  if (p = () => {
    var y = (
      /** @type {V} */
      e[t]
    );
    return y === void 0 ? h() : (o = !0, y);
  }, (n & Ss) === 0)
    return p;
  if (_) {
    var u = e.$$legacy;
    return (
      /** @type {() => V} */
      (function(y, $) {
        return arguments.length > 0 ? ((!$ || u || g) && _($ ? p() : y), y) : p();
      })
    );
  }
  var b = !1, w = ((n & Ts) !== 0 ? on : Mi)(() => (b = !1, p()));
  s && N(w);
  var m = (
    /** @type {Effect} */
    C
  );
  return (
    /** @type {() => V} */
    (function(y, $) {
      if (arguments.length > 0) {
        const R = $ ? N(w) : s ? pt(y) : y;
        return Me(w, R), b = !0, a !== void 0 && (a = R), y;
      }
      return tt && b || (m.f & ve) !== 0 ? w.v : N(w);
    })
  );
}
function Zl(e) {
  return new Ql(e);
}
var Ke, ye;
class Ql {
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
      var o = /* @__PURE__ */ Hi(a, !1, !1);
      return n.set(l, o), o;
    };
    const i = new Proxy(
      { ...t.props || {}, $$events: {} },
      {
        get(l, a) {
          return N(n.get(a) ?? r(a, Reflect.get(l, a)));
        },
        has(l, a) {
          return a === hi ? !0 : (N(n.get(a) ?? r(a, Reflect.get(l, a))), Reflect.has(l, a));
        },
        set(l, a, o) {
          return Me(n.get(a) ?? r(a, o), o), Reflect.set(l, a, o);
        }
      }
    );
    k(this, ye, (t.hydrate ? jl : as)(t.component, {
      target: t.target,
      anchor: t.anchor,
      props: i,
      context: t.context,
      intro: t.intro ?? !1,
      recover: t.recover,
      transformError: t.transformError
    })), (!((s = t == null ? void 0 : t.props) != null && s.$$host) || t.sync === !1) && O(), k(this, Ke, i.$$events);
    for (const l of Object.keys(f(this, ye)))
      l === "$set" || l === "$destroy" || l === "$on" || Nn(this, l, {
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
      Fl(f(this, ye));
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
let hs;
typeof HTMLElement == "function" && (hs = class extends HTMLElement {
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
      const n = {}, r = ea(this);
      for (const i of this.$$s)
        i in r && (i === "default" && !this.$$d.children ? (this.$$d.children = t(i), n.default = !0) : n[i] = t(i));
      for (const i of this.attributes) {
        const s = this.$$g_p(i.name);
        s in this.$$d || (this.$$d[s] = Cn(s, i.value, this.$$p_d, "toProp"));
      }
      for (const i in this.$$p_d)
        !(i in this.$$d) && this[i] !== void 0 && (this.$$d[i] = this[i], delete this[i]);
      this.$$c = Zl({
        component: this.$$ctor,
        target: this.$$shadowRoot || this,
        props: {
          ...this.$$d,
          $$slots: n,
          $$host: this
        }
      }), this.$$me = Sl(() => {
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
function ea(e) {
  const t = {};
  return e.childNodes.forEach((n) => {
    t[
      /** @type {Element} node */
      n.slot || "default"
    ] = !0;
  }), t;
}
function dt(e, t, n, r, i, s) {
  let l = class extends hs {
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
    Nn(l.prototype, a, {
      get() {
        return this.$$c && a in this.$$c ? this.$$c[a] : this.$$d[a];
      },
      set(o) {
        var _;
        o = Cn(a, o, t), this.$$d[a] = o;
        var c = this.$$c;
        if (c) {
          var h = (_ = Et(c, a)) == null ? void 0 : _.get;
          h ? c[a] = o : c.$set({ [a]: o });
        }
      }
    });
  }), r.forEach((a) => {
    Nn(l.prototype, a, {
      get() {
        var o;
        return (o = this.$$c) == null ? void 0 : o[a];
      }
    });
  }), e.element = /** @type {any} */
  l, l;
}
var ta = /* @__PURE__ */ se('<span class="lbl"> </span>'), na = /* @__PURE__ */ se('<label class="xi-slider svelte-8xo3l7"><!> <input type="range" class="svelte-8xo3l7"/> <span class="val svelte-8xo3l7"> </span></label>');
const ra = {
  hash: "svelte-8xo3l7",
  code: '.xi-slider.svelte-8xo3l7 {display:inline-flex;align-items:center;gap:0.5rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}input[type="range"].svelte-8xo3l7 {accent-color:var(--xi-accent, #3b82f6);vertical-align:middle;}.val.svelte-8xo3l7 {min-width:2.5em;text-align:right;font-variant-numeric:tabular-nums;}'
};
function ia(e, t) {
  nt(t, !0), ct(e, ra);
  let n = I(t, "value", 15, 0), r = I(t, "min", 7, 0), i = I(t, "max", 7, 100), s = I(t, "step", 7, 1), l = I(t, "label", 7, ""), a = I(t, "disabled", 7, !1);
  const o = t.$$host, c = (m) => o.dispatchEvent(new CustomEvent(m, { detail: { value: n() }, bubbles: !0, composed: !0 }));
  function h(m) {
    n(Number(m.target.value)), c("input");
  }
  function _(m) {
    n(Number(m.target.value)), c("change");
  }
  var d = {
    get value() {
      return n();
    },
    set value(m = 0) {
      n(m), O();
    },
    get min() {
      return r();
    },
    set min(m = 0) {
      r(m), O();
    },
    get max() {
      return i();
    },
    set max(m = 100) {
      i(m), O();
    },
    get step() {
      return s();
    },
    set step(m = 1) {
      s(m), O();
    },
    get label() {
      return l();
    },
    set label(m = "") {
      l(m), O();
    },
    get disabled() {
      return a();
    },
    set disabled(m = !1) {
      a(m), O();
    }
  }, v = na(), g = X(v);
  {
    var p = (m) => {
      var E = ta(), y = X(E, !0);
      B(E), ge(() => Re(y, l())), ee(m, E);
    };
    bn(g, (m) => {
      l() && m(p);
    });
  }
  var u = Ee(g, 2);
  Xn(u);
  var b = Ee(u, 2), w = X(b, !0);
  return B(b), B(v), ge(() => {
    Qe(u, "min", r()), Qe(u, "max", i()), Qe(u, "step", s()), Ir(u, n()), u.disabled = a(), Re(w, n());
  }), Q("input", u, h), Q("change", u, _), ee(e, v), rt(d);
}
Ot(["input", "change"]);
customElements.define("xi-slider", dt(
  ia,
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
var sa = /* @__PURE__ */ se('<span class="lbl"> </span>'), la = /* @__PURE__ */ se('<label class="xi-number svelte-1f6ykwb"><!> <input type="number" class="svelte-1f6ykwb"/></label>');
const aa = {
  hash: "svelte-1f6ykwb",
  code: ".xi-number.svelte-1f6ykwb {display:inline-flex;align-items:center;gap:0.5rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}input.svelte-1f6ykwb {width:6em;padding:0.15rem 0.3rem;accent-color:var(--xi-accent, #3b82f6);}"
};
function oa(e, t) {
  nt(t, !0), ct(e, aa);
  let n = I(t, "value", 15, 0), r = I(t, "min", 7), i = I(t, "max", 7), s = I(t, "step", 7, 1), l = I(t, "label", 7, ""), a = I(t, "disabled", 7, !1);
  const o = t.$$host, c = (w) => o.dispatchEvent(new CustomEvent(w, { detail: { value: n() }, bubbles: !0, composed: !0 })), h = (w) => w.target.value === "" ? null : Number(w.target.value);
  function _(w) {
    n(h(w)), c("input");
  }
  function d(w) {
    n(h(w)), c("change");
  }
  var v = {
    get value() {
      return n();
    },
    set value(w = 0) {
      n(w), O();
    },
    get min() {
      return r();
    },
    set min(w) {
      r(w), O();
    },
    get max() {
      return i();
    },
    set max(w) {
      i(w), O();
    },
    get step() {
      return s();
    },
    set step(w = 1) {
      s(w), O();
    },
    get label() {
      return l();
    },
    set label(w = "") {
      l(w), O();
    },
    get disabled() {
      return a();
    },
    set disabled(w = !1) {
      a(w), O();
    }
  }, g = la(), p = X(g);
  {
    var u = (w) => {
      var m = sa(), E = X(m, !0);
      B(m), ge(() => Re(E, l())), ee(w, m);
    };
    bn(p, (w) => {
      l() && w(u);
    });
  }
  var b = Ee(p, 2);
  return Xn(b), B(g), ge(() => {
    Qe(b, "min", r()), Qe(b, "max", i()), Qe(b, "step", s()), Ir(b, n()), b.disabled = a();
  }), Q("input", b, _), Q("change", b, d), ee(e, g), rt(v);
}
Ot(["input", "change"]);
customElements.define("xi-number", dt(
  oa,
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
var fa = /* @__PURE__ */ se('<span class="lbl"> </span>'), ua = /* @__PURE__ */ se('<label class="xi-toggle svelte-141rfru"><input type="checkbox" class="svelte-141rfru"/> <!></label>');
const ca = {
  hash: "svelte-141rfru",
  code: ".xi-toggle.svelte-141rfru {display:inline-flex;align-items:center;gap:0.4rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);cursor:pointer;}input.svelte-141rfru {accent-color:var(--xi-accent, #3b82f6);}"
};
function da(e, t) {
  nt(t, !0), ct(e, ca);
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
      n(d), O();
    },
    get label() {
      return r();
    },
    set label(d = "") {
      r(d), O();
    },
    get disabled() {
      return i();
    },
    set disabled(d = !1) {
      i(d), O();
    }
  }, o = ua(), c = X(o);
  Xn(c);
  var h = Ee(c, 2);
  {
    var _ = (d) => {
      var v = fa(), g = X(v, !0);
      B(v), ge(() => Re(g, r())), ee(d, v);
    };
    bn(h, (d) => {
      r() && d(_);
    });
  }
  return B(o), ge(() => {
    ds(c, n()), c.disabled = i();
  }), Q("change", c, l), ee(e, o), rt(a);
}
Ot(["change"]);
customElements.define("xi-toggle", dt(
  da,
  {
    value: { reflect: !0, type: "Boolean" },
    label: {},
    disabled: {}
  },
  [],
  [],
  { mode: "open" }
));
function vs(e) {
  let t = e;
  if (typeof e == "string")
    try {
      t = JSON.parse(e);
    } catch {
      t = [];
    }
  return Array.isArray(t) ? t.map((n) => n && typeof n == "object" ? { value: n.value, label: n.label ?? String(n.value) } : { value: n, label: String(n) }) : [];
}
var ha = /* @__PURE__ */ se('<span class="lbl"> </span>'), va = /* @__PURE__ */ se('<label class="opt svelte-mtcxej"><input type="radio" class="svelte-mtcxej"/> <span> </span></label>'), pa = /* @__PURE__ */ se('<div class="xi-radio svelte-mtcxej" role="radiogroup"><!> <!></div>');
const ga = {
  hash: "svelte-mtcxej",
  code: ".xi-radio.svelte-mtcxej {display:inline-flex;align-items:center;gap:0.75rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}.opt.svelte-mtcxej {display:inline-flex;align-items:center;gap:0.25rem;cursor:pointer;}input.svelte-mtcxej {accent-color:var(--xi-accent, #3b82f6);}"
};
function ma(e, t) {
  nt(t, !0), ct(e, ga);
  let n = I(t, "value", 15, ""), r = I(t, "options", 23, () => []), i = I(t, "label", 7, ""), s = I(t, "disabled", 7, !1), l = I(t, "name", 7, "xi-radio");
  const a = t.$$host, o = /* @__PURE__ */ Ci(() => vs(r()));
  function c(p) {
    n(p), a.dispatchEvent(new CustomEvent("change", { detail: { value: n() }, bubbles: !0, composed: !0 }));
  }
  var h = {
    get value() {
      return n();
    },
    set value(p = "") {
      n(p), O();
    },
    get options() {
      return r();
    },
    set options(p = []) {
      r(p), O();
    },
    get label() {
      return i();
    },
    set label(p = "") {
      i(p), O();
    },
    get disabled() {
      return s();
    },
    set disabled(p = !1) {
      s(p), O();
    },
    get name() {
      return l();
    },
    set name(p = "xi-radio") {
      l(p), O();
    }
  }, _ = pa(), d = X(_);
  {
    var v = (p) => {
      var u = ha(), b = X(u, !0);
      B(u), ge(() => Re(b, i())), ee(p, u);
    };
    bn(d, (p) => {
      i() && p(v);
    });
  }
  var g = Ee(d, 2);
  return us(g, 17, () => N(o), fs, (p, u) => {
    var b = va(), w = X(b);
    Xn(w);
    var m = Ee(w, 2), E = X(m, !0);
    B(m), B(b), ge(() => {
      Qe(w, "name", l()), Ir(w, N(u).value), ds(w, N(u).value === n()), w.disabled = s(), Re(E, N(u).label);
    }), Q("change", w, () => c(N(u).value)), ee(p, b);
  }), B(_), ee(e, _), rt(h);
}
Ot(["change"]);
customElements.define("xi-radio", dt(
  ma,
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
var _a = /* @__PURE__ */ se('<span class="lbl"> </span>'), ba = /* @__PURE__ */ se("<option> </option>"), ya = /* @__PURE__ */ se('<label class="xi-dropdown svelte-1wd9iqr"><!> <select class="svelte-1wd9iqr"></select></label>');
const wa = {
  hash: "svelte-1wd9iqr",
  code: ".xi-dropdown.svelte-1wd9iqr {display:inline-flex;align-items:center;gap:0.5rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}select.svelte-1wd9iqr {padding:0.15rem 0.3rem;}"
};
function xa(e, t) {
  nt(t, !0), ct(e, wa);
  let n = I(t, "value", 15, ""), r = I(t, "options", 23, () => []), i = I(t, "label", 7, ""), s = I(t, "disabled", 7, !1);
  const l = t.$$host, a = /* @__PURE__ */ Ci(() => vs(r()));
  function o(p) {
    n(p.target.value), l.dispatchEvent(new CustomEvent("change", { detail: { value: n() }, bubbles: !0, composed: !0 }));
  }
  var c = {
    get value() {
      return n();
    },
    set value(p = "") {
      n(p), O();
    },
    get options() {
      return r();
    },
    set options(p = []) {
      r(p), O();
    },
    get label() {
      return i();
    },
    set label(p = "") {
      i(p), O();
    },
    get disabled() {
      return s();
    },
    set disabled(p = !1) {
      s(p), O();
    }
  }, h = ya(), _ = X(h);
  {
    var d = (p) => {
      var u = _a(), b = X(u, !0);
      B(u), ge(() => Re(b, i())), ee(p, u);
    };
    bn(_, (p) => {
      i() && p(d);
    });
  }
  var v = Ee(_, 2);
  us(v, 21, () => N(a), fs, (p, u) => {
    var b = ba(), w = X(b, !0);
    B(b);
    var m = {};
    ge(() => {
      Kl(b, N(u).value === n()), Re(w, N(u).label), m !== (m = N(u).value) && (b.value = (b.__value = N(u).value) ?? "");
    }), ee(p, b);
  }), B(v);
  var g;
  return Bl(v), B(h), ge(() => {
    v.disabled = s(), g !== (g = n()) && (v.value = (v.__value = n()) ?? "", cs(v, n()));
  }), Q("change", v, o), ee(e, h), rt(c);
}
Ot(["change"]);
customElements.define("xi-dropdown", dt(
  xa,
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
var Ea = /* @__PURE__ */ se('<div class="xi-trace svelte-1m1z2sd"><span class="lbl"> </span> <canvas class="svelte-1m1z2sd"></canvas> <span class="val svelte-1m1z2sd"> </span></div>');
const $a = {
  hash: "svelte-1m1z2sd",
  code: ".xi-trace.svelte-1m1z2sd {display:inline-flex;align-items:center;gap:0.5rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}canvas.svelte-1m1z2sd {width:120px;height:28px;display:block;}.val.svelte-1m1z2sd {min-width:3em;text-align:right;font-variant-numeric:tabular-nums;}"
};
function ka(e, t) {
  nt(t, !0), ct(e, $a);
  let n = I(t, "key", 7, ""), r = I(t, "label", 7, ""), i = I(t, "max", 7, 60);
  const s = t.$$host;
  let l, a = /* @__PURE__ */ Ie(null), o = /* @__PURE__ */ Ie(pt([]));
  function c() {
    if (!l) return;
    const m = l.getContext && l.getContext("2d");
    if (!m) return;
    const E = l.width = l.clientWidth || 120, y = l.height = l.clientHeight || 28;
    if (m.clearRect(0, 0, E, y), N(o).length < 2) return;
    const $ = Math.min(...N(o)), R = Math.max(...N(o)), Y = R - $ || 1;
    m.beginPath(), N(o).forEach((L, j) => {
      const q = j / (N(o).length - 1) * (E - 2) + 1, K = y - 2 - (L - $) / Y * (y - 4);
      j ? m.lineTo(q, K) : m.moveTo(q, K);
    }), m.strokeStyle = getComputedStyle(s).getPropertyValue("--xi-accent") || "#3b82f6", m.lineWidth = 1.5, m.stroke();
  }
  function h(m) {
    const E = m && m[n()];
    E && (Me(a, E.value, !0), typeof E.value == "number" && Number.isFinite(E.value) && (Me(o, [...N(o), E.value].slice(-i()), !0), c()), s.dispatchEvent(new CustomEvent("sample", { detail: { value: E.value }, bubbles: !0, composed: !0 })));
  }
  Cr(() => {
    s.update = h, Object.defineProperty(s, "latest", { get: () => N(a), configurable: !0 }), Object.defineProperty(s, "history", { get: () => N(o).slice(), configurable: !0 }), c();
  });
  const _ = (m) => m == null ? "—" : typeof m == "number" ? Number.isInteger(m) ? m : m.toFixed(3) : String(m);
  var d = {
    get key() {
      return n();
    },
    set key(m = "") {
      n(m), O();
    },
    get label() {
      return r();
    },
    set label(m = "") {
      r(m), O();
    },
    get max() {
      return i();
    },
    set max(m = 60) {
      i(m), O();
    }
  }, v = Ea(), g = X(v), p = X(g, !0);
  B(g);
  var u = Ee(g, 2);
  Lr(u, (m) => l = m, () => l);
  var b = Ee(u, 2), w = X(b, !0);
  return B(b), B(v), ge(
    (m) => {
      Re(p, r() || n()), Re(w, m);
    },
    [() => _(N(a))]
  ), ee(e, v), rt(d);
}
customElements.define("xi-trace", dt(ka, { key: {}, label: {}, max: { type: "Number" } }, [], [], { mode: "open" }));
function ps() {
  return { scale: 1, panX: 0, panY: 0, imgW: 0, imgH: 0, viewW: 0, viewH: 0 };
}
function Pr(e, t, n) {
  return { x: (t - e.panX) / e.scale, y: (n - e.panY) / e.scale };
}
function Ta(e, t, n) {
  return { x: e.panX + t * e.scale, y: e.panY + n * e.scale };
}
const Sa = 0.05, Ca = 64, Ma = (e) => Math.max(Sa, Math.min(Ca, e));
function hr(e) {
  return !e.imgW || !e.imgH || !e.viewW || !e.viewH || (e.scale = Math.min(e.viewW / e.imgW, e.viewH / e.imgH) * 0.95, e.panX = (e.viewW - e.imgW * e.scale) / 2, e.panY = (e.viewH - e.imgH * e.scale) / 2), e;
}
function Aa(e) {
  return e.scale = 1, e.panX = (e.viewW - e.imgW) / 2, e.panY = (e.viewH - e.imgH) / 2, e;
}
function gs(e, t, n, r) {
  const { x: i, y: s } = Pr(e, t, n);
  return e.scale = Ma(e.scale * r), e.panX = t - i * e.scale, e.panY = n - s * e.scale, e;
}
function Na(e, t, n) {
  return e.panX += t, e.panY += n, e;
}
var Oa = /* @__PURE__ */ se('<canvas class="svelte-1yjweo0"></canvas>');
const Ra = {
  hash: "svelte-1yjweo0",
  code: ":host {display:block;width:100%;height:100%;}canvas.svelte-1yjweo0 {width:100%;height:100%;display:block;background:var(--xi-viewer-bg, #111);cursor:grab;touch-action:none;image-rendering:pixelated;}canvas.svelte-1yjweo0:active {cursor:grabbing;}"
};
function Ia(e, t) {
  nt(t, !0), ct(e, Ra);
  const n = t.$$host;
  let r;
  const i = ps();
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
  function c(y, $) {
    n.dispatchEvent(new CustomEvent(y, { detail: $, bubbles: !0, composed: !0 }));
  }
  function h(y) {
    return !!y && typeof y != "string" && !("dataUrl" in y) && (typeof HTMLImageElement < "u" && y instanceof HTMLImageElement || typeof HTMLCanvasElement < "u" && y instanceof HTMLCanvasElement || typeof OffscreenCanvas < "u" && y instanceof OffscreenCanvas || typeof ImageBitmap < "u" && y instanceof ImageBitmap);
  }
  function _(y) {
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
    gs(i, y.clientX - $.left, y.clientY - $.top, y.deltaY < 0 ? 1.15 : 1 / 1.15), a(), c("viewchange", { scale: i.scale });
  }
  let g = null, p = !1;
  function u(y) {
    var $;
    s && (g = { x: y.clientX, y: y.clientY }, p = !1, ($ = r.setPointerCapture) == null || $.call(r, y.pointerId));
  }
  function b(y) {
    if (!g) return;
    const $ = y.clientX - g.x, R = y.clientY - g.y;
    ($ || R) && (p = !0), Na(i, $, R), g = { x: y.clientX, y: y.clientY }, a();
  }
  function w(y) {
    g && !p && m(y), g = null;
  }
  function m(y) {
    if (!s || !l) return;
    const $ = r.getBoundingClientRect(), R = Pr(i, y.clientX - $.left, y.clientY - $.top), Y = Math.floor(R.x), L = Math.floor(R.y);
    let j = null;
    if (Y >= 0 && L >= 0 && Y < i.imgW && L < i.imgH) {
      const q = l.getContext("2d").getImageData(Y, L, 1, 1).data;
      j = [q[0], q[1], q[2]];
    }
    c("pixelpick", { x: Y, y: L, rgb: j });
  }
  Cr(() => {
    n.setFrame = _, n.fit = () => {
      hr(i), a(), c("viewchange", { scale: i.scale });
    }, n.oneToOne = () => {
      Aa(i), a(), c("viewchange", { scale: i.scale });
    }, o();
    const y = new ResizeObserver(o);
    return y.observe(r), () => y.disconnect();
  });
  var E = Oa();
  Lr(E, (y) => r = y, () => r), ls("wheel", E, v), Q("pointerdown", E, u), Q("pointermove", E, b), Q("pointerup", E, w), ee(e, E), rt();
}
Ot(["pointerdown", "pointermove", "pointerup"]);
customElements.define("xi-image-viewer", dt(Ia, {}, [], [], { mode: "open" }));
function Da() {
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
function La() {
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
function Pa() {
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
const vr = { point: Da, rect: La, polygon: Pa };
function eo(e, t) {
  vr[e] = t;
}
function ni(e) {
  return vr[e] ? vr[e]() : null;
}
var Ha = /* @__PURE__ */ se('<div class="xi-editor svelte-1qrk1oj"><div class="bar svelte-1qrk1oj"><span class="lbl"> </span> <span class="spacer svelte-1qrk1oj"></span> <button class="cancel svelte-1qrk1oj">Cancel</button> <button class="commit svelte-1qrk1oj">Commit</button></div> <canvas class="svelte-1qrk1oj"></canvas></div>');
const ja = {
  hash: "svelte-1qrk1oj",
  code: ":host {display:block;width:100%;height:100%;}.xi-editor.svelte-1qrk1oj {display:flex;flex-direction:column;height:100%;}.bar.svelte-1qrk1oj {display:flex;align-items:center;gap:0.5rem;padding:0.35rem 0.5rem;font:13px system-ui, sans-serif;background:var(--xi-bar-bg, #1f2937);color:#e5e7eb;}.spacer.svelte-1qrk1oj {flex:1;}button.svelte-1qrk1oj {padding:0.2rem 0.7rem;border:0;border-radius:4px;cursor:pointer;font:inherit;}.commit.svelte-1qrk1oj {background:var(--xi-accent, #6366f1);color:#fff;}.cancel.svelte-1qrk1oj {background:#374151;color:#e5e7eb;}canvas.svelte-1qrk1oj {flex:1;width:100%;display:block;background:var(--xi-viewer-bg, #111);cursor:crosshair;touch-action:none;image-rendering:pixelated;}"
};
function Fa(e, t) {
  nt(t, !0), ct(e, ja);
  let n = I(t, "tool", 7, "rect"), r = I(t, "label", 7, "");
  const i = t.$$host;
  let s;
  const l = ps();
  let a = null, o = ni(n());
  const c = (x) => Ta(l, x.x, x.y);
  function h() {
    if (!s) return;
    const x = s.getContext("2d");
    x && (x.imageSmoothingEnabled = !1, x.setTransform(1, 0, 0, 1, 0, 0), x.clearRect(0, 0, s.width, s.height), a && (x.setTransform(l.scale, 0, 0, l.scale, l.panX, l.panY), x.drawImage(a, 0, 0), x.setTransform(1, 0, 0, 1, 0, 0)), o && o.draw(x, c));
  }
  function _() {
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
  const u = (x) => {
    const le = s.getBoundingClientRect();
    return Pr(l, x.clientX - le.left, x.clientY - le.top);
  };
  function b(x) {
    o && (o.onDown(u(x)), h());
  }
  function w(x) {
    o && x.buttons && (o.onMove(u(x)), h());
  }
  function m(x) {
    o && (o.onUp(u(x)), h());
  }
  function E(x) {
    o && (o.onDbl(u(x)), h());
  }
  function y(x) {
    if (!a) return;
    x.preventDefault();
    const le = s.getBoundingClientRect();
    gs(l, x.clientX - le.left, x.clientY - le.top, x.deltaY < 0 ? 1.15 : 1 / 1.15), h();
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
    i.setFrame = v, i.setTool = p, i.getResult = () => o && o.done() ? o.result() : null, _();
    const x = new ResizeObserver(_);
    return x.observe(s), () => x.disconnect();
  });
  var Y = {
    get tool() {
      return n();
    },
    set tool(x = "rect") {
      n(x), O();
    },
    get label() {
      return r();
    },
    set label(x = "") {
      r(x), O();
    }
  }, L = Ha(), j = X(L), q = X(j), K = X(q, !0);
  B(q);
  var ht = Ee(q, 4), It = Ee(ht, 2);
  B(j);
  var ze = Ee(j, 2);
  return Lr(ze, (x) => s = x, () => s), B(L), ge(() => Re(K, r() || n())), Q("click", ht, R), Q("click", It, $), Q("pointerdown", ze, b), Q("pointermove", ze, w), Q("pointerup", ze, m), Q("dblclick", ze, E), ls("wheel", ze, y), ee(e, L), rt(Y);
}
Ot([
  "click",
  "pointerdown",
  "pointermove",
  "pointerup",
  "dblclick"
]);
customElements.define("xi-image-editor", dt(Fa, { tool: {}, label: {} }, [], [], { mode: "open" }));
class to {
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
      binary: /* @__PURE__ */ new Set()
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
const Wa = {
  slider: "xi-slider",
  number: "xi-number",
  toggle: "xi-toggle",
  radio: "xi-radio",
  dropdown: "xi-dropdown"
};
function Ya(e, { section: t = "Config", tag: n = "control" } = {}) {
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
async function no(e, t) {
  const { client: n, instance: r, sectionFilter: i } = t, s = e.ownerDocument || globalThis.document, l = await n.getInstanceDef(r) || {}, a = { ...l }, o = t.descriptor && t.descriptor.length ? t.descriptor : Ya(l), c = [];
  e.innerHTML = "";
  for (const h of o) {
    if (i && !i(h)) continue;
    const _ = s.createElement("section");
    if (_.className = "xi-section", _.dataset.tag = h.tag || "control", h.section) {
      const d = s.createElement("h3");
      d.className = "xi-section-title", d.textContent = h.section, _.appendChild(d);
    }
    for (const d of h.controls || []) {
      const v = Wa[d.type] || "xi-number", g = s.createElement(v);
      d.label && g.setAttribute("label", d.label);
      for (const u of ["min", "max", "step"]) d[u] != null && g.setAttribute(u, String(d[u]));
      const p = s.createElement("div");
      p.className = "xi-control", p.appendChild(g), _.appendChild(p), d.options != null && (g.options = d.options), d.key in a && (g.value = a[d.key]), g.addEventListener("change", async (u) => {
        a[d.key] = u.detail.value;
        try {
          await n.setInstanceDef(r, { ...a });
        } catch {
        }
        e.dispatchEvent(new CustomEvent("xi-change", { detail: { key: d.key, value: u.detail.value }, bubbles: !0 }));
      }), c.push({ el: g, key: d.key });
    }
    e.appendChild(_);
  }
  return {
    // Re-read the def from the backend and push values back into the controls.
    async refresh() {
      const h = await n.getInstanceDef(r) || {};
      Object.assign(a, h);
      for (const { el: _, key: d } of c) d in a && (_.value = a[d]);
    },
    destroy() {
      e.innerHTML = "";
    }
  };
}
const qa = `
  :host { display:block; height:100%; box-sizing:border-box; background:#1e1e1e;
          border:1px solid #333; border-radius:6px; color:#ddd; overflow:hidden;
          font:13px/1.4 system-ui,sans-serif; }
  .hd { padding:4px 8px; font-size:11px; letter-spacing:.04em; text-transform:uppercase;
        color:#888; border-bottom:1px solid #2a2a2a; }
  .body { padding:8px; height:calc(100% - 24px); box-sizing:border-box; }
`;
function Vn(e, t) {
  return e.attachShadow({ mode: "open" }), e.shadowRoot.innerHTML = `<style>${qa}</style>
    <div class="hd">${t || ""}</div><div class="body"></div>`, e.shadowRoot.querySelector(".body");
}
const za = (e, t) => e.config && e.config.title || t;
function ms(e) {
  return e == null ? { kind: "none", label: "—", color: "#bbb" } : e <= -99e4 ? { kind: "sys", label: "SYS", color: "#c084fc" } : e > 0 ? { kind: "ok", label: e > 1 ? `OK${e}` : "OK", color: "#3ad17a" } : e < 0 ? { kind: "ng", label: e < -1 ? `NG${-e}` : "NG", color: "#ff5b5b" } : { kind: "na", label: "NA", color: "#ffb454" };
}
class Ba extends HTMLElement {
  connectedCallback() {
    this.body = Vn(this, za(this, "Verdict")), this.body.style.cssText = "display:flex;flex-direction:column;align-items:center;justify-content:center;gap:6px;text-align:center", this.big = document.createElement("div"), this.big.style.cssText = "font-weight:800;font-size:clamp(20px,7vw,72px);line-height:1", this.sub = document.createElement("div"), this.sub.style.cssText = "font-size:12px;color:#888;max-width:96%;overflow:hidden;text-overflow:ellipsis;white-space:nowrap", this.body.append(this.big, this.sub);
  }
  feed(t) {
    const n = t.result, r = ms(n ? n.code : null);
    this.big.textContent = r.label, this.big.style.color = r.color, this.sub.textContent = n && n.msg ? n.msg : "";
  }
}
class Xa extends HTMLElement {
  connectedCallback() {
    var t, e;
    this.body = Vn(this, ((t = this.config) == null ? void 0 : t.title) || "Throughput"), this.windowSec = ((e = this.config) == null ? void 0 : e.windowSec) || 60, this.stamps = [], this.lastResult = -1, this.lastCompute = null, this.body.style.cssText = "display:flex;flex-direction:column;align-items:center;justify-content:center;gap:4px", this.big = document.createElement("div"), this.big.style.cssText = "font-size:clamp(18px,6vw,44px);font-weight:700;color:#9ad", this.sub = document.createElement("div"), this.sub.style.cssText = "font-size:12px;color:#888", this.body.append(this.big, this.sub), this.timer = setInterval(() => this.render(), 1e3);
  }
  disconnectedCallback() { this.timer && (clearInterval(this.timer), this.timer = 0); }
  feed(t) {
    const e = t.result;
    e && e.run_id != null && e.run_id !== this.lastResult && (this.lastResult = e.run_id, this.stamps.push(Date.now())), t.run_ms != null && (this.lastCompute = t.run_ms), this.render();
  }
  render() {
    var i;
    const t = Date.now(), e = t - this.windowSec * 1e3;
    for (; this.stamps.length && this.stamps[0] < e;) this.stamps.shift();
    const n = this.stamps.length, r = n ? Math.max((t - this.stamps[0]) / 1e3, 1) : this.windowSec, s = n > 1 ? n / r * 60 : 0;
    this.big.textContent = `${s.toFixed(0)} /min`, this.sub.textContent = `${n} in ${this.windowSec}s` + (this.lastCompute != null ? ` · compute ${((i = this.lastCompute.toFixed) == null ? void 0 : i.call(this.lastCompute, 1)) ?? this.lastCompute} ms` : "");
  }
}
class Va extends HTMLElement {
  connectedCallback() {
    var t;
    this.body = Vn(this, ((t = this.config) == null ? void 0 : t.title) || "Yield"), this.ok = 0, this.ng = 0, this.last = -1, this.body.style.cssText = "display:flex;flex-direction:column;align-items:center;justify-content:center;gap:4px", this.big = document.createElement("div"), this.big.style.cssText = "font-size:clamp(18px,6vw,44px);font-weight:700", this.sub = document.createElement("div"), this.sub.style.cssText = "font-size:12px;color:#888", this.body.append(this.big, this.sub);
  }
  feed(t) {
    var s;
    const n = t.result;
    if (n && n.run_id != null && n.run_id !== this.last) {
      this.last = n.run_id;
      const l = ms(n.code);
      l.kind === "ok" ? this.ok++ : l.kind === "ng" ? this.ng++ : l.kind === "na" && (this.na = (this.na || 0) + 1);
    }
    const r = this.ok + this.ng, i = r ? 100 * this.ok / r : 0;
    this.big.textContent = `${i.toFixed(1)}%`, this.big.style.color = i >= (((s = this.config) == null ? void 0 : s.warn) ?? 95) ? "#3ad17a" : "#ffb454", this.sub.textContent = `OK ${this.ok} / NG ${this.ng}` + (this.na ? ` · NA ${this.na}` : "");
  }
}
class Ua extends HTMLElement {
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
        const c = document.createElement("span");
        c.style.color = "#888", a.append(o, c);
        const h = document.createElement("div");
        h.style.cssText = "display:flex;gap:3px;height:18px", l.append(a, h), this.body.appendChild(l), this.rows[r.name] = l = { row: l, name: o, meta: c, bar: h, cells: [] };
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
const _s = {
  verdict: Ba,
  throughput: Xa,
  yield: Va,
  groups: Ua
};
for (const [e, t] of Object.entries(_s)) customElements.define(`xi-card-${e}`, t);
const Hr = (e) => !!(e && e.card), Rt = (e) => !!(e && (e.dir === "row" || e.dir === "col") && Array.isArray(e.children) && e.children.length >= 1), qe = (e) => !!(e && Array.isArray(e.tabs) && e.tabs.length >= 1 && e.tabs.every((t) => t && t.child)), yn = () => ({ type: "verdict", bind: {}, config: { title: "(empty)" } });
function jr(e) {
  const t = e.children.length;
  return (Array.isArray(e.weights) && e.weights.length === t ? e.weights.slice() : Array(t).fill(1)).map((r) => typeof r == "number" && r > 0 ? r : 1);
}
function Ga(e) {
  const t = jr(e), n = t.reduce((r, i) => r + i, 0) || 1;
  return t.map((r) => r / n);
}
function bs(e, t) {
  return qe(e) ? e.tabs[t].child : e.children[t];
}
function Ka(e, t, n) {
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
function ro(e) {
  let t = 0;
  return pr(e, () => t++), t;
}
function Ja(e, t) {
  let n = e;
  for (const r of t)
    if (Rt(n) || qe(n)) n = bs(n, r);
    else return;
  return n;
}
function ke(e, t, n) {
  if (t.length === 0) return n(e);
  const [r, ...i] = t;
  return Ka(e, r, ke(bs(e, r), i, n));
}
function io(e, t, n, r = yn()) {
  return ke(e, t, (i) => ({ dir: n === "col" ? "col" : "row", children: [i, { card: r }], weights: [1, 1] }));
}
function so(e, t, n, r = yn()) {
  if (n = n === "col" ? "col" : "row", t.length === 0) return { dir: n, children: [e, { card: r }], weights: [1, 1] };
  const i = t.slice(0, -1), s = t[t.length - 1], l = Ja(e, i);
  return Rt(l) && l.dir === n ? ke(e, i, (a) => {
    const o = a.children.slice();
    o.splice(s + 1, 0, { card: r });
    const c = jr(a);
    return c.splice(s + 1, 0, c[s]), { ...a, children: o, weights: c };
  }) : ke(e, t, (a) => ({ dir: n, children: [a, { card: r }], weights: [1, 1] }));
}
function lo(e, t) {
  if (t.length === 0) return { card: yn() };
  const n = t.slice(0, -1), r = t[t.length - 1];
  return ke(e, n, (i) => {
    if (!Rt(i)) return i;
    const s = i.children.filter((a, o) => o !== r), l = jr(i).filter((a, o) => o !== r);
    return s.length === 1 ? s[0] : { ...i, children: s, weights: l };
  });
}
function ao(e, t, n) {
  return ke(e, t, () => ({ card: n }));
}
function oo(e, t, n) {
  return ke(e, t, (r) => Rt(r) ? { ...r, weights: n.slice() } : r);
}
function fo(e, t) {
  return ke(e, t, (n) => ({ tabs: [{ name: "Page 1", child: n }, { name: "Page 2", child: { card: yn() } }], active: 0 }));
}
function uo(e, t, n, r = { card: yn() }) {
  return ke(e, t, (i) => qe(i) ? { ...i, tabs: [...i.tabs, { name: n || `Page ${i.tabs.length + 1}`, child: r }], active: i.tabs.length } : i);
}
function co(e, t, n) {
  return ke(e, t, (r) => {
    if (!qe(r)) return r;
    const i = r.tabs.filter((s, l) => l !== n);
    return i.length === 1 ? i[0].child : { ...r, tabs: i, active: Math.max(0, Math.min(r.active || 0, i.length - 1)) };
  });
}
function ho(e, t, n, r) {
  return ke(e, t, (i) => qe(i) ? { ...i, tabs: i.tabs.map((s, l) => l === n ? { ...s, name: r } : s) } : i);
}
function vo(e, t, n) {
  return ke(e, t, (r) => qe(r) ? { ...r, active: n } : r);
}
function ri(e, t = "root") {
  return Hr(e) ? e.card.type ? [] : [`${t}: leaf has no card.type`] : Rt(e) ? e.children.flatMap((n, r) => ri(n, `${t}.${r}`)) : qe(e) ? e.tabs.flatMap((n, r) => ri(n.child, `${t}.${n.name || r}`)) : [`${t}: node is not a leaf {card}, split {dir,children}, or tabs {tabs}`];
}
function po(e, { client: t, dashboard: n, pollStatsMs: r = 200 }) {
  const i = e.ownerDocument || globalThis.document, s = globalThis.requestAnimationFrame || ((u) => setTimeout(u, 16)), l = { run_id: -1, run_ms: null, status: null, result: null, groups: [] };
  let a = [], o = 0;
  function c() {
    o || (o = s(() => {
      o = 0;
      for (const u of a)
        try {
          u.feed(l);
        } catch {
        }
    }));
  }
  function h(u) {
    const b = _s[u.type], w = i.createElement(b ? `xi-card-${u.type}` : "div");
    return b || (w.textContent = `unknown card: ${u.type}`, w.style.cssText = "color:#f88;padding:8px"), w.binding = u.bind || {}, w.config = u.config || {}, w.style.minWidth = "0", w.style.minHeight = "0", w.style.overflow = "hidden", b && a.push(w), w;
  }
  function _(u) {
    let b = Math.min(u.active || 0, u.tabs.length - 1);
    const w = i.createElement("div");
    w.style.cssText = "display:flex;flex-direction:column;min-width:0;min-height:0;width:100%;height:100%";
    const m = i.createElement("div");
    m.style.cssText = "display:flex;gap:2px;flex:0 0 auto;align-items:center;padding:2px 2px 0;overflow:auto";
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
    return u.tabs.forEach((Y, L) => {
      const j = i.createElement("div");
      j.style.cssText = "padding:4px 11px;cursor:pointer;border-radius:6px 6px 0 0;border:1px solid #333;border-bottom:none;font:12px system-ui,sans-serif", j.textContent = Y.name || `Page ${L + 1}`, j.onclick = () => {
        b = L, R();
      }, y.push(j), m.appendChild(j);
      const q = d(Y.child);
      q.style.cssText += ";position:absolute;inset:0;min-width:0;min-height:0;border:1px solid #333;border-radius:0 6px 6px 6px;overflow:hidden", $.push(q), E.appendChild(q);
    }), R(), w.append(m, E), w;
  }
  function d(u) {
    if (Hr(u)) return h(u.card);
    if (qe(u)) return _(u);
    if (!Rt(u)) {
      const E = i.createElement("div");
      return E.textContent = "bad layout node", E.style.color = "#f88", E;
    }
    const b = u.dir === "col", w = i.createElement("div");
    w.style.cssText = `display:flex;flex-direction:${b ? "column" : "row"};min-width:0;min-height:0;width:100%;height:100%`;
    const m = Ga(u);
    return u.children.forEach((E, y) => {
      const $ = d(E);
      $.style.flex = `${m[y]} 1 0`, $.style.minWidth = "0", $.style.minHeight = "0", w.appendChild($);
    }), w;
  }
  function v() {
    a = [], e.replaceChildren(), e.style.cssText += ";display:flex;min-width:0;min-height:0";
    const u = n && n.layout;
    if (!u) return;
    const b = d(u);
    b.style.flex = "1 1 0", b.style.minWidth = "0", b.style.minHeight = "0", e.appendChild(b), c();
  }
  const g = [
    t.onEvent((u) => {
      u.name === "run_finished" && u.data ? (typeof u.data.run_id == "number" && (l.run_id = u.data.run_id), typeof u.data.ms == "number" && (l.run_ms = u.data.ms), c()) : u.name === "run_result" && u.data ? (l.result = u.data, c()) : (u.name === "safe_state" || u.name === "status") && (l.status = u.data, c());
    })
  ], p = setInterval(() => {
    t.cmd("dispatch_stats").then((u) => {
      u && Array.isArray(u.groups) && (l.groups = u.groups, c());
    }).catch(() => {
    });
  }, r);
  return v(), {
    setDashboard(u) {
      n = u, v();
    },
    state: l,
    destroy() {
      g.forEach((u) => u()), clearInterval(p), e.replaceChildren();
    }
  };
}
const go = [
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
  _s as CARDS,
  Wa as CONTROL_TAGS,
  vr as TOOLS,
  go as XI_COMPONENTS,
  to as XiClient,
  so as addSibling,
  uo as addTab,
  ro as countLeaves,
  pr as eachLeaf,
  yn as emptyCard,
  Ja as getNode,
  Ya as inferDescriptor,
  Hr as isLeaf,
  Rt as isSplit,
  qe as isTabs,
  ni as makeTool,
  po as mountDashboard,
  no as mountPanel,
  eo as registerTool,
  lo as removePane,
  co as removeTab,
  ho as renameTab,
  vo as setActive,
  ao as setCard,
  oo as setWeights,
  io as splitLeaf,
  ri as validate,
  Ga as weightsOf,
  fo as wrapInTabs
};
