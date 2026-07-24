var ha = Object.defineProperty;
var ui = (t) => {
  throw TypeError(t);
};
var pa = (t, e, n) => e in t ? ha(t, e, { enumerable: !0, configurable: !0, writable: !0, value: n }) : t[e] = n;
var J = (t, e, n) => pa(t, typeof e != "symbol" ? e + "" : e, n), fr = (t, e, n) => e.has(t) || ui("Cannot " + n);
var v = (t, e, n) => (fr(t, e, "read from private field"), n ? n.call(t) : e.get(t)), H = (t, e, n) => e.has(t) ? ui("Cannot add the same private member more than once") : e instanceof WeakSet ? e.add(t) : e.set(t, n), L = (t, e, n, r) => (fr(t, e, "write to private field"), r ? r.call(t, n) : e.set(t, n), n), z = (t, e, n) => (fr(t, e, "access private method"), n);
var zi;
typeof window < "u" && ((zi = window.__svelte ?? (window.__svelte = {})).v ?? (zi.v = /* @__PURE__ */ new Set())).add("5");
const va = 1, ma = 2, Vi = 4, ga = 8, ba = 16, xa = 1, ya = 4, _a = 8, wa = 16, Ea = 1, ka = 2, Ui = "[", Pr = "[!", fi = "[?", Hr = "]", nn = {}, re = Symbol("uninitialized"), Ca = "http://www.w3.org/1999/xhtml", Yi = !1;
var Br = Array.isArray, Ta = Array.prototype.indexOf, Un = Array.prototype.includes, ir = Array.from, Yn = Object.keys, Xn = Object.defineProperty, Lt = Object.getOwnPropertyDescriptor, $a = Object.getOwnPropertyDescriptors, Na = Object.prototype, Sa = Array.prototype, Xi = Object.getPrototypeOf, di = Object.isExtensible;
const Aa = () => {
};
function Ma(t) {
  for (var e = 0; e < t.length; e++)
    t[e]();
}
function qi() {
  var t, e, n = new Promise((r, i) => {
    t = r, e = i;
  });
  return { promise: n, resolve: t, reject: e };
}
const ce = 2, rn = 4, sr = 8, Gi = 1 << 24, Ue = 16, qe = 32, vt = 64, br = 128, Re = 512, ie = 1024, se = 2048, tt = 4096, ve = 8192, Ne = 16384, zt = 32768, xr = 1 << 25, sn = 65536, qn = 1 << 17, Ia = 1 << 18, jt = 1 << 19, Da = 1 << 20, Ze = 1 << 25, Bt = 65536, Gn = 1 << 21, qt = 1 << 22, Et = 1 << 23, Ot = Symbol("$state"), Ki = Symbol("legacy props"), La = Symbol(""), Hn = Symbol("attributes"), yr = Symbol("class"), _r = Symbol("style"), pn = Symbol("text"), Ji = Symbol("form reset"), ar = new class extends Error {
  constructor() {
    super(...arguments);
    J(this, "name", "StaleReactionError");
    J(this, "message", "The reaction that called `getAbortSignal()` was re-run or destroyed");
  }
}();
var ji;
const Zi = (
  // We gotta write it like this because after downleveling the pure comment may end up in the wrong location
  !!((ji = globalThis.document) != null && ji.contentType) && /* @__PURE__ */ globalThis.document.contentType.includes("xml")
), Fr = 3, An = 8;
function Oa() {
  throw new Error("https://svelte.dev/e/async_derived_orphan");
}
function Ra(t, e, n) {
  throw new Error("https://svelte.dev/e/each_key_duplicate");
}
function Pa(t) {
  throw new Error("https://svelte.dev/e/effect_in_teardown");
}
function Ha() {
  throw new Error("https://svelte.dev/e/effect_in_unowned_derived");
}
function Ba(t) {
  throw new Error("https://svelte.dev/e/effect_orphan");
}
function Fa() {
  throw new Error("https://svelte.dev/e/effect_update_depth_exceeded");
}
function za() {
  throw new Error("https://svelte.dev/e/hydration_failed");
}
function ja(t) {
  throw new Error("https://svelte.dev/e/props_invalid_value");
}
function Wa() {
  throw new Error("https://svelte.dev/e/state_descriptors_fixed");
}
function Va() {
  throw new Error("https://svelte.dev/e/state_prototype_fixed");
}
function Ua() {
  throw new Error("https://svelte.dev/e/state_unsafe_mutation");
}
function Ya() {
  throw new Error("https://svelte.dev/e/svelte_boundary_reset_onerror");
}
function Xa() {
  console.warn("https://svelte.dev/e/derived_inert");
}
function lr(t) {
  console.warn("https://svelte.dev/e/hydration_mismatch");
}
function qa() {
  console.warn("https://svelte.dev/e/select_multiple_invalid_value");
}
function Ga() {
  console.warn("https://svelte.dev/e/svelte_boundary_reset_noop");
}
let j = !1;
function Ye(t) {
  j = t;
}
let G;
function Se(t) {
  if (t === null)
    throw lr(), nn;
  return G = t;
}
function Mn() {
  return Se(/* @__PURE__ */ bt(G));
}
function O(t) {
  if (j) {
    if (/* @__PURE__ */ bt(G) !== null)
      throw lr(), nn;
    G = t;
  }
}
function Ka(t = 1) {
  if (j) {
    for (var e = t, n = G; e--; )
      n = /** @type {TemplateNode} */
      /* @__PURE__ */ bt(n);
    G = n;
  }
}
function Kn(t = !0) {
  for (var e = 0, n = G; ; ) {
    if (n.nodeType === An) {
      var r = (
        /** @type {Comment} */
        n.data
      );
      if (r === Hr) {
        if (e === 0) return n;
        e -= 1;
      } else (r === Ui || r === Pr || // "[1", "[2", etc. for if blocks
      r[0] === "[" && !isNaN(Number(r.slice(1)))) && (e += 1);
    }
    var i = (
      /** @type {TemplateNode} */
      /* @__PURE__ */ bt(n)
    );
    t && n.remove(), n = i;
  }
}
function Qi(t) {
  if (!t || t.nodeType !== An)
    throw lr(), nn;
  return (
    /** @type {Comment} */
    t.data
  );
}
function es(t) {
  return t === this.v;
}
function Ja(t, e) {
  return t != t ? e == e : t !== e || t !== null && typeof t == "object" || typeof t == "function";
}
function ts(t) {
  return !Ja(t, this.v);
}
let Za = !1, me = null;
function an(t) {
  me = t;
}
function ae(t, e = !1, n) {
  me = {
    p: me,
    i: !1,
    c: null,
    e: null,
    s: t,
    x: null,
    r: (
      /** @type {Effect} */
      B
    ),
    l: null
  };
}
function le(t) {
  var e = (
    /** @type {ComponentContext} */
    me
  ), n = e.e;
  if (n !== null) {
    e.e = null;
    for (var r of n)
      Ns(r);
  }
  return t !== void 0 && (e.x = t), e.i = !0, me = e.p, t ?? /** @type {T} */
  {};
}
function ns() {
  return !0;
}
let Ct = [];
function rs() {
  var t = Ct;
  Ct = [], Ma(t);
}
function pt(t) {
  if (Ct.length === 0 && !yn) {
    var e = Ct;
    queueMicrotask(() => {
      e === Ct && rs();
    });
  }
  Ct.push(t);
}
function Qa() {
  for (; Ct.length > 0; )
    rs();
}
function is(t) {
  var e = B;
  if (e === null)
    return F.f |= Et, t;
  if ((e.f & zt) === 0 && (e.f & rn) === 0)
    throw t;
  wt(t, e);
}
function wt(t, e) {
  if (!(e !== null && (e.f & Ne) !== 0)) {
    for (; e !== null; ) {
      if ((e.f & br) !== 0) {
        if ((e.f & zt) === 0)
          throw t;
        try {
          e.b.error(t);
          return;
        } catch (n) {
          t = n;
        }
      }
      e = e.parent;
    }
    throw t;
  }
}
const el = -7169;
function Q(t, e) {
  t.f = t.f & el | e;
}
function zr(t) {
  (t.f & Re) !== 0 || t.deps === null ? Q(t, ie) : Q(t, tt);
}
function ss(t) {
  if (t !== null)
    for (const e of t)
      (e.f & ce) === 0 || (e.f & Bt) === 0 || (e.f ^= Bt, ss(
        /** @type {Derived} */
        e.deps
      ));
}
function as(t, e, n) {
  (t.f & se) !== 0 ? e.add(t) : (t.f & tt) !== 0 && n.add(t), ss(t.deps), Q(t, ie);
}
let On = !1;
function tl(t) {
  var e = On;
  try {
    return On = !1, [t(), On];
  } finally {
    On = e;
  }
}
function nl(t) {
  let e = 0, n = Ft(0), r;
  return () => {
    Xr() && (I(n), Gr(() => (e === 0 && (r = Qr(() => t(() => _n(n)))), e += 1, () => {
      pt(() => {
        e -= 1, e === 0 && (r == null || r(), r = void 0, _n(n));
      });
    })));
  };
}
var rl = sn | jt;
function il(t, e, n, r) {
  new sl(t, e, n, r);
}
var ke, kn, Ie, St, xe, De, he, Ce, lt, At, yt, Gt, Cn, Tn, ot, tr, K, ls, os, cs, wr, Bn, Fn, Er, kr;
class sl {
  /**
   * @param {TemplateNode} node
   * @param {BoundaryProps} props
   * @param {((anchor: Node) => void)} children
   * @param {((error: unknown) => unknown) | undefined} [transform_error]
   */
  constructor(e, n, r, i) {
    H(this, K);
    /** @type {Boundary | null} */
    J(this, "parent");
    J(this, "is_pending", !1);
    /**
     * API-level transformError transform function. Transforms errors before they reach the `failed` snippet.
     * Inherited from parent boundary, or defaults to identity.
     * @type {(error: unknown) => unknown}
     */
    J(this, "transform_error");
    /** @type {TemplateNode} */
    H(this, ke);
    /** @type {TemplateNode | null} */
    H(this, kn, j ? G : null);
    /** @type {BoundaryProps} */
    H(this, Ie);
    /** @type {((anchor: Node) => void)} */
    H(this, St);
    /** @type {Effect} */
    H(this, xe);
    /** @type {Effect | null} */
    H(this, De, null);
    /** @type {Effect | null} */
    H(this, he, null);
    /** @type {Effect | null} */
    H(this, Ce, null);
    /** @type {DocumentFragment | null} */
    H(this, lt, null);
    H(this, At, 0);
    H(this, yt, 0);
    H(this, Gt, !1);
    /** @type {Set<Effect>} */
    H(this, Cn, /* @__PURE__ */ new Set());
    /** @type {Set<Effect>} */
    H(this, Tn, /* @__PURE__ */ new Set());
    /**
     * A source containing the number of pending async deriveds/expressions.
     * Only created if `$effect.pending()` is used inside the boundary,
     * otherwise updating the source results in needless `Batch.ensure()`
     * calls followed by no-op flushes
     * @type {Source<number> | null}
     */
    H(this, ot, null);
    H(this, tr, nl(() => (L(this, ot, Ft(v(this, At))), () => {
      L(this, ot, null);
    })));
    var s;
    L(this, ke, e), L(this, Ie, n), L(this, St, (a) => {
      var l = (
        /** @type {Effect} */
        B
      );
      l.b = this, l.f |= br, r(a);
    }), this.parent = /** @type {Effect} */
    B.b, this.transform_error = i ?? ((s = this.parent) == null ? void 0 : s.transform_error) ?? ((a) => a), L(this, xe, Kr(() => {
      if (j) {
        const a = (
          /** @type {Comment} */
          v(this, kn)
        );
        Mn();
        const l = a.data === Pr;
        if (a.data.startsWith(fi)) {
          const c = JSON.parse(a.data.slice(fi.length));
          z(this, K, os).call(this, c);
        } else l ? z(this, K, cs).call(this) : z(this, K, ls).call(this);
      } else
        z(this, K, wr).call(this);
    }, rl)), j && L(this, ke, G);
  }
  /**
   * Defer an effect inside a pending boundary until the boundary resolves
   * @param {Effect} effect
   */
  defer_effect(e) {
    as(e, v(this, Cn), v(this, Tn));
  }
  /**
   * Returns `false` if the effect exists inside a boundary whose pending snippet is shown
   * @returns {boolean}
   */
  is_rendered() {
    return !this.is_pending && (!this.parent || this.parent.is_rendered());
  }
  has_pending_snippet() {
    return !!v(this, Ie).pending;
  }
  /**
   * Update the source that powers `$effect.pending()` inside this boundary,
   * and controls when the current `pending` snippet (if any) is removed.
   * Do not call from inside the class
   * @param {1 | -1} d
   * @param {Batch} batch
   */
  update_pending_count(e, n) {
    z(this, K, Er).call(this, e, n), L(this, At, v(this, At) + e), !(!v(this, ot) || v(this, Gt)) && (L(this, Gt, !0), pt(() => {
      L(this, Gt, !1), v(this, ot) && ln(v(this, ot), v(this, At));
    }));
  }
  get_effect_pending() {
    return v(this, tr).call(this), I(
      /** @type {Source<number>} */
      v(this, ot)
    );
  }
  /** @param {unknown} error */
  error(e) {
    if (!v(this, Ie).onerror && !v(this, Ie).failed)
      throw e;
    R != null && R.is_fork ? (v(this, De) && R.skip_effect(v(this, De)), v(this, he) && R.skip_effect(v(this, he)), v(this, Ce) && R.skip_effect(v(this, Ce)), R.oncommit(() => {
      z(this, K, kr).call(this, e);
    })) : z(this, K, kr).call(this, e);
  }
}
ke = new WeakMap(), kn = new WeakMap(), Ie = new WeakMap(), St = new WeakMap(), xe = new WeakMap(), De = new WeakMap(), he = new WeakMap(), Ce = new WeakMap(), lt = new WeakMap(), At = new WeakMap(), yt = new WeakMap(), Gt = new WeakMap(), Cn = new WeakMap(), Tn = new WeakMap(), ot = new WeakMap(), tr = new WeakMap(), K = new WeakSet(), ls = function() {
  try {
    L(this, De, Oe(() => v(this, St).call(this, v(this, ke))));
  } catch (e) {
    this.error(e);
  }
}, /**
 * @param {unknown} error The deserialized error from the server's hydration comment
 */
os = function(e) {
  const n = v(this, Ie).failed;
  n && L(this, Ce, Oe(() => {
    n(
      v(this, ke),
      () => e,
      () => () => {
      }
    );
  }));
}, cs = function() {
  const e = v(this, Ie).pending;
  e && (this.is_pending = !0, L(this, he, Oe(() => e(v(this, ke)))), pt(() => {
    var n = L(this, lt, document.createDocumentFragment()), r = Qe();
    n.append(r), L(this, De, z(this, K, Fn).call(this, () => Oe(() => v(this, St).call(this, r)))), v(this, yt) === 0 && (v(this, ke).before(n), L(this, lt, null), Pt(
      /** @type {Effect} */
      v(this, he),
      () => {
        L(this, he, null);
      }
    ), z(this, K, Bn).call(
      this,
      /** @type {Batch} */
      R
    ));
  }));
}, wr = function() {
  try {
    if (this.is_pending = this.has_pending_snippet(), L(this, yt, 0), L(this, At, 0), L(this, De, Oe(() => {
      v(this, St).call(this, v(this, ke));
    })), v(this, yt) > 0) {
      var e = L(this, lt, document.createDocumentFragment());
      Zr(v(this, De), e);
      const n = (
        /** @type {(anchor: Node) => void} */
        v(this, Ie).pending
      );
      L(this, he, Oe(() => n(v(this, ke))));
    } else
      z(this, K, Bn).call(
        this,
        /** @type {Batch} */
        R
      );
  } catch (n) {
    this.error(n);
  }
}, /**
 * @param {Batch} batch
 */
Bn = function(e) {
  this.is_pending = !1, e.transfer_effects(v(this, Cn), v(this, Tn));
}, /**
 * @template T
 * @param {() => T} fn
 */
Fn = function(e) {
  var n = B, r = F, i = me;
  Pe(v(this, xe)), we(v(this, xe)), an(v(this, xe).ctx);
  try {
    return kt.ensure(), e();
  } catch (s) {
    return is(s), null;
  } finally {
    Pe(n), we(r), an(i);
  }
}, /**
 * Updates the pending count associated with the currently visible pending snippet,
 * if any, such that we can replace the snippet with content once work is done
 * @param {1 | -1} d
 * @param {Batch} batch
 */
Er = function(e, n) {
  var r;
  if (!this.has_pending_snippet()) {
    this.parent && z(r = this.parent, K, Er).call(r, e, n);
    return;
  }
  L(this, yt, v(this, yt) + e), v(this, yt) === 0 && (z(this, K, Bn).call(this, n), v(this, he) && Pt(v(this, he), () => {
    L(this, he, null);
  }), v(this, lt) && (v(this, ke).before(v(this, lt)), L(this, lt, null)));
}, /**
 * @param {unknown} error
 */
kr = function(e) {
  v(this, De) && (ge(v(this, De)), L(this, De, null)), v(this, he) && (ge(v(this, he)), L(this, he, null)), v(this, Ce) && (ge(v(this, Ce)), L(this, Ce, null)), j && (Se(
    /** @type {TemplateNode} */
    v(this, kn)
  ), Ka(), Se(Kn()));
  var n = v(this, Ie).onerror;
  let r = v(this, Ie).failed;
  var i = !1, s = !1;
  const a = () => {
    if (i) {
      Ga();
      return;
    }
    i = !0, s && Ya(), v(this, Ce) !== null && Pt(v(this, Ce), () => {
      L(this, Ce, null);
    }), z(this, K, Fn).call(this, () => {
      z(this, K, wr).call(this);
    });
  }, l = (o) => {
    try {
      s = !0, n == null || n(o, a), s = !1;
    } catch (c) {
      wt(c, v(this, xe) && v(this, xe).parent);
    }
    r && L(this, Ce, z(this, K, Fn).call(this, () => {
      try {
        return Oe(() => {
          var c = (
            /** @type {Effect} */
            B
          );
          c.b = this, c.f |= br, r(
            v(this, ke),
            () => o,
            () => a
          );
        });
      } catch (c) {
        return wt(
          c,
          /** @type {Effect} */
          v(this, xe).parent
        ), null;
      }
    }));
  };
  pt(() => {
    var o;
    try {
      o = this.transform_error(e);
    } catch (c) {
      wt(c, v(this, xe) && v(this, xe).parent);
      return;
    }
    o !== null && typeof o == "object" && typeof /** @type {any} */
    o.then == "function" ? o.then(
      l,
      /** @param {unknown} e */
      (c) => wt(c, v(this, xe) && v(this, xe).parent)
    ) : l(o);
  });
};
function al(t, e, n, r) {
  const i = wn;
  var s = t.filter((g) => !g.settled), a = e.map(i);
  if (n.length === 0 && s.length === 0) {
    r(a);
    return;
  }
  var l = (
    /** @type {Effect} */
    B
  ), o = ll(), c = s.length === 1 ? s[0].promise : s.length > 1 ? Promise.all(s.map((g) => g.promise)) : null;
  function u(g) {
    if ((l.f & Ne) === 0) {
      o();
      try {
        r([...a, ...g]);
      } catch (x) {
        wt(x, l);
      }
      Jn();
    }
  }
  var d = us();
  if (n.length === 0) {
    c.then(() => u([])).finally(d);
    return;
  }
  function f() {
    Promise.all(n.map((g) => /* @__PURE__ */ ol(g))).then(u).catch((g) => wt(g, l)).finally(d);
  }
  c ? c.then(() => {
    o(), f(), Jn();
  }) : f();
}
function ll() {
  var t = (
    /** @type {Effect} */
    B
  ), e = F, n = me, r = (
    /** @type {Batch} */
    R
  );
  return function(s = !0) {
    Pe(t), we(e), an(n), s && (t.f & Ne) === 0 && (r == null || r.activate(), r == null || r.apply());
  };
}
function Jn(t = !0) {
  Pe(null), we(null), an(null), t && (R == null || R.deactivate());
}
function us() {
  var t = (
    /** @type {Effect} */
    B
  ), e = t.b, n = (
    /** @type {Batch} */
    R
  ), r = !!(e != null && e.is_rendered());
  return e == null || e.update_pending_count(1, n), n.increment(r, t), () => {
    e == null || e.update_pending_count(-1, n), n.decrement(r, t);
  };
}
// @__NO_SIDE_EFFECTS__
function wn(t) {
  var e = ce | se;
  return B !== null && (B.f |= jt), {
    ctx: me,
    deps: null,
    effects: null,
    equals: es,
    f: e,
    fn: t,
    reactions: null,
    rv: 0,
    v: (
      /** @type {V} */
      re
    ),
    wv: 0,
    parent: B,
    ac: null
  };
}
const vn = Symbol("obsolete");
// @__NO_SIDE_EFFECTS__
function ol(t, e, n) {
  let r = (
    /** @type {Effect | null} */
    B
  );
  r === null && Oa();
  var i = (
    /** @type {Promise<V>} */
    /** @type {unknown} */
    void 0
  ), s = Ft(
    /** @type {V} */
    re
  ), a = !F, l = /* @__PURE__ */ new Set();
  return _l(() => {
    var g, x;
    var o = (
      /** @type {Effect} */
      B
    ), c = qi();
    i = c.promise;
    try {
      Promise.resolve(t()).then(c.resolve, (y) => {
        y !== ar && c.reject(y);
      }).finally(Jn);
    } catch (y) {
      c.reject(y), Jn();
    }
    var u = (
      /** @type {Batch} */
      R
    );
    if (a) {
      if ((o.f & zt) !== 0)
        var d = us();
      if (
        // boundary can be null if the async derived is inside an $effect.root not connected to the component render tree
        (g = r.b) != null && g.is_rendered()
      )
        (x = u.async_deriveds.get(o)) == null || x.reject(vn);
      else
        for (const y of l.values())
          y.reject(vn);
      l.add(c), u.async_deriveds.set(o, c);
    }
    const f = (y, h = void 0) => {
      d == null || d(), l.delete(c), h !== vn && (u.activate(), h ? (s.f |= Et, ln(s, h)) : ((s.f & Et) !== 0 && (s.f ^= Et), ln(s, y)), u.deactivate());
    };
    c.promise.then(f, (y) => f(null, y || "unknown"));
  }), qr(() => {
    for (const o of l)
      o.reject(vn);
  }), new Promise((o) => {
    function c(u) {
      function d() {
        u === i ? o(s) : c(i);
      }
      u.then(d, d);
    }
    c(i);
  });
}
// @__NO_SIDE_EFFECTS__
function Tt(t) {
  const e = /* @__PURE__ */ wn(t);
  return Ls(e), e;
}
// @__NO_SIDE_EFFECTS__
function fs(t) {
  const e = /* @__PURE__ */ wn(t);
  return e.equals = ts, e;
}
function cl(t) {
  var e = t.effects;
  if (e !== null) {
    t.effects = null;
    for (var n = 0; n < e.length; n += 1)
      ge(
        /** @type {Effect} */
        e[n]
      );
  }
}
function jr(t) {
  var e, n = B, r = t.parent;
  if (!mt && r !== null && t.v !== re && // if it was never evaluated before, it's guaranteed to fail downstream, so we try to execute instead
  (r.f & (Ne | ve)) !== 0)
    return Xa(), t.v;
  Pe(r);
  try {
    t.f &= ~Bt, cl(t), e = Hs(t);
  } finally {
    Pe(n);
  }
  return e;
}
function ds(t) {
  var e = jr(t);
  if (!t.equals(e) && (t.wv = Rs(), (!(R != null && R.is_fork) || t.deps === null) && (R !== null ? (R.capture(t, e, !0), xn == null || xn.capture(t, e, !0)) : t.v = e, t.deps === null))) {
    Q(t, ie);
    return;
  }
  mt || (oe !== null ? (Xr() || R != null && R.is_fork) && oe.set(t, e) : zr(t));
}
function ul(t) {
  var e, n;
  if (t.effects !== null)
    for (const r of t.effects)
      (r.teardown || r.ac) && ((e = r.teardown) == null || e.call(r), (n = r.ac) == null || n.abort(ar), r.fn !== null && (r.teardown = Aa), r.ac = null, En(r, 0), Jr(r));
}
function hs(t) {
  if (t.effects !== null)
    for (const e of t.effects)
      e.teardown && e.fn !== null && cn(e);
}
let dr = null, Vt = null, R = null, xn = null, oe = null, Cr = null, yn = !1, hr = !1, Xt = null, zn = null;
var hi = 0;
let fl = 1;
var Kt, _t, Mt, Jt, Zt, Qt, ct, en, ye, $n, ut, We, Ke, tn, It, X, Tr, mn, $r, ps, vs, Yt, dl, gn;
const nr = class nr {
  constructor() {
    H(this, X);
    J(this, "id", fl++);
    /** True as soon as `#process` was called */
    H(this, Kt, !1);
    J(this, "linked", !0);
    /** @type {Batch | null} */
    H(this, _t, null);
    /** @type {Batch | null} */
    H(this, Mt, null);
    /** @type {Map<Effect, ReturnType<typeof deferred<any>>>} */
    J(this, "async_deriveds", /* @__PURE__ */ new Map());
    /**
     * The current values of any signals that are updated in this batch.
     * Tuple format: [value, is_derived] (note: is_derived is false for deriveds, too, if they were overridden via assignment)
     * They keys of this map are identical to `this.#previous`
     * @type {Map<Value, [any, boolean]>}
     */
    J(this, "current", /* @__PURE__ */ new Map());
    /**
     * The values of any signals (sources and deriveds) that are updated in this batch _before_ those updates took place.
     * They keys of this map are identical to `this.#current`
     * @type {Map<Value, any>}
     */
    J(this, "previous", /* @__PURE__ */ new Map());
    /**
     * When the batch is committed (and the DOM is updated), we need to remove old branches
     * and append new ones by calling the functions added inside (if/each/key/etc) blocks
     * @type {Set<(batch: Batch) => void>}
     */
    H(this, Jt, /* @__PURE__ */ new Set());
    /**
     * If a fork is discarded, we need to destroy any effects that are no longer needed
     * @type {Set<(batch: Batch) => void>}
     */
    H(this, Zt, /* @__PURE__ */ new Set());
    /**
     * The number of async effects that are currently in flight
     */
    H(this, Qt, 0);
    /**
     * Async effects that are currently in flight, _not_ inside a pending boundary
     * @type {Map<Effect, number>}
     */
    H(this, ct, /* @__PURE__ */ new Map());
    /**
     * A deferred that resolves when the batch is committed, used with `settled()`
     * TODO replace with Promise.withResolvers once supported widely enough
     * @type {{ promise: Promise<void>, resolve: (value?: any) => void, reject: (reason: unknown) => void } | null}
     */
    H(this, en, null);
    /**
     * The root effects that need to be flushed
     * @type {Effect[]}
     */
    H(this, ye, []);
    /**
     * Effects created while this batch was active.
     * @type {Effect[]}
     */
    H(this, $n, []);
    /**
     * Deferred effects (which run after async work has completed) that are DIRTY
     * @type {Set<Effect>}
     */
    H(this, ut, /* @__PURE__ */ new Set());
    /**
     * Deferred effects that are MAYBE_DIRTY
     * @type {Set<Effect>}
     */
    H(this, We, /* @__PURE__ */ new Set());
    /**
     * A map of branches that still exist, but will be destroyed when this batch
     * is committed — we skip over these during `process`.
     * The value contains child effects that were dirty/maybe_dirty before being reset,
     * so they can be rescheduled if the branch survives.
     * @type {Map<Effect, { d: Effect[], m: Effect[] }>}
     */
    H(this, Ke, /* @__PURE__ */ new Map());
    /**
     * Inverse of #skipped_branches which we need to tell prior batches to unskip them when committing
     * @type {Set<Effect>}
     */
    H(this, tn, /* @__PURE__ */ new Set());
    J(this, "is_fork", !1);
    H(this, It, !1);
    Vt === null ? dr = Vt = this : (L(Vt, Mt, this), L(this, _t, Vt)), Vt = this;
  }
  /**
   * Add an effect to the #skipped_branches map and reset its children
   * @param {Effect} effect
   */
  skip_effect(e) {
    v(this, Ke).has(e) || v(this, Ke).set(e, { d: [], m: [] }), v(this, tn).delete(e);
  }
  /**
   * Remove an effect from the #skipped_branches map and reschedule
   * any tracked dirty/maybe_dirty child effects
   * @param {Effect} effect
   * @param {(e: Effect) => void} callback
   */
  unskip_effect(e, n = (r) => this.schedule(r)) {
    var r = v(this, Ke).get(e);
    if (r) {
      v(this, Ke).delete(e);
      for (var i of r.d)
        Q(i, se), n(i);
      for (i of r.m)
        Q(i, tt), n(i);
    }
    v(this, tn).add(e);
  }
  /**
   * Associate a change to a given source with the current
   * batch, noting its previous and current values
   * @param {Value} source
   * @param {any} value
   * @param {boolean} [is_derived]
   */
  capture(e, n, r = !1) {
    e.v !== re && !this.previous.has(e) && this.previous.set(e, e.v), (e.f & Et) === 0 && (this.current.set(e, [n, r]), oe == null || oe.set(e, n)), this.is_fork || (e.v = n);
  }
  activate() {
    R = this;
  }
  deactivate() {
    R = null, oe = null;
  }
  flush() {
    try {
      hr = !0, R = this, z(this, X, mn).call(this);
    } finally {
      hi = 0, Cr = null, Xt = null, zn = null, hr = !1, R = null, oe = null, Rt.clear();
    }
  }
  discard() {
    var e;
    for (const n of v(this, Zt)) n(this);
    v(this, Zt).clear();
    for (const n of this.async_deriveds.values())
      n.reject(vn);
    z(this, X, gn).call(this), (e = v(this, en)) == null || e.resolve();
  }
  /**
   * @param {Effect} effect
   */
  register_created_effect(e) {
    v(this, $n).push(e);
  }
  /**
   * @param {boolean} blocking
   * @param {Effect} effect
   */
  increment(e, n) {
    if (L(this, Qt, v(this, Qt) + 1), e) {
      let r = v(this, ct).get(n) ?? 0;
      v(this, ct).set(n, r + 1);
    }
  }
  /**
   * @param {boolean} blocking
   * @param {Effect} effect
   */
  decrement(e, n) {
    if (L(this, Qt, v(this, Qt) - 1), e) {
      let r = v(this, ct).get(n) ?? 0;
      r === 1 ? v(this, ct).delete(n) : v(this, ct).set(n, r - 1);
    }
    v(this, It) || (L(this, It, !0), pt(() => {
      L(this, It, !1), this.linked && this.flush();
    }));
  }
  /**
   * @param {Set<Effect>} dirty_effects
   * @param {Set<Effect>} maybe_dirty_effects
   */
  transfer_effects(e, n) {
    for (const r of e)
      v(this, ut).add(r);
    for (const r of n)
      v(this, We).add(r);
    e.clear(), n.clear();
  }
  /** @param {(batch: Batch) => void} fn */
  oncommit(e) {
    v(this, Jt).add(e);
  }
  /** @param {(batch: Batch) => void} fn */
  ondiscard(e) {
    v(this, Zt).add(e);
  }
  settled() {
    return (v(this, en) ?? L(this, en, qi())).promise;
  }
  static ensure() {
    if (R === null) {
      const e = R = new nr();
      !hr && !yn && pt(() => {
        v(e, Kt) || e.flush();
      });
    }
    return R;
  }
  apply() {
    {
      oe = null;
      return;
    }
  }
  /**
   *
   * @param {Effect} effect
   */
  schedule(e) {
    var i;
    if (Cr = e, (i = e.b) != null && i.is_pending && (e.f & (rn | sr | Gi)) !== 0 && (e.f & zt) === 0) {
      e.b.defer_effect(e);
      return;
    }
    for (var n = e; n.parent !== null; ) {
      n = n.parent;
      var r = n.f;
      if (Xt !== null && n === B && (F === null || (F.f & ce) === 0))
        return;
      if ((r & (vt | qe)) !== 0) {
        if ((r & ie) === 0)
          return;
        n.f ^= ie;
      }
    }
    v(this, ye).push(n);
  }
};
Kt = new WeakMap(), _t = new WeakMap(), Mt = new WeakMap(), Jt = new WeakMap(), Zt = new WeakMap(), Qt = new WeakMap(), ct = new WeakMap(), en = new WeakMap(), ye = new WeakMap(), $n = new WeakMap(), ut = new WeakMap(), We = new WeakMap(), Ke = new WeakMap(), tn = new WeakMap(), It = new WeakMap(), X = new WeakSet(), Tr = function() {
  if (this.is_fork) return !0;
  for (const r of v(this, ct).keys()) {
    for (var e = r, n = !1; e.parent !== null; ) {
      if (v(this, Ke).has(e)) {
        n = !0;
        break;
      }
      e = e.parent;
    }
    if (!n)
      return !0;
  }
  return !1;
}, mn = function() {
  var o, c, u, d;
  L(this, Kt, !0), hi++ > 1e3 && (z(this, X, gn).call(this), hl());
  for (const f of v(this, ut))
    v(this, We).delete(f), Q(f, se), this.schedule(f);
  for (const f of v(this, We))
    Q(f, tt), this.schedule(f);
  const e = v(this, ye);
  L(this, ye, []), this.apply();
  var n = Xt = [], r = [], i = zn = [];
  for (const f of e)
    try {
      z(this, X, $r).call(this, f, n, r);
    } catch (g) {
      throw bs(f), z(this, X, Tr).call(this) || this.discard(), g;
    }
  if (R = null, i.length > 0) {
    var s = nr.ensure();
    for (const f of i)
      s.schedule(f);
  }
  if (Xt = null, zn = null, z(this, X, Tr).call(this)) {
    z(this, X, Yt).call(this, r), z(this, X, Yt).call(this, n);
    for (const [f, g] of v(this, Ke))
      gs(f, g);
    i.length > 0 && /** @type {unknown} */
    z(o = R, X, mn).call(o);
    return;
  }
  const a = z(this, X, ps).call(this);
  if (a) {
    z(this, X, Yt).call(this, r), z(this, X, Yt).call(this, n), z(c = a, X, vs).call(c, this);
    return;
  }
  v(this, ut).clear(), v(this, We).clear();
  for (const f of v(this, Jt)) f(this);
  v(this, Jt).clear(), xn = this, pi(r), pi(n), xn = null, (u = v(this, en)) == null || u.resolve();
  var l = (
    /** @type {Batch | null} */
    /** @type {unknown} */
    R
  );
  if (v(this, Qt) === 0 && (v(this, ye).length === 0 || l !== null) && z(this, X, gn).call(this), v(this, ye).length > 0)
    if (l !== null) {
      const f = l;
      v(f, ye).push(...v(this, ye).filter((g) => !v(f, ye).includes(g)));
    } else
      l = this;
  l !== null && z(d = l, X, mn).call(d);
}, /**
 * Traverse the effect tree, executing effects or stashing
 * them for later execution as appropriate
 * @param {Effect} root
 * @param {Effect[]} effects
 * @param {Effect[]} render_effects
 */
$r = function(e, n, r) {
  e.f ^= ie;
  for (var i = e.first; i !== null; ) {
    var s = i.f, a = (s & (qe | vt)) !== 0, l = a && (s & ie) !== 0, o = l || (s & ve) !== 0 || v(this, Ke).has(i);
    if (!o && i.fn !== null) {
      a ? i.f ^= ie : (s & rn) !== 0 ? n.push(i) : In(i) && ((s & Ue) !== 0 && v(this, We).add(i), cn(i));
      var c = i.first;
      if (c !== null) {
        i = c;
        continue;
      }
    }
    for (; i !== null; ) {
      var u = i.next;
      if (u !== null) {
        i = u;
        break;
      }
      i = i.parent;
    }
  }
}, ps = function() {
  for (var e = v(this, _t); e !== null; ) {
    if (!e.is_fork) {
      for (const [n, [, r]] of this.current)
        if (e.current.has(n) && !r)
          return e;
    }
    e = v(e, _t);
  }
  return null;
}, /**
 * @param {Batch} batch
 */
vs = function(e) {
  var r;
  for (const [i, s] of e.current)
    !this.previous.has(i) && e.previous.has(i) && this.previous.set(i, e.previous.get(i)), this.current.set(i, s);
  for (const [i, s] of e.async_deriveds) {
    const a = this.async_deriveds.get(i);
    a && s.promise.then(a.resolve).catch(a.reject);
  }
  e.async_deriveds.clear(), this.transfer_effects(v(e, ut), v(e, We));
  const n = (i) => {
    var s = i.reactions;
    if (s !== null)
      for (const o of s) {
        var a = o.f;
        if ((a & ce) !== 0)
          n(
            /** @type {Derived} */
            o
          );
        else {
          var l = (
            /** @type {Effect} */
            o
          );
          a & (qt | Ue) && !this.async_deriveds.has(l) && (v(this, We).delete(l), Q(l, se), this.schedule(l));
        }
      }
  };
  for (const i of this.current.keys())
    n(i);
  this.oncommit(() => e.discard()), z(r = e, X, gn).call(r), R = this, z(this, X, mn).call(this);
}, /**
 * @param {Effect[]} effects
 */
Yt = function(e) {
  for (var n = 0; n < e.length; n += 1)
    as(e[n], v(this, ut), v(this, We));
}, dl = function() {
  var d;
  for (let f = dr; f !== null; f = v(f, Mt)) {
    var e = f.id < this.id, n = [];
    for (const [g, [x, y]] of this.current) {
      if (f.current.has(g)) {
        var r = (
          /** @type {[any, boolean]} */
          f.current.get(g)[0]
        );
        if (e && x !== r)
          f.current.set(g, [x, y]);
        else
          continue;
      }
      n.push(g);
    }
    if (e)
      for (const [g, x] of this.async_deriveds) {
        const y = f.async_deriveds.get(g);
        y && x.promise.then(y.resolve).catch(y.reject);
      }
    var i = [...f.current.keys()].filter(
      (g) => !/** @type {[any, boolean]} */
      f.current.get(g)[1]
    );
    if (!(!v(f, Kt) || i.length === 0)) {
      var s = i.filter((g) => !this.current.has(g));
      if (s.length === 0)
        e && f.discard();
      else if (n.length > 0) {
        if (e)
          for (const g of v(this, tn))
            f.unskip_effect(g, (x) => {
              var y;
              (x.f & (Ue | qt)) !== 0 ? f.schedule(x) : z(y = f, X, Yt).call(y, [x]);
            });
        f.activate();
        var a = /* @__PURE__ */ new Set(), l = /* @__PURE__ */ new Map();
        for (var o of n)
          ms(o, s, a, l);
        l = /* @__PURE__ */ new Map();
        var c = [...f.current].filter(([g, x]) => {
          const y = this.current.get(g);
          return y ? y[0] !== x[0] || y[1] !== x[1] : !0;
        }).map(([g]) => g);
        if (c.length > 0)
          for (const g of v(this, $n))
            (g.f & (Ne | ve | qn)) === 0 && Wr(g, c, l) && ((g.f & (qt | Ue)) !== 0 ? (Q(g, se), f.schedule(g)) : v(f, ut).add(g));
        if (v(f, ye).length > 0 && !v(f, It)) {
          f.apply();
          for (var u of v(f, ye))
            z(d = f, X, $r).call(d, u, [], []);
          L(f, ye, []);
        }
        f.deactivate();
      }
    }
  }
}, gn = function() {
  if (this.linked) {
    var e = v(this, _t), n = v(this, Mt);
    e === null ? dr = n : L(e, Mt, n), n === null ? Vt = e : L(n, _t, e), this.linked = !1;
  }
};
let kt = nr;
function S(t) {
  var e = yn;
  yn = !0;
  try {
    for (var n; ; ) {
      if (Qa(), R === null)
        return (
          /** @type {T} */
          n
        );
      R.flush();
    }
  } finally {
    yn = e;
  }
}
function hl() {
  try {
    Fa();
  } catch (t) {
    wt(t, Cr);
  }
}
let ze = null;
function pi(t) {
  var e = t.length;
  if (e !== 0) {
    for (var n = 0; n < e; ) {
      var r = t[n++];
      if ((r.f & (Ne | ve)) === 0 && In(r) && (ze = /* @__PURE__ */ new Set(), cn(r), r.deps === null && r.first === null && r.nodes === null && r.teardown === null && r.ac === null && Ms(r), (ze == null ? void 0 : ze.size) > 0)) {
        Rt.clear();
        for (const i of ze) {
          if ((i.f & (Ne | ve)) !== 0) continue;
          const s = [i];
          let a = i.parent;
          for (; a !== null; )
            ze.has(a) && (ze.delete(a), s.push(a)), a = a.parent;
          for (let l = s.length - 1; l >= 0; l--) {
            const o = s[l];
            (o.f & (Ne | ve)) === 0 && cn(o);
          }
        }
        ze.clear();
      }
    }
    ze = null;
  }
}
function ms(t, e, n, r) {
  if (!n.has(t) && (n.add(t), t.reactions !== null))
    for (const i of t.reactions) {
      const s = i.f;
      (s & ce) !== 0 ? ms(
        /** @type {Derived} */
        i,
        e,
        n,
        r
      ) : (s & (qt | Ue)) !== 0 && (s & se) === 0 && Wr(i, e, r) && (Q(i, se), Vr(
        /** @type {Effect} */
        i
      ));
    }
}
function Wr(t, e, n) {
  const r = n.get(t);
  if (r !== void 0) return r;
  if (t.deps !== null)
    for (const i of t.deps) {
      if (Un.call(e, i))
        return !0;
      if ((i.f & ce) !== 0 && Wr(
        /** @type {Derived} */
        i,
        e,
        n
      ))
        return n.set(
          /** @type {Derived} */
          i,
          !0
        ), !0;
    }
  return n.set(t, !1), !1;
}
function Vr(t) {
  R.schedule(t);
}
function gs(t, e) {
  if (!((t.f & qe) !== 0 && (t.f & ie) !== 0)) {
    (t.f & se) !== 0 ? e.d.push(t) : (t.f & tt) !== 0 && e.m.push(t), Q(t, ie);
    for (var n = t.first; n !== null; )
      gs(n, e), n = n.next;
  }
}
function bs(t) {
  Q(t, ie);
  for (var e = t.first; e !== null; )
    bs(e), e = e.next;
}
let Zn = /* @__PURE__ */ new Set();
const Rt = /* @__PURE__ */ new Map();
let xs = !1;
function Ft(t, e) {
  var n = {
    f: 0,
    // TODO ideally we could skip this altogether, but it causes type errors
    v: t,
    reactions: null,
    equals: es,
    rv: 0,
    wv: 0
  };
  return n;
}
// @__NO_SIDE_EFFECTS__
function je(t, e) {
  const n = Ft(t);
  return Ls(n), n;
}
// @__NO_SIDE_EFFECTS__
function ys(t, e = !1, n = !0) {
  const r = Ft(t);
  return e || (r.equals = ts), r;
}
function $e(t, e, n = !1) {
  F !== null && // since we are untracking the function inside `$inspect.with` we need to add this check
  // to ensure we error if state is set inside an inspect effect
  (!Xe || (F.f & qn) !== 0) && ns() && (F.f & (ce | Ue | qt | qn)) !== 0 && (et === null || !et.has(t)) && Ua();
  let r = n ? dt(e) : e;
  return ln(t, r, zn);
}
function ln(t, e, n = null) {
  if (!t.equals(e)) {
    Rt.set(t, mt ? e : t.v);
    var r = kt.ensure();
    if (r.capture(t, e), (t.f & ce) !== 0) {
      const i = (
        /** @type {Derived} */
        t
      );
      (t.f & se) !== 0 && jr(i), oe === null && zr(i);
    }
    t.wv = Rs(), _s(t, se, n), B !== null && (B.f & ie) !== 0 && (B.f & (qe | vt)) === 0 && (Me === null ? kl([t]) : Me.push(t)), !r.is_fork && Zn.size > 0 && !xs && pl();
  }
  return e;
}
function pl() {
  xs = !1;
  for (const t of Zn) {
    (t.f & ie) !== 0 && Q(t, tt);
    let e;
    try {
      e = In(t);
    } catch {
      e = !0;
    }
    e && cn(t);
  }
  Zn.clear();
}
function _n(t) {
  $e(t, t.v + 1);
}
function _s(t, e, n) {
  var r = t.reactions;
  if (r !== null)
    for (var i = r.length, s = 0; s < i; s++) {
      var a = r[s], l = a.f, o = (l & se) === 0;
      if (o && Q(a, e), (l & qn) !== 0)
        Zn.add(
          /** @type {Effect} */
          a
        );
      else if ((l & ce) !== 0) {
        var c = (
          /** @type {Derived} */
          a
        );
        oe == null || oe.delete(c), (l & Bt) === 0 && (l & Re && (B === null || (B.f & Gn) === 0) && (a.f |= Bt), _s(c, tt, n));
      } else if (o) {
        var u = (
          /** @type {Effect} */
          a
        );
        (l & Ue) !== 0 && ze !== null && ze.add(u), n !== null ? n.push(u) : Vr(u);
      }
    }
}
function dt(t) {
  if (typeof t != "object" || t === null || Ot in t)
    return t;
  const e = Xi(t);
  if (e !== Na && e !== Sa)
    return t;
  var n = /* @__PURE__ */ new Map(), r = Br(t), i = /* @__PURE__ */ je(0), s = Ht, a = (l) => {
    if (Ht === s)
      return l();
    var o = F, c = Ht;
    we(null), xi(s);
    var u = l();
    return we(o), xi(c), u;
  };
  return r && n.set("length", /* @__PURE__ */ je(
    /** @type {any[]} */
    t.length
  )), new Proxy(
    /** @type {any} */
    t,
    {
      defineProperty(l, o, c) {
        (!("value" in c) || c.configurable === !1 || c.enumerable === !1 || c.writable === !1) && Wa();
        var u = n.get(o);
        return u === void 0 ? a(() => {
          var d = /* @__PURE__ */ je(c.value);
          return n.set(o, d), d;
        }) : $e(u, c.value, !0), !0;
      },
      deleteProperty(l, o) {
        var c = n.get(o);
        if (c === void 0) {
          if (o in l) {
            const u = a(() => /* @__PURE__ */ je(re));
            n.set(o, u), _n(i);
          }
        } else
          $e(c, re), _n(i);
        return !0;
      },
      get(l, o, c) {
        var g;
        if (o === Ot)
          return t;
        var u = n.get(o), d = o in l;
        if (u === void 0 && (!d || (g = Lt(l, o)) != null && g.writable) && (u = a(() => {
          var x = dt(d ? l[o] : re), y = /* @__PURE__ */ je(x);
          return y;
        }), n.set(o, u)), u !== void 0) {
          var f = I(u);
          return f === re ? void 0 : f;
        }
        return Reflect.get(l, o, c);
      },
      getOwnPropertyDescriptor(l, o) {
        var c = Reflect.getOwnPropertyDescriptor(l, o);
        if (c && "value" in c) {
          var u = n.get(o);
          u && (c.value = I(u));
        } else if (c === void 0) {
          var d = n.get(o), f = d == null ? void 0 : d.v;
          if (d !== void 0 && f !== re)
            return {
              enumerable: !0,
              configurable: !0,
              value: f,
              writable: !0
            };
        }
        return c;
      },
      has(l, o) {
        var f;
        if (o === Ot)
          return !0;
        var c = n.get(o), u = c !== void 0 && c.v !== re || Reflect.has(l, o);
        if (c !== void 0 || B !== null && (!u || (f = Lt(l, o)) != null && f.writable)) {
          c === void 0 && (c = a(() => {
            var g = u ? dt(l[o]) : re, x = /* @__PURE__ */ je(g);
            return x;
          }), n.set(o, c));
          var d = I(c);
          if (d === re)
            return !1;
        }
        return u;
      },
      set(l, o, c, u) {
        var p;
        var d = n.get(o), f = o in l;
        if (r && o === "length")
          for (var g = c; g < /** @type {Source<number>} */
          d.v; g += 1) {
            var x = n.get(g + "");
            x !== void 0 ? $e(x, re) : g in l && (x = a(() => /* @__PURE__ */ je(re)), n.set(g + "", x));
          }
        if (d === void 0)
          (!f || (p = Lt(l, o)) != null && p.writable) && (d = a(() => /* @__PURE__ */ je(void 0)), $e(d, dt(c)), n.set(o, d));
        else {
          f = d.v !== re;
          var y = a(() => dt(c));
          $e(d, y);
        }
        var h = Reflect.getOwnPropertyDescriptor(l, o);
        if (h != null && h.set && h.set.call(u, c), !f) {
          if (r && typeof o == "string") {
            var b = (
              /** @type {Source<number>} */
              n.get("length")
            ), m = Number(o);
            Number.isInteger(m) && m >= b.v && $e(b, m + 1);
          }
          _n(i);
        }
        return !0;
      },
      ownKeys(l) {
        I(i);
        var o = Reflect.ownKeys(l).filter((d) => {
          var f = n.get(d);
          return f === void 0 || f.v !== re;
        });
        for (var [c, u] of n)
          u.v !== re && !(c in l) && o.push(c);
        return o;
      },
      setPrototypeOf() {
        Va();
      }
    }
  );
}
function vi(t) {
  try {
    if (t !== null && typeof t == "object" && Ot in t)
      return t[Ot];
  } catch {
  }
  return t;
}
function vl(t, e) {
  return Object.is(vi(t), vi(e));
}
var mi, ws, Es, ks;
function Nr() {
  if (mi === void 0) {
    mi = window, ws = /Firefox/.test(navigator.userAgent);
    var t = Element.prototype, e = Node.prototype, n = Text.prototype;
    Es = Lt(e, "firstChild").get, ks = Lt(e, "nextSibling").get, di(t) && (t[yr] = void 0, t[Hn] = null, t[_r] = void 0, t.__e = void 0), di(n) && (n[pn] = void 0);
  }
}
function Qe(t = "") {
  return document.createTextNode(t);
}
// @__NO_SIDE_EFFECTS__
function on(t) {
  return (
    /** @type {TemplateNode | null} */
    Es.call(t)
  );
}
// @__NO_SIDE_EFFECTS__
function bt(t) {
  return (
    /** @type {TemplateNode | null} */
    ks.call(t)
  );
}
function P(t, e) {
  if (!j)
    return /* @__PURE__ */ on(t);
  var n = /* @__PURE__ */ on(G);
  if (n === null)
    n = G.appendChild(Qe());
  else if (e && n.nodeType !== Fr) {
    var r = Qe();
    return n == null || n.before(r), Se(r), r;
  }
  return e && $s(
    /** @type {Text} */
    n
  ), Se(n), n;
}
function V(t, e = 1, n = !1) {
  let r = j ? G : t;
  for (var i; e--; )
    i = r, r = /** @type {TemplateNode} */
    /* @__PURE__ */ bt(r);
  if (!j)
    return r;
  if (n) {
    if ((r == null ? void 0 : r.nodeType) !== Fr) {
      var s = Qe();
      return r === null ? i == null || i.after(s) : r.before(s), Se(s), s;
    }
    $s(
      /** @type {Text} */
      r
    );
  }
  return Se(r), r;
}
function Cs(t) {
  t.textContent = "";
}
function Ts() {
  return !1;
}
function Ur(t, e, n) {
  return (
    /** @type {T extends keyof HTMLElementTagNameMap ? HTMLElementTagNameMap[T] : Element} */
    n ? document.createElement(t, { is: n }) : document.createElement(t)
  );
}
function $s(t) {
  if (
    /** @type {string} */
    t.nodeValue.length < 65536
  )
    return;
  let e = t.nextSibling;
  for (; e !== null && e.nodeType === Fr; )
    e.remove(), t.nodeValue += /** @type {string} */
    e.nodeValue, e = t.nextSibling;
}
let gi = !1;
function ml() {
  gi || (gi = !0, document.addEventListener(
    "reset",
    (t) => {
      Promise.resolve().then(() => {
        var e;
        if (!t.defaultPrevented)
          for (
            const n of
            /**@type {HTMLFormElement} */
            t.target.elements
          )
            (e = n[Ji]) == null || e.call(n);
      });
    },
    // In the capture phase to guarantee we get noticed of it (no possibility of stopPropagation)
    { capture: !0 }
  ));
}
function Yr(t) {
  var e = F, n = B;
  we(null), Pe(null);
  try {
    return t();
  } finally {
    we(e), Pe(n);
  }
}
function gl(t) {
  B === null && (F === null && Ba(), Ha()), mt && Pa();
}
function bl(t, e) {
  var n = e.last;
  n === null ? e.last = e.first = t : (n.next = t, t.prev = n, e.last = t);
}
function rt(t, e) {
  var n = B;
  n !== null && (n.f & ve) !== 0 && (t |= ve);
  var r = {
    ctx: me,
    deps: null,
    nodes: null,
    f: t | se | Re,
    first: null,
    fn: e,
    last: null,
    next: null,
    parent: n,
    b: n && n.b,
    prev: null,
    teardown: null,
    wv: 0,
    ac: null
  };
  R == null || R.register_created_effect(r);
  var i = r;
  if ((t & rn) !== 0)
    Xt !== null ? Xt.push(r) : kt.ensure().schedule(r);
  else if (e !== null) {
    try {
      cn(r);
    } catch (a) {
      throw ge(r), a;
    }
    i.deps === null && i.teardown === null && i.nodes === null && i.first === i.last && // either `null`, or a singular child
    (i.f & jt) === 0 && (i = i.first, (t & Ue) !== 0 && (t & sn) !== 0 && i !== null && (i.f |= sn));
  }
  if (i !== null && (i.parent = n, n !== null && bl(i, n), F !== null && (F.f & ce) !== 0 && (t & vt) === 0)) {
    var s = (
      /** @type {Derived} */
      F
    );
    (s.effects ?? (s.effects = [])).push(i);
  }
  return r;
}
function Xr() {
  return F !== null && !Xe;
}
function qr(t) {
  const e = rt(sr, null);
  return Q(e, ie), e.teardown = t, e;
}
function or(t) {
  gl();
  var e = (
    /** @type {Effect} */
    B.f
  ), n = !F && (e & qe) !== 0 && me !== null && !me.i;
  if (n) {
    var r = (
      /** @type {ComponentContext} */
      me
    );
    (r.e ?? (r.e = [])).push(t);
  } else
    return Ns(t);
}
function Ns(t) {
  return rt(rn | Da, t);
}
function xl(t) {
  kt.ensure();
  const e = rt(vt | jt, t);
  return () => {
    ge(e);
  };
}
function yl(t) {
  kt.ensure();
  const e = rt(vt | jt, t);
  return (n = {}) => new Promise((r) => {
    n.outro ? Pt(e, () => {
      ge(e), r(void 0);
    }) : (ge(e), r(void 0));
  });
}
function Ss(t) {
  return rt(rn, t);
}
function _l(t) {
  return rt(qt | jt, t);
}
function Gr(t, e = 0) {
  return rt(sr | e, t);
}
function U(t, e = [], n = [], r = []) {
  al(r, e, n, (i) => {
    rt(sr, () => {
      t(...i.map(I));
    });
  });
}
function Kr(t, e = 0) {
  var n = rt(Ue | e, t);
  return n;
}
function Oe(t) {
  return rt(qe | jt, t);
}
function As(t) {
  var e = t.teardown;
  if (e !== null) {
    const n = mt, r = F;
    bi(!0), we(null);
    try {
      e.call(null);
    } finally {
      bi(n), we(r);
    }
  }
}
function Jr(t, e = !1) {
  var n = t.first;
  for (t.first = t.last = null; n !== null; ) {
    const i = n.ac;
    i !== null && Yr(() => {
      i.abort(ar);
    });
    var r = n.next;
    (n.f & vt) !== 0 ? n.parent = null : ge(n, e), n = r;
  }
}
function wl(t) {
  for (var e = t.first; e !== null; ) {
    var n = e.next;
    (e.f & qe) === 0 && ge(e), e = n;
  }
}
function ge(t, e = !0) {
  var n = !1;
  (e || (t.f & Ia) !== 0) && t.nodes !== null && t.nodes.end !== null && (El(
    t.nodes.start,
    /** @type {TemplateNode} */
    t.nodes.end
  ), n = !0), t.f |= xr, Jr(t, e && !n), En(t, 0);
  var r = t.nodes && t.nodes.t;
  if (r !== null)
    for (const s of r)
      s.stop();
  As(t), t.f ^= xr, t.f |= Ne;
  var i = t.parent;
  i !== null && i.first !== null && Ms(t), t.next = t.prev = t.teardown = t.ctx = t.deps = t.fn = t.nodes = t.ac = t.b = null;
}
function El(t, e) {
  for (; t !== null; ) {
    var n = t === e ? null : /* @__PURE__ */ bt(t);
    t.remove(), t = n;
  }
}
function Ms(t) {
  var e = t.parent, n = t.prev, r = t.next;
  n !== null && (n.next = r), r !== null && (r.prev = n), e !== null && (e.first === t && (e.first = r), e.last === t && (e.last = n));
}
function Pt(t, e, n = !0) {
  var r = [];
  Is(t, r, !0);
  var i = () => {
    n && ge(t), e && e();
  }, s = r.length;
  if (s > 0) {
    var a = () => --s || i();
    for (var l of r)
      l.out(a);
  } else
    i();
}
function Is(t, e, n) {
  if ((t.f & ve) === 0) {
    t.f ^= ve;
    var r = t.nodes && t.nodes.t;
    if (r !== null)
      for (const l of r)
        (l.is_global || n) && e.push(l);
    for (var i = t.first; i !== null; ) {
      var s = i.next;
      if ((i.f & vt) === 0) {
        var a = (i.f & sn) !== 0 || // If this is a branch effect without a block effect parent,
        // it means the parent block effect was pruned. In that case,
        // transparency information was transferred to the branch effect.
        (i.f & qe) !== 0 && (t.f & Ue) !== 0;
        Is(i, e, a ? n : !1);
      }
      i = s;
    }
  }
}
function Qn(t) {
  Ds(t, !0);
}
function Ds(t, e) {
  if ((t.f & ve) !== 0) {
    t.f ^= ve, (t.f & ie) === 0 && (Q(t, se), kt.ensure().schedule(t));
    for (var n = t.first; n !== null; ) {
      var r = n.next, i = (n.f & sn) !== 0 || (n.f & qe) !== 0;
      Ds(n, i ? e : !1), n = r;
    }
    var s = t.nodes && t.nodes.t;
    if (s !== null)
      for (const a of s)
        (a.is_global || e) && a.in();
  }
}
function Zr(t, e) {
  if (t.nodes)
    for (var n = t.nodes.start, r = t.nodes.end; n !== null; ) {
      var i = n === r ? null : /* @__PURE__ */ bt(n);
      e.append(n), n = i;
    }
}
let jn = !1, mt = !1;
function bi(t) {
  mt = t;
}
let F = null, Xe = !1;
function we(t) {
  F = t;
}
let B = null;
function Pe(t) {
  B = t;
}
let et = null;
function Ls(t) {
  F !== null && (et ?? (et = /* @__PURE__ */ new Set())).add(t);
}
let _e = null, Ee = 0, Me = null;
function kl(t) {
  Me = t;
}
let Os = 1, $t = 0, Ht = $t;
function xi(t) {
  Ht = t;
}
function Rs() {
  return ++Os;
}
function In(t) {
  var e = t.f;
  if ((e & se) !== 0)
    return !0;
  if (e & ce && (t.f &= ~Bt), (e & tt) !== 0) {
    for (var n = (
      /** @type {Value[]} */
      t.deps
    ), r = n.length, i = 0; i < r; i++) {
      var s = n[i];
      if (In(
        /** @type {Derived} */
        s
      ) && ds(
        /** @type {Derived} */
        s
      ), s.wv > t.wv)
        return !0;
    }
    (e & Re) !== 0 && // During time traveling we don't want to reset the status so that
    // traversal of the graph in the other batches still happens
    oe === null && Q(t, ie);
  }
  return !1;
}
function Ps(t, e, n = !0) {
  var r = t.reactions;
  if (r !== null && !(et !== null && et.has(t)))
    for (var i = 0; i < r.length; i++) {
      var s = r[i];
      (s.f & ce) !== 0 ? Ps(
        /** @type {Derived} */
        s,
        e,
        !1
      ) : e === s && (n ? Q(s, se) : (s.f & ie) !== 0 && Q(s, tt), Vr(
        /** @type {Effect} */
        s
      ));
    }
}
function Hs(t) {
  var y;
  var e = _e, n = Ee, r = Me, i = F, s = et, a = me, l = Xe, o = Ht, c = t.f;
  _e = /** @type {null | Value[]} */
  null, Ee = 0, Me = null, F = (c & (qe | vt)) === 0 ? t : null, et = null, an(t.ctx), Xe = !1, Ht = ++$t, t.ac !== null && (Yr(() => {
    t.ac.abort(ar);
  }), t.ac = null);
  try {
    t.f |= Gn;
    var u = (
      /** @type {Function} */
      t.fn
    ), d = u();
    t.f |= zt;
    var f = t.deps, g = R == null ? void 0 : R.is_fork;
    if (_e !== null) {
      var x;
      if (g || En(t, Ee), f !== null && Ee > 0)
        for (f.length = Ee + _e.length, x = 0; x < _e.length; x++)
          f[Ee + x] = _e[x];
      else
        t.deps = f = _e;
      if (Xr() && (t.f & Re) !== 0)
        for (x = Ee; x < f.length; x++)
          ((y = f[x]).reactions ?? (y.reactions = [])).push(t);
    } else !g && f !== null && Ee < f.length && (En(t, Ee), f.length = Ee);
    if (ns() && Me !== null && !Xe && f !== null && (t.f & (ce | tt | se)) === 0)
      for (x = 0; x < /** @type {Source[]} */
      Me.length; x++)
        Ps(
          Me[x],
          /** @type {Effect} */
          t
        );
    if (i !== null && i !== t) {
      if ($t++, i.deps !== null)
        for (let h = 0; h < n; h += 1)
          i.deps[h].rv = $t;
      if (e !== null)
        for (const h of e)
          h.rv = $t;
      Me !== null && (r === null ? r = Me : r.push(.../** @type {Source[]} */
      Me));
    }
    return (t.f & Et) !== 0 && (t.f ^= Et), d;
  } catch (h) {
    return is(h);
  } finally {
    t.f ^= Gn, _e = e, Ee = n, Me = r, F = i, et = s, an(a), Xe = l, Ht = o;
  }
}
function Cl(t, e) {
  let n = e.reactions;
  if (n !== null) {
    var r = Ta.call(n, t);
    if (r !== -1) {
      var i = n.length - 1;
      i === 0 ? n = e.reactions = null : (n[r] = n[i], n.pop());
    }
  }
  if (n === null && (e.f & ce) !== 0 && // Destroying a child effect while updating a parent effect can cause a dependency to appear
  // to be unused, when in fact it is used by the currently-updating parent. Checking `new_deps`
  // allows us to skip the expensive work of disconnecting and immediately reconnecting it
  (_e === null || !Un.call(_e, e))) {
    var s = (
      /** @type {Derived} */
      e
    );
    (s.f & Re) !== 0 && (s.f ^= Re, s.f &= ~Bt), s.v !== re && zr(s), ul(s), En(s, 0);
  }
}
function En(t, e) {
  var n = t.deps;
  if (n !== null)
    for (var r = e; r < n.length; r++)
      Cl(t, n[r]);
}
function cn(t) {
  var e = t.f;
  if ((e & Ne) === 0) {
    Q(t, ie);
    var n = B, r = jn;
    B = t, jn = !0;
    try {
      (e & (Ue | Gi)) !== 0 ? wl(t) : Jr(t), As(t);
      var i = Hs(t);
      t.teardown = typeof i == "function" ? i : null, t.wv = Os;
      var s;
      Yi && Za && (t.f & se) !== 0 && t.deps;
    } finally {
      jn = r, B = n;
    }
  }
}
function I(t) {
  var e = t.f, n = (e & ce) !== 0;
  if (F !== null && !Xe) {
    var r = B !== null && (B.f & Ne) !== 0;
    if (!r && (et === null || !et.has(t))) {
      var i = F.deps;
      if ((F.f & Gn) !== 0)
        t.rv < $t && (t.rv = $t, _e === null && i !== null && i[Ee] === t ? Ee++ : _e === null ? _e = [t] : _e.push(t));
      else {
        F.deps ?? (F.deps = []), Un.call(F.deps, t) || F.deps.push(t);
        var s = t.reactions;
        s === null ? t.reactions = [F] : Un.call(s, F) || s.push(F);
      }
    }
  }
  if (mt && Rt.has(t))
    return Rt.get(t);
  if (n) {
    var a = (
      /** @type {Derived} */
      t
    );
    if (mt) {
      var l = a.v;
      return ((a.f & ie) === 0 && a.reactions !== null || Fs(a)) && (l = jr(a)), Rt.set(a, l), l;
    }
    var o = (a.f & Re) === 0 && !Xe && F !== null && (jn || (F.f & Re) !== 0), c = (a.f & zt) === 0;
    In(a) && (o && (a.f |= Re), ds(a)), o && !c && (hs(a), Bs(a));
  }
  if (oe != null && oe.has(t))
    return oe.get(t);
  if ((t.f & Et) !== 0)
    throw t.v;
  return t.v;
}
function Bs(t) {
  if (t.f |= Re, t.deps !== null)
    for (const e of t.deps)
      (e.reactions ?? (e.reactions = [])).push(t), (e.f & ce) !== 0 && (e.f & Re) === 0 && (hs(
        /** @type {Derived} */
        e
      ), Bs(
        /** @type {Derived} */
        e
      ));
}
function Fs(t) {
  if (t.v === re) return !0;
  if (t.deps === null) return !1;
  for (const e of t.deps)
    if (Rt.has(e) || (e.f & ce) !== 0 && Fs(
      /** @type {Derived} */
      e
    ))
      return !0;
  return !1;
}
function Qr(t) {
  var e = Xe;
  try {
    return Xe = !0, t();
  } finally {
    Xe = e;
  }
}
const Nt = Symbol("events"), zs = /* @__PURE__ */ new Set(), Sr = /* @__PURE__ */ new Set();
function Tl(t, e, n, r = {}) {
  function i(s) {
    if (r.capture || Ar.call(e, s), !s.cancelBubble)
      return Yr(() => n == null ? void 0 : n.call(this, s));
  }
  return t.startsWith("pointer") || t.startsWith("touch") || t === "wheel" ? pt(() => {
    e.addEventListener(t, i, r);
  }) : e.addEventListener(t, i, r), i;
}
function cr(t, e, n, r, i) {
  var s = { capture: r, passive: i }, a = Tl(t, e, n, s);
  (e === document.body || // @ts-ignore
  e === window || // @ts-ignore
  e === document || // Firefox has quirky behavior, it can happen that we still get "canplay" events when the element is already removed
  e instanceof HTMLMediaElement) && qr(() => {
    e.removeEventListener(t, a, s);
  });
}
function W(t, e, n) {
  (e[Nt] ?? (e[Nt] = {}))[t] = n;
}
function Ae(t) {
  for (var e = 0; e < t.length; e++)
    zs.add(t[e]);
  for (var n of Sr)
    n(t);
}
let yi = null;
function Ar(t) {
  var y, h;
  var e = this, n = (
    /** @type {Node} */
    e.ownerDocument
  ), r = t.type, i = ((y = t.composedPath) == null ? void 0 : y.call(t)) || [], s = (
    /** @type {null | Element} */
    i[0] || t.target
  );
  yi = t;
  var a = 0, l = yi === t && t[Nt];
  if (l) {
    var o = i.indexOf(l);
    if (o !== -1 && (e === document || e === /** @type {any} */
    window)) {
      t[Nt] = e;
      return;
    }
    var c = i.indexOf(e);
    if (c === -1)
      return;
    o <= c && (a = o);
  }
  if (s = /** @type {Element} */
  i[a] || t.target, s !== e) {
    Xn(t, "currentTarget", {
      configurable: !0,
      get() {
        return s || n;
      }
    });
    var u = F, d = B;
    we(null), Pe(null);
    try {
      for (var f, g = []; s !== null && s !== e; ) {
        try {
          var x = (h = s[Nt]) == null ? void 0 : h[r];
          x != null && (!/** @type {any} */
          s.disabled || // DOM could've been updated already by the time this is reached, so we check this as well
          // -> the target could not have been disabled because it emits the event in the first place
          t.target === s) && x.call(s, t);
        } catch (b) {
          f ? g.push(b) : f = b;
        }
        if (t.cancelBubble) break;
        a++, s = a < i.length ? (
          /** @type {Element} */
          i[a]
        ) : null;
      }
      if (f) {
        for (let b of g)
          queueMicrotask(() => {
            throw b;
          });
        throw f;
      }
    } finally {
      t[Nt] = e, delete t.currentTarget, we(u), Pe(d);
    }
  }
}
var Wi;
const pr = (
  // We gotta write it like this because after downleveling the pure comment may end up in the wrong location
  ((Wi = globalThis == null ? void 0 : globalThis.window) == null ? void 0 : Wi.trustedTypes) && /* @__PURE__ */ globalThis.window.trustedTypes.createPolicy("svelte-trusted-html", {
    /** @param {string} html */
    createHTML: (t) => t
  })
);
function $l(t) {
  return (
    /** @type {string} */
    (pr == null ? void 0 : pr.createHTML(t)) ?? t
  );
}
function Nl(t) {
  var e = Ur("template");
  return e.innerHTML = $l(t.replaceAll("<!>", "<!---->")), e.content;
}
function Wn(t, e) {
  var n = (
    /** @type {Effect} */
    B
  );
  n.nodes === null && (n.nodes = { start: t, end: e, a: null, t: null });
}
// @__NO_SIDE_EFFECTS__
function q(t, e) {
  var n = (e & Ea) !== 0, r = (e & ka) !== 0, i, s = !t.startsWith("<!>");
  return () => {
    if (j)
      return Wn(G, null), G;
    i === void 0 && (i = Nl(s ? t : "<!>" + t), n || (i = /** @type {TemplateNode} */
    /* @__PURE__ */ on(i)));
    var a = (
      /** @type {TemplateNode} */
      r || ws ? document.importNode(i, !0) : i.cloneNode(!0)
    );
    if (n) {
      var l = (
        /** @type {TemplateNode} */
        /* @__PURE__ */ on(a)
      ), o = (
        /** @type {TemplateNode} */
        a.lastChild
      );
      Wn(l, o);
    } else
      Wn(a, a);
    return a;
  };
}
function Y(t, e) {
  if (j) {
    var n = (
      /** @type {Effect & { nodes: EffectNodes }} */
      B
    );
    ((n.f & zt) === 0 || n.nodes.end === null) && (n.nodes.end = G), Mn();
    return;
  }
  t !== null && t.before(
    /** @type {Node} */
    e
  );
}
const Sl = ["touchstart", "touchmove"];
function Al(t) {
  return Sl.includes(t);
}
function ne(t, e) {
  var n = e == null ? "" : typeof e == "object" ? `${e}` : e;
  n !== /** @type {any} */
  (t[pn] ?? (t[pn] = t.nodeValue)) && (t[pn] = n, t.nodeValue = `${n}`);
}
function ei(t, e) {
  return js(t, e);
}
function Ml(t, e) {
  Nr(), e.intro = e.intro ?? !1;
  const n = e.target, r = j, i = G;
  try {
    for (var s = /* @__PURE__ */ on(n); s && (s.nodeType !== An || /** @type {Comment} */
    s.data !== Ui); )
      s = /* @__PURE__ */ bt(s);
    if (!s)
      throw nn;
    Ye(!0), Se(
      /** @type {Comment} */
      s
    );
    const a = js(t, { ...e, anchor: s });
    return Ye(!1), /**  @type {Exports} */
    a;
  } catch (a) {
    if (a instanceof Error && a.message.split(`
`).some((l) => l.startsWith("https://svelte.dev/e/")))
      throw a;
    return a !== nn && console.warn("Failed to hydrate: ", a), e.recover === !1 && za(), Nr(), Cs(n), Ye(!1), ei(t, e);
  } finally {
    Ye(r), Se(i);
  }
}
const Rn = /* @__PURE__ */ new Map();
function js(t, { target: e, anchor: n, props: r = {}, events: i, context: s, intro: a = !0, transformError: l }) {
  Nr();
  var o = void 0, c = yl(() => {
    var u = n ?? e.appendChild(Qe());
    il(
      /** @type {TemplateNode} */
      u,
      {
        pending: () => {
        }
      },
      (g) => {
        ae({});
        var x = (
          /** @type {ComponentContext} */
          me
        );
        if (s && (x.c = s), i && (r.$$events = i), j && Wn(
          /** @type {TemplateNode} */
          g,
          null
        ), o = t(g, r) || {}, j && (B.nodes.end = G, G === null || G.nodeType !== An || /** @type {Comment} */
        G.data !== Hr))
          throw lr(), nn;
        le();
      },
      l
    );
    var d = /* @__PURE__ */ new Set(), f = (g) => {
      for (var x = 0; x < g.length; x++) {
        var y = g[x];
        if (!d.has(y)) {
          d.add(y);
          var h = Al(y);
          for (const p of [e, document]) {
            var b = Rn.get(p);
            b === void 0 && (b = /* @__PURE__ */ new Map(), Rn.set(p, b));
            var m = b.get(y);
            m === void 0 ? (p.addEventListener(y, Ar, { passive: h }), b.set(y, 1)) : b.set(y, m + 1);
          }
        }
      }
    };
    return f(ir(zs)), Sr.add(f), () => {
      var h;
      for (var g of d)
        for (const b of [e, document]) {
          var x = (
            /** @type {Map<string, number>} */
            Rn.get(b)
          ), y = (
            /** @type {number} */
            x.get(g)
          );
          --y == 0 ? (b.removeEventListener(g, Ar), x.delete(g), x.size === 0 && Rn.delete(b)) : x.set(g, y);
        }
      Sr.delete(f), u !== n && ((h = u.parentNode) == null || h.removeChild(u));
    };
  });
  return Mr.set(o, c), o;
}
let Mr = /* @__PURE__ */ new WeakMap();
function Ws(t, e) {
  const n = Mr.get(t);
  return n ? (Mr.delete(t), n(e)) : Promise.resolve();
}
var Ve, Je, Te, Dt, Nn, Sn, rr;
class Il {
  /**
   * @param {TemplateNode} anchor
   * @param {boolean} transition
   */
  constructor(e, n = !0) {
    /** @type {TemplateNode} */
    J(this, "anchor");
    /** @type {Map<Batch, Key>} */
    H(this, Ve, /* @__PURE__ */ new Map());
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
    H(this, Je, /* @__PURE__ */ new Map());
    /**
     * Similar to #onscreen with respect to the keys, but contains branches that are not yet
     * in the DOM, because their insertion is deferred.
     * @type {Map<Key, Branch>}
     */
    H(this, Te, /* @__PURE__ */ new Map());
    /**
     * Keys of effects that are currently outroing
     * @type {Set<Key>}
     */
    H(this, Dt, /* @__PURE__ */ new Set());
    /**
     * Whether to pause (i.e. outro) on change, or destroy immediately.
     * This is necessary for `<svelte:element>`
     */
    H(this, Nn, !0);
    /**
     * @param {Batch} batch
     */
    H(this, Sn, (e) => {
      if (v(this, Ve).has(e)) {
        var n = (
          /** @type {Key} */
          v(this, Ve).get(e)
        ), r = v(this, Je).get(n);
        if (r)
          Qn(r), v(this, Dt).delete(n);
        else {
          var i = v(this, Te).get(n);
          i && (Qn(i.effect), v(this, Je).set(n, i.effect), v(this, Te).delete(n), i.fragment.lastChild.remove(), this.anchor.before(i.fragment), r = i.effect);
        }
        for (const [s, a] of v(this, Ve)) {
          if (v(this, Ve).delete(s), s === e)
            break;
          const l = v(this, Te).get(a);
          l && (ge(l.effect), v(this, Te).delete(a));
        }
        for (const [s, a] of v(this, Je)) {
          if (s === n || v(this, Dt).has(s)) continue;
          const l = () => {
            if (Array.from(v(this, Ve).values()).includes(s)) {
              var c = document.createDocumentFragment();
              Zr(a, c), c.append(Qe()), v(this, Te).set(s, { effect: a, fragment: c });
            } else
              ge(a);
            v(this, Dt).delete(s), v(this, Je).delete(s);
          };
          v(this, Nn) || !r ? (v(this, Dt).add(s), Pt(a, l, !1)) : l();
        }
      }
    });
    /**
     * @param {Batch} batch
     */
    H(this, rr, (e) => {
      v(this, Ve).delete(e);
      const n = Array.from(v(this, Ve).values());
      for (const [r, i] of v(this, Te))
        n.includes(r) || (ge(i.effect), v(this, Te).delete(r));
    });
    this.anchor = e, L(this, Nn, n);
  }
  /**
   *
   * @param {any} key
   * @param {null | ((target: TemplateNode) => void)} fn
   */
  ensure(e, n) {
    var r = (
      /** @type {Batch} */
      R
    ), i = Ts();
    if (n && !v(this, Je).has(e) && !v(this, Te).has(e))
      if (i) {
        var s = document.createDocumentFragment(), a = Qe();
        s.append(a), v(this, Te).set(e, {
          effect: Oe(() => n(a)),
          fragment: s
        });
      } else
        v(this, Je).set(
          e,
          Oe(() => n(this.anchor))
        );
    if (v(this, Ve).set(r, e), i) {
      for (const [l, o] of v(this, Je))
        l === e ? r.unskip_effect(o) : r.skip_effect(o);
      for (const [l, o] of v(this, Te))
        l === e ? r.unskip_effect(o.effect) : r.skip_effect(o.effect);
      r.oncommit(v(this, Sn)), r.ondiscard(v(this, rr));
    } else
      j && (this.anchor = G), v(this, Sn).call(this, r);
  }
}
Ve = new WeakMap(), Je = new WeakMap(), Te = new WeakMap(), Dt = new WeakMap(), Nn = new WeakMap(), Sn = new WeakMap(), rr = new WeakMap();
function it(t, e, n = !1) {
  var r;
  j && (r = G, Mn());
  var i = new Il(t), s = n ? sn : 0;
  function a(l, o) {
    if (j) {
      var c = Qi(
        /** @type {TemplateNode} */
        r
      );
      if (l !== parseInt(c.substring(1))) {
        var u = Kn();
        Se(u), i.anchor = u, Ye(!1), i.ensure(l, o), Ye(!0);
        return;
      }
    }
    i.ensure(l, o);
  }
  Kr(() => {
    var l = !1;
    e((o, c = 0) => {
      l = !0, a(c, o);
    }), l || a(-1, null);
  }, s);
}
function Vs(t, e) {
  return e;
}
function Dl(t, e, n) {
  for (var r = [], i = e.length, s, a = e.length, l = 0; l < i; l++) {
    let d = e[l];
    Pt(
      d,
      () => {
        if (s) {
          if (s.pending.delete(d), s.done.add(d), s.pending.size === 0) {
            var f = (
              /** @type {Set<EachOutroGroup>} */
              t.outrogroups
            );
            Ir(t, ir(s.done)), f.delete(s), f.size === 0 && (t.outrogroups = null);
          }
        } else
          a -= 1;
      },
      !1
    );
  }
  if (a === 0) {
    var o = r.length === 0 && n !== null;
    if (o) {
      var c = (
        /** @type {Element} */
        n
      ), u = (
        /** @type {Element} */
        c.parentNode
      );
      Cs(u), u.append(c), t.items.clear();
    }
    Ir(t, e, !o);
  } else
    s = {
      pending: new Set(e),
      done: /* @__PURE__ */ new Set()
    }, (t.outrogroups ?? (t.outrogroups = /* @__PURE__ */ new Set())).add(s);
}
function Ir(t, e, n = !0) {
  var r;
  if (t.pending.size > 0) {
    r = /* @__PURE__ */ new Set();
    for (const a of t.pending.values())
      for (const l of a)
        r.add(
          /** @type {EachItem} */
          t.items.get(l).e
        );
  }
  for (var i = 0; i < e.length; i++) {
    var s = e[i];
    if (r != null && r.has(s)) {
      s.f |= Ze;
      const a = document.createDocumentFragment();
      Zr(s, a);
    } else
      ge(e[i], n);
  }
}
var _i;
function Us(t, e, n, r, i, s = null) {
  var a = t, l = /* @__PURE__ */ new Map(), o = (e & Vi) !== 0;
  if (o) {
    var c = (
      /** @type {Element} */
      t
    );
    a = j ? Se(/* @__PURE__ */ on(c)) : c.appendChild(Qe());
  }
  j && Mn();
  var u = null, d = /* @__PURE__ */ fs(() => {
    var p = n();
    return (
      /** @type {V[]} */
      Br(p) ? p : p == null ? [] : ir(p)
    );
  }), f, g = /* @__PURE__ */ new Map(), x = !0;
  function y(p) {
    (m.effect.f & Ne) === 0 && (m.pending.delete(p), m.fallback = u, Ll(m, f, a, e, r), u !== null && (f.length === 0 ? (u.f & Ze) === 0 ? Qn(u) : (u.f ^= Ze, bn(u, null, a)) : Pt(u, () => {
      u = null;
    })));
  }
  function h(p) {
    m.pending.delete(p);
  }
  var b = Kr(() => {
    f = /** @type {V[]} */
    I(d);
    var p = f.length;
    let _ = !1;
    if (j) {
      var w = Qi(a) === Pr;
      w !== (p === 0) && (a = Kn(), Se(a), Ye(!1), _ = !0);
    }
    for (var E = /* @__PURE__ */ new Set(), M = (
      /** @type {Batch} */
      R
    ), k = Ts(), T = 0; T < p; T += 1) {
      j && G.nodeType === An && /** @type {Comment} */
      G.data === Hr && (a = /** @type {Comment} */
      G, _ = !0, Ye(!1));
      var C = f[T], $ = r(C, T), D = x ? null : l.get($);
      D ? (D.v && ln(D.v, C), D.i && ln(D.i, T), k && M.unskip_effect(D.e)) : (D = Ol(
        l,
        x ? a : _i ?? (_i = Qe()),
        C,
        $,
        T,
        i,
        e,
        n
      ), x || (D.e.f |= Ze), l.set($, D)), E.add($);
    }
    if (p === 0 && s && !u && (x ? u = Oe(() => s(a)) : (u = Oe(() => s(_i ?? (_i = Qe()))), u.f |= Ze)), p > E.size && Ra(), j && p > 0 && Se(Kn()), !x)
      if (g.set(M, E), k) {
        for (const [ee, Be] of l)
          E.has(ee) || M.skip_effect(Be.e);
        M.oncommit(y), M.ondiscard(h);
      } else
        y(M);
    _ && Ye(!0), I(d);
  }), m = { effect: b, items: l, pending: g, outrogroups: null, fallback: u };
  x = !1, j && (a = G);
}
function un(t) {
  for (; t !== null && (t.f & qe) === 0; )
    t = t.next;
  return t;
}
function Ll(t, e, n, r, i) {
  var C, $, D, ee, Be, Fe, N, be, oi;
  var s = (r & ga) !== 0, a = e.length, l = t.items, o = un(t.effect.first), c, u = null, d, f = [], g = [], x, y, h, b;
  if (s)
    for (b = 0; b < a; b += 1)
      x = e[b], y = i(x, b), h = /** @type {EachItem} */
      l.get(y).e, (h.f & Ze) === 0 && (($ = (C = h.nodes) == null ? void 0 : C.a) == null || $.measure(), (d ?? (d = /* @__PURE__ */ new Set())).add(h));
  for (b = 0; b < a; b += 1) {
    if (x = e[b], y = i(x, b), h = /** @type {EachItem} */
    l.get(y).e, t.outrogroups !== null)
      for (const at of t.outrogroups)
        at.pending.delete(h), at.done.delete(h);
    if ((h.f & ve) !== 0 && (Qn(h), s && ((ee = (D = h.nodes) == null ? void 0 : D.a) == null || ee.unfix(), (d ?? (d = /* @__PURE__ */ new Set())).delete(h))), (h.f & Ze) !== 0)
      if (h.f ^= Ze, h === o)
        bn(h, null, n);
      else {
        var m = u ? u.next : o;
        h === t.effect.last && (t.effect.last = h.prev), h.prev && (h.prev.next = h.next), h.next && (h.next.prev = h.prev), xt(t, u, h), xt(t, h, m), bn(h, m, n), u = h, f = [], g = [], o = un(u.next);
        continue;
      }
    if (h !== o) {
      if (c !== void 0 && c.has(h)) {
        if (f.length < g.length) {
          var p = g[0], _;
          u = p.prev;
          var w = f[0], E = f[f.length - 1];
          for (_ = 0; _ < f.length; _ += 1)
            bn(f[_], p, n);
          for (_ = 0; _ < g.length; _ += 1)
            c.delete(g[_]);
          xt(t, w.prev, E.next), xt(t, u, w), xt(t, E, p), o = p, u = E, b -= 1, f = [], g = [];
        } else
          c.delete(h), bn(h, o, n), xt(t, h.prev, h.next), xt(t, h, u === null ? t.effect.first : u.next), xt(t, u, h), u = h;
        continue;
      }
      for (f = [], g = []; o !== null && o !== h; )
        (c ?? (c = /* @__PURE__ */ new Set())).add(o), g.push(o), o = un(o.next);
      if (o === null)
        continue;
    }
    (h.f & Ze) === 0 && f.push(h), u = h, o = un(h.next);
  }
  if (t.outrogroups !== null) {
    for (const at of t.outrogroups)
      at.pending.size === 0 && (Ir(t, ir(at.done)), (Be = t.outrogroups) == null || Be.delete(at));
    t.outrogroups.size === 0 && (t.outrogroups = null);
  }
  if (o !== null || c !== void 0) {
    var M = [];
    if (c !== void 0)
      for (h of c)
        (h.f & ve) === 0 && M.push(h);
    for (; o !== null; )
      (o.f & ve) === 0 && o !== t.fallback && M.push(o), o = un(o.next);
    var k = M.length;
    if (k > 0) {
      var T = (r & Vi) !== 0 && a === 0 ? n : null;
      if (s) {
        for (b = 0; b < k; b += 1)
          (N = (Fe = M[b].nodes) == null ? void 0 : Fe.a) == null || N.measure();
        for (b = 0; b < k; b += 1)
          (oi = (be = M[b].nodes) == null ? void 0 : be.a) == null || oi.fix();
      }
      Dl(t, M, T);
    }
  }
  s && pt(() => {
    var at, ci;
    if (d !== void 0)
      for (h of d)
        (ci = (at = h.nodes) == null ? void 0 : at.a) == null || ci.apply();
  });
}
function Ol(t, e, n, r, i, s, a, l) {
  var o = (a & va) !== 0 ? (a & ba) === 0 ? /* @__PURE__ */ ys(n, !1, !1) : Ft(n) : null, c = (a & ma) !== 0 ? Ft(i) : null;
  return {
    v: o,
    i: c,
    e: Oe(() => (s(e, o ?? n, c ?? i, l), () => {
      t.delete(r);
    }))
  };
}
function bn(t, e, n) {
  if (t.nodes)
    for (var r = t.nodes.start, i = t.nodes.end, s = e && (e.f & Ze) === 0 ? (
      /** @type {EffectNodes} */
      e.nodes.start
    ) : n; r !== null; ) {
      var a = (
        /** @type {TemplateNode} */
        /* @__PURE__ */ bt(r)
      );
      if (s.before(r), r === i)
        return;
      r = a;
    }
}
function xt(t, e, n) {
  e === null ? t.effect.first = n : e.next = n, n === null ? t.effect.last = e : n.prev = e;
}
function Ys(t, e, n, r, i) {
  var l;
  j && Mn();
  var s = (l = e.$$slots) == null ? void 0 : l[n], a = !1;
  s === !0 && (s = e.children, a = !0), s === void 0 || s(t, a ? () => r : r);
}
function ue(t, e) {
  Ss(() => {
    var n = t.getRootNode(), r = (
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
    if (!r.querySelector("#" + e.hash)) {
      const i = Ur("style");
      i.id = e.hash, i.textContent = e.code, r.appendChild(i);
    }
  });
}
const wi = [...` 	
\r\f \v\uFEFF`];
function Rl(t, e, n) {
  var r = t == null ? "" : "" + t;
  if (n) {
    for (var i of Object.keys(n))
      if (n[i])
        r = r ? r + " " + i : i;
      else if (r.length)
        for (var s = i.length, a = 0; (a = r.indexOf(i, a)) >= 0; ) {
          var l = a + s;
          (a === 0 || wi.includes(r[a - 1])) && (l === r.length || wi.includes(r[l])) ? r = (a === 0 ? "" : r.substring(0, a)) + r.substring(l + 1) : a = l;
        }
  }
  return r === "" ? null : r;
}
function Pl(t, e) {
  return t == null ? null : String(t);
}
function ti(t, e, n, r, i, s) {
  var a = (
    /** @type {any} */
    t[yr]
  );
  if (j || a !== n || a === void 0) {
    var l = Rl(n, r, s);
    (!j || l !== t.getAttribute("class")) && (l == null ? t.removeAttribute("class") : t.className = l), t[yr] = n;
  } else if (s && i !== s)
    for (var o in s) {
      var c = !!s[o];
      (i == null || c !== !!i[o]) && t.classList.toggle(o, c);
    }
  return s;
}
function Xs(t, e, n, r) {
  var i = (
    /** @type {any} */
    t[_r]
  );
  if (j || i !== e) {
    var s = Pl(e);
    (!j || s !== t.getAttribute("style")) && (s == null ? t.removeAttribute("style") : t.style.cssText = s), t[_r] = e;
  }
  return r;
}
function qs(t, e, n = !1) {
  if (t.multiple) {
    if (e == null)
      return;
    if (!Br(e))
      return qa();
    for (var r of t.options)
      r.selected = e.includes(Ei(r));
    return;
  }
  for (r of t.options) {
    var i = Ei(r);
    if (vl(i, e)) {
      r.selected = !0;
      return;
    }
  }
  (!n || e !== void 0) && (t.selectedIndex = -1);
}
function Hl(t) {
  var e = new MutationObserver(() => {
    qs(t, t.__value);
  });
  e.observe(t, {
    // Listen to option element changes
    childList: !0,
    subtree: !0,
    // because of <optgroup>
    // Listen to option element value attribute changes
    // (doesn't get notified of select value changes,
    // because that property is not reflected as an attribute)
    attributes: !0,
    attributeFilter: ["value"]
  }), qr(() => {
    e.disconnect();
  });
}
function Ei(t) {
  return "__value" in t ? t.__value : t.value;
}
const Bl = Symbol("is custom element"), Fl = Symbol("is html"), zl = Zi ? "link" : "LINK", jl = Zi ? "progress" : "PROGRESS";
function nt(t) {
  if (j) {
    var e = !1, n = () => {
      if (!e) {
        if (e = !0, t.hasAttribute("value")) {
          var r = t.value;
          Z(t, "value", null), t.value = r;
        }
        if (t.hasAttribute("checked")) {
          var i = t.checked;
          Z(t, "checked", null), t.checked = i;
        }
      }
    };
    t[Ji] = n, pt(n), ml();
  }
}
function gt(t, e) {
  var n = ni(t);
  n.value === (n.value = // treat null and undefined the same for the initial value
  e ?? void 0) || // @ts-expect-error
  // `progress` elements always need their value set when it's `0`
  t.value === e && (e !== 0 || t.nodeName !== jl) || (t.value = e ?? "");
}
function Gs(t, e) {
  var n = ni(t);
  n.checked !== (n.checked = // treat null and undefined the same for the initial value
  e ?? void 0) && (t.checked = e);
}
function Wl(t, e) {
  e ? t.hasAttribute("selected") || t.setAttribute("selected", "") : t.removeAttribute("selected");
}
function Z(t, e, n, r) {
  var i = ni(t);
  j && (i[e] = t.getAttribute(e), e === "src" || e === "srcset" || e === "href" && t.nodeName === zl) || i[e] !== (i[e] = n) && (e === "loading" && (t[La] = n), n == null ? t.removeAttribute(e) : typeof n != "string" && Ks(t).includes(e) ? t[e] = n : t.setAttribute(e, n));
}
function te(t, e, n) {
  var r = F, i = B;
  let s = j;
  j && Ye(!1), we(null), Pe(null);
  try {
    // `style` should use `set_attribute` rather than the setter
    e !== "style" && // Don't compute setters for custom elements while they aren't registered yet,
    // because during their upgrade/instantiation they might add more setters.
    // Instead, fall back to a simple "an object, then set as property" heuristic.
    (Dr.has(t.getAttribute("is") || t.nodeName) || // customElements may not be available in browser extension contexts
    !customElements || customElements.get(t.getAttribute("is") || t.nodeName.toLowerCase()) ? Ks(t).includes(e) : n && typeof n == "object") ? t[e] = n : Z(t, e, n == null ? n : String(n));
  } finally {
    we(r), Pe(i), s && Ye(!0);
  }
}
function ni(t) {
  return (
    /** @type {Record<string | symbol, unknown>} **/
    /** @type {any} */
    t[Hn] ?? (t[Hn] = {
      [Bl]: t.nodeName.includes("-"),
      [Fl]: t.namespaceURI === Ca
    })
  );
}
var Dr = /* @__PURE__ */ new Map();
function Ks(t) {
  var e = t.getAttribute("is") || t.nodeName, n = Dr.get(e);
  if (n) return n;
  Dr.set(e, n = []);
  for (var r, i = t, s = Element.prototype; s !== i; ) {
    r = $a(i);
    for (var a in r)
      r[a].set && // better safe than sorry, we don't want spread attributes to mess with HTML content
      a !== "innerHTML" && a !== "textContent" && a !== "innerText" && n.push(a);
    i = Xi(i);
  }
  return n;
}
function vr(t, e) {
  return t === e || (t == null ? void 0 : t[Ot]) === e;
}
function ri(t = {}, e, n, r) {
  var i = (
    /** @type {ComponentContext} */
    me.r
  ), s = (
    /** @type {Effect} */
    B
  );
  return Ss(() => {
    var a, l;
    return Gr(() => {
      a = l, l = [], Qr(() => {
        vr(n(...l), t) || (e(t, ...l), a && vr(n(...a), t) && e(null, ...a));
      });
    }), () => {
      let o = s;
      for (; o !== i && o.parent !== null && o.parent.f & xr; )
        o = o.parent;
      const c = () => {
        l && vr(n(...l), t) && e(null, ...l);
      }, u = o.teardown;
      o.teardown = () => {
        c(), u == null || u();
      };
    };
  }), t;
}
function A(t, e, n, r) {
  var _;
  var i = !0, s = (n & _a) !== 0, a = (n & wa) !== 0, l = (
    /** @type {V} */
    r
  ), o = !0, c = (
    /** @type {Derived<V> | undefined} */
    void 0
  ), u = () => a && i ? (c ?? (c = /* @__PURE__ */ wn(
    /** @type {() => V} */
    r
  )), I(c)) : (o && (o = !1, l = a ? Qr(
    /** @type {() => V} */
    r
  ) : (
    /** @type {V} */
    r
  )), l);
  let d;
  if (s) {
    var f = Ot in t || Ki in t;
    d = ((_ = Lt(t, e)) == null ? void 0 : _.set) ?? (f && e in t ? (w) => t[e] = w : void 0);
  }
  var g, x = !1;
  s ? [g, x] = tl(() => (
    /** @type {V} */
    t[e]
  )) : g = /** @type {V} */
  t[e], g === void 0 && r !== void 0 && (g = u(), d && (ja(), d(g)));
  var y;
  if (y = () => {
    var w = (
      /** @type {V} */
      t[e]
    );
    return w === void 0 ? u() : (o = !0, w);
  }, (n & ya) === 0)
    return y;
  if (d) {
    var h = t.$$legacy;
    return (
      /** @type {() => V} */
      (function(w, E) {
        return arguments.length > 0 ? ((!E || h || x) && d(E ? y() : w), w) : y();
      })
    );
  }
  var b = !1, m = ((n & xa) !== 0 ? wn : fs)(() => (b = !1, y()));
  s && I(m);
  var p = (
    /** @type {Effect} */
    B
  );
  return (
    /** @type {() => V} */
    (function(w, E) {
      if (arguments.length > 0) {
        const M = E ? I(m) : s ? dt(w) : w;
        return $e(m, M), b = !0, l !== void 0 && (l = M), w;
      }
      return mt && b || (p.f & Ne) !== 0 ? m.v : I(m);
    })
  );
}
function Vl(t) {
  return new Ul(t);
}
var ft, Le;
class Ul {
  /**
   * @param {ComponentConstructorOptions & {
   *  component: any;
   * }} options
   */
  constructor(e) {
    /** @type {any} */
    H(this, ft);
    /** @type {Record<string, any>} */
    H(this, Le);
    var s;
    var n = /* @__PURE__ */ new Map(), r = (a, l) => {
      var o = /* @__PURE__ */ ys(l, !1, !1);
      return n.set(a, o), o;
    };
    const i = new Proxy(
      { ...e.props || {}, $$events: {} },
      {
        get(a, l) {
          return I(n.get(l) ?? r(l, Reflect.get(a, l)));
        },
        has(a, l) {
          return l === Ki ? !0 : (I(n.get(l) ?? r(l, Reflect.get(a, l))), Reflect.has(a, l));
        },
        set(a, l, o) {
          return $e(n.get(l) ?? r(l, o), o), Reflect.set(a, l, o);
        }
      }
    );
    L(this, Le, (e.hydrate ? Ml : ei)(e.component, {
      target: e.target,
      anchor: e.anchor,
      props: i,
      context: e.context,
      intro: e.intro ?? !1,
      recover: e.recover,
      transformError: e.transformError
    })), (!((s = e == null ? void 0 : e.props) != null && s.$$host) || e.sync === !1) && S(), L(this, ft, i.$$events);
    for (const a of Object.keys(v(this, Le)))
      a === "$set" || a === "$destroy" || a === "$on" || Xn(this, a, {
        get() {
          return v(this, Le)[a];
        },
        /** @param {any} value */
        set(l) {
          v(this, Le)[a] = l;
        },
        enumerable: !0
      });
    v(this, Le).$set = /** @param {Record<string, any>} next */
    (a) => {
      Object.assign(i, a);
    }, v(this, Le).$destroy = () => {
      Ws(v(this, Le));
    };
  }
  /** @param {Record<string, any>} props */
  $set(e) {
    v(this, Le).$set(e);
  }
  /**
   * @param {string} event
   * @param {(...args: any[]) => any} callback
   * @returns {any}
   */
  $on(e, n) {
    v(this, ft)[e] = v(this, ft)[e] || [];
    const r = (...i) => n.call(this, ...i);
    return v(this, ft)[e].push(r), () => {
      v(this, ft)[e] = v(this, ft)[e].filter(
        /** @param {any} fn */
        (i) => i !== r
      );
    };
  }
  $destroy() {
    v(this, Le).$destroy();
  }
}
ft = new WeakMap(), Le = new WeakMap();
let Js;
typeof HTMLElement == "function" && (Js = class extends HTMLElement {
  /**
   * @param {*} $$componentCtor
   * @param {*} $$slots
   * @param {ShadowRootInit | undefined} shadow_root_init
   */
  constructor(e, n, r) {
    super();
    /** The Svelte component constructor */
    J(this, "$$ctor");
    /** Slots */
    J(this, "$$s");
    /** @type {any} The Svelte component instance */
    J(this, "$$c");
    /** Whether or not the custom element is connected */
    J(this, "$$cn", !1);
    /** @type {Record<string, any>} Component props data */
    J(this, "$$d", {});
    /** `true` if currently in the process of reflecting component props back to attributes */
    J(this, "$$r", !1);
    /** @type {Record<string, CustomElementPropDefinition>} Props definition (name, reflected, type etc) */
    J(this, "$$p_d", {});
    /** @type {Record<string, EventListenerOrEventListenerObject[]>} Event listeners */
    J(this, "$$l", {});
    /** @type {Map<EventListenerOrEventListenerObject, Function>} Event listener unsubscribe functions */
    J(this, "$$l_u", /* @__PURE__ */ new Map());
    /** @type {any} The managed render effect for reflecting attributes */
    J(this, "$$me");
    /** @type {ShadowRoot | null} The ShadowRoot of the custom element */
    J(this, "$$shadowRoot", null);
    this.$$ctor = e, this.$$s = n, r && (this.$$shadowRoot = this.attachShadow(r));
  }
  /**
   * @param {string} type
   * @param {EventListenerOrEventListenerObject} listener
   * @param {boolean | AddEventListenerOptions} [options]
   */
  addEventListener(e, n, r) {
    if (this.$$l[e] = this.$$l[e] || [], this.$$l[e].push(n), this.$$c) {
      const i = this.$$c.$on(e, n);
      this.$$l_u.set(n, i);
    }
    super.addEventListener(e, n, r);
  }
  /**
   * @param {string} type
   * @param {EventListenerOrEventListenerObject} listener
   * @param {boolean | AddEventListenerOptions} [options]
   */
  removeEventListener(e, n, r) {
    if (super.removeEventListener(e, n, r), this.$$c) {
      const i = this.$$l_u.get(n);
      i && (i(), this.$$l_u.delete(n));
    }
  }
  async connectedCallback() {
    if (this.$$cn = !0, !this.$$c) {
      let e = function(i) {
        return (s) => {
          const a = Ur("slot");
          i !== "default" && (a.name = i), Y(s, a);
        };
      };
      if (await Promise.resolve(), !this.$$cn || this.$$c)
        return;
      const n = {}, r = Yl(this);
      for (const i of this.$$s)
        i in r && (i === "default" && !this.$$d.children ? (this.$$d.children = e(i), n.default = !0) : n[i] = e(i));
      for (const i of this.attributes) {
        const s = this.$$g_p(i.name);
        s in this.$$d || (this.$$d[s] = Vn(s, i.value, this.$$p_d, "toProp"));
      }
      for (const i in this.$$p_d)
        !(i in this.$$d) && this[i] !== void 0 && (this.$$d[i] = this[i], delete this[i]);
      this.$$c = Vl({
        component: this.$$ctor,
        target: this.$$shadowRoot || this,
        props: {
          ...this.$$d,
          $$slots: n,
          $$host: this
        }
      }), this.$$me = xl(() => {
        Gr(() => {
          var i;
          this.$$r = !0;
          for (const s of Yn(this.$$c)) {
            if (!((i = this.$$p_d[s]) != null && i.reflect)) continue;
            this.$$d[s] = this.$$c[s];
            const a = Vn(
              s,
              this.$$d[s],
              this.$$p_d,
              "toAttribute"
            );
            a == null ? this.removeAttribute(this.$$p_d[s].attribute || s) : this.setAttribute(this.$$p_d[s].attribute || s, a);
          }
          this.$$r = !1;
        });
      });
      for (const i in this.$$l)
        for (const s of this.$$l[i]) {
          const a = this.$$c.$on(i, s);
          this.$$l_u.set(s, a);
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
  attributeChangedCallback(e, n, r) {
    var i;
    this.$$r || (e = this.$$g_p(e), this.$$d[e] = Vn(e, r, this.$$p_d, "toProp"), (i = this.$$c) == null || i.$set({ [e]: this.$$d[e] }));
  }
  disconnectedCallback() {
    this.$$cn = !1, Promise.resolve().then(() => {
      !this.$$cn && this.$$c && (this.$$c.$destroy(), this.$$me(), this.$$c = void 0);
    });
  }
  /**
   * @param {string} attribute_name
   */
  $$g_p(e) {
    return Yn(this.$$p_d).find(
      (n) => this.$$p_d[n].attribute === e || !this.$$p_d[n].attribute && n.toLowerCase() === e
    ) || e;
  }
});
function Vn(t, e, n, r) {
  var s;
  const i = (s = n[t]) == null ? void 0 : s.type;
  if (e = i === "Boolean" && typeof e != "boolean" ? e != null : e, !r || !n[t])
    return e;
  if (r === "toAttribute")
    switch (i) {
      case "Object":
      case "Array":
        return e == null ? null : JSON.stringify(e);
      case "Boolean":
        return e ? "" : null;
      case "Number":
        return e ?? null;
      default:
        return e;
    }
  else
    switch (i) {
      case "Object":
      case "Array":
        return e && JSON.parse(e);
      case "Boolean":
        return e;
      // conversion already handled above
      case "Number":
        return e != null ? +e : e;
      default:
        return e;
    }
}
function Yl(t) {
  const e = {};
  return t.childNodes.forEach((n) => {
    e[
      /** @type {Element} node */
      n.slot || "default"
    ] = !0;
  }), e;
}
function fe(t, e, n, r, i, s) {
  let a = class extends Js {
    constructor() {
      super(t, n, i), this.$$p_d = e;
    }
    static get observedAttributes() {
      return Yn(e).map(
        (l) => (e[l].attribute || l).toLowerCase()
      );
    }
  };
  return Yn(e).forEach((l) => {
    Xn(a.prototype, l, {
      get() {
        return this.$$c && l in this.$$c ? this.$$c[l] : this.$$d[l];
      },
      set(o) {
        var d;
        o = Vn(l, o, e), this.$$d[l] = o;
        var c = this.$$c;
        if (c) {
          var u = (d = Lt(c, l)) == null ? void 0 : d.get;
          u ? c[l] = o : c.$set({ [l]: o });
        }
      }
    });
  }), r.forEach((l) => {
    Xn(a.prototype, l, {
      get() {
        var o;
        return (o = this.$$c) == null ? void 0 : o[l];
      }
    });
  }), t.element = /** @type {any} */
  a, a;
}
var Xl = /* @__PURE__ */ q('<span class="lbl"> </span>'), ql = /* @__PURE__ */ q('<label class="xi-slider svelte-ayba00"><!> <input type="range" class="svelte-ayba00"/> <span class="val svelte-ayba00"> </span></label>');
const Gl = {
  hash: "svelte-ayba00",
  code: '.xi-slider.svelte-ayba00 {display:inline-flex;align-items:center;gap:0.5rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}input[type="range"].svelte-ayba00 {accent-color:var(--xi-accent, #3b82f6);vertical-align:middle;}.val.svelte-ayba00 {min-width:2.5em;text-align:right;font-variant-numeric:tabular-nums;}'
};
function Kl(t, e) {
  ae(e, !0), ue(t, Gl);
  let n = A(e, "value", 15, 0), r = A(e, "min", 7, 0), i = A(e, "max", 7, 100), s = A(e, "step", 7, 1), a = A(e, "label", 7, ""), l = A(e, "disabled", 7, !1);
  const o = e.$$host, c = (p) => o.dispatchEvent(new CustomEvent(p, { detail: { value: n() }, bubbles: !0, composed: !0 }));
  function u(p) {
    n(Number(p.target.value)), c("input");
  }
  function d(p) {
    n(Number(p.target.value)), c("change");
  }
  var f = {
    get value() {
      return n();
    },
    set value(p = 0) {
      n(p), S();
    },
    get min() {
      return r();
    },
    set min(p = 0) {
      r(p), S();
    },
    get max() {
      return i();
    },
    set max(p = 100) {
      i(p), S();
    },
    get step() {
      return s();
    },
    set step(p = 1) {
      s(p), S();
    },
    get label() {
      return a();
    },
    set label(p = "") {
      a(p), S();
    },
    get disabled() {
      return l();
    },
    set disabled(p = !1) {
      l(p), S();
    }
  }, g = ql(), x = P(g);
  {
    var y = (p) => {
      var _ = Xl(), w = P(_, !0);
      O(_), U(() => ne(w, a())), Y(p, _);
    };
    it(x, (p) => {
      a() && p(y);
    });
  }
  var h = V(x, 2);
  nt(h);
  var b = V(h, 2), m = P(b, !0);
  return O(b), O(g), U(() => {
    Z(h, "min", r()), Z(h, "max", i()), Z(h, "step", s()), gt(h, n()), h.disabled = l(), ne(m, n());
  }), W("input", h, u), W("change", h, d), Y(t, g), le(f);
}
Ae(["input", "change"]);
customElements.define("xi-slider", fe(
  Kl,
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
let de = null, pe = null;
const Jl = ["7", "8", "9", "4", "5", "6", "1", "2", "3", "±", "0", "."];
function Zl(t) {
  if (de && de.ownerDocument === t) return de;
  de = t.createElement("div"), de.className = "xi-numpad", de.setAttribute("role", "dialog"), de.setAttribute("aria-label", "numeric keypad"), de.style.cssText = [
    "position:fixed",
    "inset:0",
    "display:none",
    "align-items:center",
    "justify-content:center",
    "background:rgba(0,0,0,.45)",
    "z-index:2147483000"
  ].join(";");
  const e = t.createElement("div");
  e.className = "xi-numpad-card", e.style.cssText = [
    "min-width:16rem",
    "padding:0.75rem",
    "border-radius:10px",
    "background:var(--xi-bg,#fff)",
    "color:var(--xi-fg,#111)",
    "border:1px solid var(--xi-border,#ccc)",
    "box-shadow:0 12px 40px rgba(0,0,0,.4)",
    "font:var(--xi-font,13px system-ui,sans-serif)"
  ].join(";");
  const n = t.createElement("div");
  n.className = "xi-numpad-label", n.style.cssText = "font-size:11px;opacity:.7;min-height:1.2em";
  const r = t.createElement("div");
  r.className = "xi-numpad-display", r.style.cssText = [
    "font-size:1.6rem",
    "text-align:right",
    "padding:0.35rem 0.5rem",
    "margin:0.25rem 0 0.5rem",
    "border:1px solid var(--xi-border,#ccc)",
    "border-radius:6px",
    "font-variant-numeric:tabular-nums",
    "min-height:2rem"
  ].join(";");
  const i = t.createElement("div");
  i.style.cssText = "display:grid;grid-template-columns:repeat(3,1fr);gap:0.4rem";
  const s = (c, u, d) => {
    const f = t.createElement("button");
    return f.type = "button", f.className = u, f.textContent = c, f.style.cssText = [
      "min-height:3rem",
      "font-size:1.15rem",
      "cursor:pointer",
      "color:var(--xi-fg,inherit)",
      "background:var(--xi-bg,#fff)",
      "border:1px solid var(--xi-border,#ccc)",
      "border-radius:8px"
    ].join(";"), f.addEventListener("click", d), f;
  };
  for (const c of Jl) i.appendChild(s(c, "xi-numpad-key", () => ki(c, r)));
  const a = t.createElement("div");
  a.style.cssText = "display:grid;grid-template-columns:repeat(3,1fr);gap:0.4rem;margin-top:0.4rem", a.appendChild(s("⌫", "xi-numpad-back", () => ki("back", r)));
  const l = s("Cancel", "xi-numpad-cancel", () => er()), o = s("OK", "xi-numpad-ok", () => Ql());
  return o.style.background = "var(--xi-accent,#3b82f6)", o.style.color = "#fff", a.appendChild(l), a.appendChild(o), e.append(n, r, i, a), de.appendChild(e), de.addEventListener("click", (c) => {
    c.target === de && er();
  }), t.body.appendChild(de), de._parts = { cap: n, disp: r }, de;
}
function ki(t, e) {
  pe && (t === "back" ? pe.buf = pe.buf.slice(0, -1) : t === "±" ? pe.buf = pe.buf.startsWith("-") ? pe.buf.slice(1) : "-" + pe.buf : t === "." && pe.buf.includes(".") || (pe.buf += t), e.textContent = pe.buf || "0");
}
function er() {
  de && (de.style.display = "none"), pe = null;
}
function Ql() {
  if (!pe) return;
  const { buf: t, min: e, max: n, onCommit: r } = pe;
  let i = Number(t);
  if (t === "" || t === "-" || Number.isNaN(i)) return er();
  e != null && i < e && (i = e), n != null && i > n && (i = n), er(), r == null || r(i);
}
function Zs(t = {}) {
  const e = t.document || globalThis.document;
  if (!e || !e.body) return !1;
  const n = Zl(e);
  return pe = {
    buf: t.value == null || t.value === "" ? "" : String(t.value),
    min: t.min,
    max: t.max,
    onCommit: t.onCommit
  }, n._parts.cap.textContent = t.label || "", n._parts.disp.textContent = pe.buf || "0", n.style.display = "flex", !0;
}
var eo = /* @__PURE__ */ q('<span class="lbl"> </span>'), to = /* @__PURE__ */ q('<label class="xi-number svelte-1gtvx4w"><!> <input type="number" class="svelte-1gtvx4w"/></label>');
const no = {
  hash: "svelte-1gtvx4w",
  code: ".xi-number.svelte-1gtvx4w {display:inline-flex;align-items:center;gap:0.5rem;font:var(--xi-font, 13px system-ui, sans-serif);color:var(--xi-fg, inherit);}input.svelte-1gtvx4w {width:6em;padding:0.15rem 0.3rem;color:var(--xi-fg, inherit);background:var(--xi-bg, #fff);border:1px solid var(--xi-border, #ccc);border-radius:var(--xi-radius, 3px);accent-color:var(--xi-accent, #3b82f6);}input.svelte-1gtvx4w:focus {outline:1px solid var(--xi-accent, #3b82f6);outline-offset:-1px;}"
};
function ro(t, e) {
  ae(e, !0), ue(t, no);
  let n = A(e, "value", 15, 0), r = A(e, "min", 7), i = A(e, "max", 7), s = A(e, "step", 7, 1), a = A(e, "label", 7, ""), l = A(e, "disabled", 7, !1), o = A(e, "numpad", 7, !1);
  const c = e.$$host, u = (_) => c.dispatchEvent(new CustomEvent(_, { detail: { value: n() }, bubbles: !0, composed: !0 })), d = (_) => _.target.value === "" ? null : Number(_.target.value);
  function f(_) {
    n(d(_)), u("input");
  }
  function g(_) {
    n(d(_)), u("change");
  }
  function x() {
    l() || o() === !1 || o() === void 0 || Zs({
      value: n(),
      min: r(),
      max: i(),
      label: a(),
      onCommit: (_) => {
        n(_), u("change");
      }
    });
  }
  var y = {
    get value() {
      return n();
    },
    set value(_ = 0) {
      n(_), S();
    },
    get min() {
      return r();
    },
    set min(_) {
      r(_), S();
    },
    get max() {
      return i();
    },
    set max(_) {
      i(_), S();
    },
    get step() {
      return s();
    },
    set step(_ = 1) {
      s(_), S();
    },
    get label() {
      return a();
    },
    set label(_ = "") {
      a(_), S();
    },
    get disabled() {
      return l();
    },
    set disabled(_ = !1) {
      l(_), S();
    },
    get numpad() {
      return o();
    },
    set numpad(_ = !1) {
      o(_), S();
    }
  }, h = to(), b = P(h);
  {
    var m = (_) => {
      var w = eo(), E = P(w, !0);
      O(w), U(() => ne(E, a())), Y(_, w);
    };
    it(b, (_) => {
      a() && _(m);
    });
  }
  var p = V(b, 2);
  return nt(p), O(h), U(() => {
    Z(p, "min", r()), Z(p, "max", i()), Z(p, "step", s()), gt(p, n()), p.disabled = l();
  }), W("input", p, f), W("change", p, g), cr("focus", p, x), Y(t, h), le(y);
}
Ae(["input", "change"]);
customElements.define("xi-number", fe(
  ro,
  {
    value: { reflect: !0, type: "Number" },
    min: { type: "Number" },
    max: { type: "Number" },
    step: { type: "Number" },
    numpad: { type: "Boolean" },
    label: {},
    disabled: {}
  },
  [],
  [],
  { mode: "open" }
));
var io = /* @__PURE__ */ q('<span class="lbl"> </span>'), so = /* @__PURE__ */ q('<div class="xi-stepper svelte-1p0cy8a"><!> <div class="grp svelte-1p0cy8a"><button type="button" class="pm svelte-1p0cy8a" aria-label="decrement">−</button> <input type="number" class="svelte-1p0cy8a"/> <button type="button" class="pm svelte-1p0cy8a" aria-label="increment">+</button></div></div>');
const ao = {
  hash: "svelte-1p0cy8a",
  code: `.xi-stepper.svelte-1p0cy8a {display:inline-flex;align-items:center;gap:0.5rem;font:var(--xi-font, 13px system-ui, sans-serif);color:var(--xi-fg, inherit);}.grp.svelte-1p0cy8a {display:inline-flex;align-items:stretch;}.pm.svelte-1p0cy8a {
    /* big hit targets: kiosk/touch first */min-width:2.2rem;min-height:2.2rem;font-size:1.1rem;line-height:1;color:var(--xi-fg, inherit);background:var(--xi-bg, #fff);border:1px solid var(--xi-border, #ccc);cursor:pointer;}.pm.svelte-1p0cy8a:first-child {border-radius:var(--xi-radius, 3px) 0 0 var(--xi-radius, 3px);}.pm.svelte-1p0cy8a:last-child  {border-radius:0 var(--xi-radius, 3px) var(--xi-radius, 3px) 0;}.pm.svelte-1p0cy8a:disabled {opacity:0.5;cursor:default;}input.svelte-1p0cy8a {width:4.5em;text-align:center;padding:0.15rem 0.3rem;color:var(--xi-fg, inherit);background:var(--xi-bg, #fff);border:1px solid var(--xi-border, #ccc);border-left:0;border-right:0;accent-color:var(--xi-accent, #3b82f6);-moz-appearance:textfield;}input.svelte-1p0cy8a::-webkit-outer-spin-button, input.svelte-1p0cy8a::-webkit-inner-spin-button {-webkit-appearance:none;margin:0;   /* our own +/- replace the native spinners */}input.svelte-1p0cy8a:focus {outline:1px solid var(--xi-accent, #3b82f6);outline-offset:-1px;}`
};
function lo(t, e) {
  ae(e, !0), ue(t, ao);
  let n = A(e, "value", 15, 0), r = A(e, "min", 7), i = A(e, "max", 7), s = A(e, "step", 7, 1), a = A(e, "label", 7, ""), l = A(e, "disabled", 7, !1), o = A(e, "numpad", 7, !0);
  const c = e.$$host, u = () => c.dispatchEvent(new CustomEvent("change", { detail: { value: n() }, bubbles: !0, composed: !0 })), d = (k) => (r() != null && k < r() && (k = r()), i() != null && k > i() && (k = i()), k), f = (k) => {
    const T = Number(s()) || 1, C = r() != null ? Number(r()) : 0;
    return d(C + Math.round((k - C) / T) * T);
  };
  function g(k) {
    l() || (n(f(Number(n() || 0) + k * (Number(s()) || 1))), u());
  }
  function x(k) {
    const T = k.target.value === "" ? null : Number(k.target.value);
    n(T == null ? T : f(T)), u();
  }
  function y() {
    l() || !o() || Zs({
      value: n(),
      min: r(),
      max: i(),
      label: a(),
      onCommit: (k) => {
        n(f(Number(k))), u();
      }
    });
  }
  var h = {
    get value() {
      return n();
    },
    set value(k = 0) {
      n(k), S();
    },
    get min() {
      return r();
    },
    set min(k) {
      r(k), S();
    },
    get max() {
      return i();
    },
    set max(k) {
      i(k), S();
    },
    get step() {
      return s();
    },
    set step(k = 1) {
      s(k), S();
    },
    get label() {
      return a();
    },
    set label(k = "") {
      a(k), S();
    },
    get disabled() {
      return l();
    },
    set disabled(k = !1) {
      l(k), S();
    },
    get numpad() {
      return o();
    },
    set numpad(k = !0) {
      o(k), S();
    }
  }, b = so(), m = P(b);
  {
    var p = (k) => {
      var T = io(), C = P(T, !0);
      O(T), U(() => ne(C, a())), Y(k, T);
    };
    it(m, (k) => {
      a() && k(p);
    });
  }
  var _ = V(m, 2), w = P(_), E = V(w, 2);
  nt(E);
  var M = V(E, 2);
  return O(_), O(b), U(() => {
    w.disabled = l(), Z(E, "min", r()), Z(E, "max", i()), Z(E, "step", s()), gt(E, n()), E.disabled = l(), M.disabled = l();
  }), W("click", w, () => g(-1)), W("change", E, x), cr("focus", E, y), W("click", M, () => g(1)), Y(t, b), le(h);
}
Ae(["click", "change"]);
customElements.define("xi-stepper", fe(
  lo,
  {
    value: { reflect: !0, type: "Number" },
    min: { type: "Number" },
    max: { type: "Number" },
    step: { type: "Number" },
    label: {},
    disabled: {},
    numpad: {}
  },
  [],
  [],
  { mode: "open" }
));
var oo = /* @__PURE__ */ q('<div class="head svelte-149masm"><span class="lbl"> </span><span class="val svelte-149masm"> </span></div>'), co = /* @__PURE__ */ q('<div><!> <div class="track svelte-149masm"><div class="fill svelte-149masm"></div> <input class="h lo svelte-149masm" type="range"/> <input class="h hi svelte-149masm" type="range"/></div></div>');
const uo = {
  hash: "svelte-149masm",
  code: `.xi-range.svelte-149masm {display:block;font:var(--xi-font, 13px system-ui, sans-serif);color:var(--xi-fg, inherit);}.head.svelte-149masm {display:flex;justify-content:space-between;margin-bottom:0.3rem;}.val.svelte-149masm {color:var(--xi-accent, #3b82f6);font-variant-numeric:tabular-nums;}.track.svelte-149masm {position:relative;height:1.6rem;}.track.svelte-149masm::before {content:"";position:absolute;left:0;right:0;top:50%;height:4px;transform:translateY(-50%);border-radius:2px;background:var(--xi-border, #ccc);}.fill.svelte-149masm {position:absolute;top:50%;height:4px;transform:translateY(-50%);border-radius:2px;background:var(--xi-accent, #3b82f6);}
  /* Two natives stacked on one track; only the thumbs take pointer events, so
     each handle stays independently draggable. */.h.svelte-149masm {position:absolute;left:0;right:0;top:0;width:100%;margin:0;height:1.6rem;background:none;pointer-events:none;-webkit-appearance:none;appearance:none;}.h.svelte-149masm::-webkit-slider-thumb {-webkit-appearance:none;pointer-events:auto;width:1.1rem;height:1.1rem;border-radius:50%;background:var(--xi-accent, #3b82f6);border:2px solid var(--xi-bg, #fff);cursor:pointer;}.h.svelte-149masm::-moz-range-thumb {pointer-events:auto;width:1.1rem;height:1.1rem;border-radius:50%;border:2px solid var(--xi-bg, #fff);background:var(--xi-accent, #3b82f6);cursor:pointer;}.disabled.svelte-149masm {opacity:0.5;}`
};
function fo(t, e) {
  ae(e, !0), ue(t, uo);
  let n = A(e, "low", 15, 0), r = A(e, "high", 15, 100), i = A(e, "min", 7, 0), s = A(e, "max", 7, 100), a = A(e, "step", 7, 1), l = A(e, "label", 7, ""), o = A(e, "disabled", 7, !1);
  const c = e.$$host, u = () => c.dispatchEvent(new CustomEvent("change", {
    detail: {
      low: n(),
      high: r(),
      value: { low: n(), high: r() }
    },
    bubbles: !0,
    composed: !0
  })), d = (k) => Math.min(Math.max(Number(k), Number(i())), Number(s()));
  function f(k) {
    n(Math.min(d(k.target.value), Number(r()))), u();
  }
  function g(k) {
    r(Math.max(d(k.target.value), Number(n()))), u();
  }
  const x = (k) => {
    const T = Number(s()) - Number(i());
    return T > 0 ? (Number(k) - Number(i())) / T * 100 : 0;
  };
  var y = {
    get low() {
      return n();
    },
    set low(k = 0) {
      n(k), S();
    },
    get high() {
      return r();
    },
    set high(k = 100) {
      r(k), S();
    },
    get min() {
      return i();
    },
    set min(k = 0) {
      i(k), S();
    },
    get max() {
      return s();
    },
    set max(k = 100) {
      s(k), S();
    },
    get step() {
      return a();
    },
    set step(k = 1) {
      a(k), S();
    },
    get label() {
      return l();
    },
    set label(k = "") {
      l(k), S();
    },
    get disabled() {
      return o();
    },
    set disabled(k = !1) {
      o(k), S();
    }
  }, h = co();
  let b;
  var m = P(h);
  {
    var p = (k) => {
      var T = oo(), C = P(T), $ = P(C, !0);
      O(C);
      var D = V(C), ee = P(D);
      O(D), O(T), U(() => {
        ne($, l()), ne(ee, `${n() ?? ""} – ${r() ?? ""}`);
      }), Y(k, T);
    };
    it(m, (k) => {
      l() && k(p);
    });
  }
  var _ = V(m, 2), w = P(_), E = V(w, 2);
  nt(E);
  var M = V(E, 2);
  return nt(M), O(_), O(h), U(
    (k, T) => {
      b = ti(h, 1, "xi-range svelte-149masm", null, b, { disabled: o() }), Xs(w, `left:${k ?? ""}%; right:${T ?? ""}%`), Z(E, "min", i()), Z(E, "max", s()), Z(E, "step", a()), gt(E, n()), E.disabled = o(), Z(E, "aria-label", `${l() ?? ""} low`), Z(M, "min", i()), Z(M, "max", s()), Z(M, "step", a()), gt(M, r()), M.disabled = o(), Z(M, "aria-label", `${l() ?? ""} high`);
    },
    [() => x(n()), () => 100 - x(r())]
  ), W("input", E, f), W("input", M, g), Y(t, h), le(y);
}
Ae(["input"]);
customElements.define("xi-range", fe(
  fo,
  {
    low: { reflect: !0, type: "Number" },
    high: { reflect: !0, type: "Number" },
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
var ho = /* @__PURE__ */ q('<span class="lbl"> </span>'), po = /* @__PURE__ */ q('<label class="xi-toggle svelte-1wgpv5v"><input type="checkbox" class="svelte-1wgpv5v"/> <!></label>');
const vo = {
  hash: "svelte-1wgpv5v",
  code: ".xi-toggle.svelte-1wgpv5v {display:inline-flex;align-items:center;gap:0.4rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);cursor:pointer;}input.svelte-1wgpv5v {accent-color:var(--xi-accent, #3b82f6);}"
};
function mo(t, e) {
  ae(e, !0), ue(t, vo);
  let n = A(e, "value", 15, !1), r = A(e, "label", 7, ""), i = A(e, "disabled", 7, !1);
  const s = e.$$host;
  function a(f) {
    n(f.target.checked), s.dispatchEvent(new CustomEvent("change", { detail: { value: n() }, bubbles: !0, composed: !0 }));
  }
  var l = {
    get value() {
      return n();
    },
    set value(f = !1) {
      n(f), S();
    },
    get label() {
      return r();
    },
    set label(f = "") {
      r(f), S();
    },
    get disabled() {
      return i();
    },
    set disabled(f = !1) {
      i(f), S();
    }
  }, o = po(), c = P(o);
  nt(c);
  var u = V(c, 2);
  {
    var d = (f) => {
      var g = ho(), x = P(g, !0);
      O(g), U(() => ne(x, r())), Y(f, g);
    };
    it(u, (f) => {
      r() && f(d);
    });
  }
  return O(o), U(() => {
    Gs(c, n()), c.disabled = i();
  }), W("change", c, a), Y(t, o), le(l);
}
Ae(["change"]);
customElements.define("xi-toggle", fe(
  mo,
  {
    value: { reflect: !0, type: "Boolean" },
    label: {},
    disabled: {}
  },
  [],
  [],
  { mode: "open" }
));
function Qs(t) {
  let e = t;
  if (typeof t == "string")
    try {
      e = JSON.parse(t);
    } catch {
      e = [];
    }
  return Array.isArray(e) ? e.map((n) => n && typeof n == "object" ? { value: n.value, label: n.label ?? String(n.value) } : { value: n, label: String(n) }) : [];
}
var go = /* @__PURE__ */ q('<span class="lbl"> </span>'), bo = /* @__PURE__ */ q('<label class="opt svelte-e7v234"><input type="radio" class="svelte-e7v234"/> <span> </span></label>'), xo = /* @__PURE__ */ q('<div class="xi-radio svelte-e7v234" role="radiogroup"><!> <!></div>');
const yo = {
  hash: "svelte-e7v234",
  code: ".xi-radio.svelte-e7v234 {display:inline-flex;align-items:center;gap:0.75rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}.opt.svelte-e7v234 {display:inline-flex;align-items:center;gap:0.25rem;cursor:pointer;}input.svelte-e7v234 {accent-color:var(--xi-accent, #3b82f6);}"
};
function _o(t, e) {
  ae(e, !0), ue(t, yo);
  let n = A(e, "value", 15, ""), r = A(e, "options", 23, () => []), i = A(e, "label", 7, ""), s = A(e, "disabled", 7, !1), a = A(e, "name", 7, "xi-radio");
  const l = e.$$host, o = /* @__PURE__ */ Tt(() => Qs(r()));
  function c(y) {
    n(y), l.dispatchEvent(new CustomEvent("change", { detail: { value: n() }, bubbles: !0, composed: !0 }));
  }
  var u = {
    get value() {
      return n();
    },
    set value(y = "") {
      n(y), S();
    },
    get options() {
      return r();
    },
    set options(y = []) {
      r(y), S();
    },
    get label() {
      return i();
    },
    set label(y = "") {
      i(y), S();
    },
    get disabled() {
      return s();
    },
    set disabled(y = !1) {
      s(y), S();
    },
    get name() {
      return a();
    },
    set name(y = "xi-radio") {
      a(y), S();
    }
  }, d = xo(), f = P(d);
  {
    var g = (y) => {
      var h = go(), b = P(h, !0);
      O(h), U(() => ne(b, i())), Y(y, h);
    };
    it(f, (y) => {
      i() && y(g);
    });
  }
  var x = V(f, 2);
  return Us(x, 17, () => I(o), Vs, (y, h) => {
    var b = bo(), m = P(b);
    nt(m);
    var p = V(m, 2), _ = P(p, !0);
    O(p), O(b), U(() => {
      Z(m, "name", a()), gt(m, I(h).value), Gs(m, I(h).value === n()), m.disabled = s(), ne(_, I(h).label);
    }), W("change", m, () => c(I(h).value)), Y(y, b);
  }), O(d), Y(t, d), le(u);
}
Ae(["change"]);
customElements.define("xi-radio", fe(
  _o,
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
var wo = /* @__PURE__ */ q('<span class="lbl"> </span>'), Eo = /* @__PURE__ */ q("<option> </option>"), ko = /* @__PURE__ */ q('<label class="xi-dropdown svelte-1qytueg"><!> <select class="svelte-1qytueg"></select></label>');
const Co = {
  hash: "svelte-1qytueg",
  code: ".xi-dropdown.svelte-1qytueg {display:inline-flex;align-items:center;gap:0.5rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}select.svelte-1qytueg {padding:0.15rem 0.3rem;}"
};
function To(t, e) {
  ae(e, !0), ue(t, Co);
  let n = A(e, "value", 15, ""), r = A(e, "options", 23, () => []), i = A(e, "label", 7, ""), s = A(e, "disabled", 7, !1);
  const a = e.$$host, l = /* @__PURE__ */ Tt(() => Qs(r()));
  function o(y) {
    n(y.target.value), a.dispatchEvent(new CustomEvent("change", { detail: { value: n() }, bubbles: !0, composed: !0 }));
  }
  var c = {
    get value() {
      return n();
    },
    set value(y = "") {
      n(y), S();
    },
    get options() {
      return r();
    },
    set options(y = []) {
      r(y), S();
    },
    get label() {
      return i();
    },
    set label(y = "") {
      i(y), S();
    },
    get disabled() {
      return s();
    },
    set disabled(y = !1) {
      s(y), S();
    }
  }, u = ko(), d = P(u);
  {
    var f = (y) => {
      var h = wo(), b = P(h, !0);
      O(h), U(() => ne(b, i())), Y(y, h);
    };
    it(d, (y) => {
      i() && y(f);
    });
  }
  var g = V(d, 2);
  Us(g, 21, () => I(l), Vs, (y, h) => {
    var b = Eo(), m = P(b, !0);
    O(b);
    var p = {};
    U(() => {
      Wl(b, I(h).value === n()), ne(m, I(h).label), p !== (p = I(h).value) && (b.value = (b.__value = I(h).value) ?? "");
    }), Y(y, b);
  }), O(g);
  var x;
  return Hl(g), O(u), U(() => {
    g.disabled = s(), x !== (x = n()) && (g.value = (g.__value = n()) ?? "", qs(g, n()));
  }), W("change", g, o), Y(t, u), le(c);
}
Ae(["change"]);
customElements.define("xi-dropdown", fe(
  To,
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
var $o = /* @__PURE__ */ q('<input class="xi-text svelte-ziilla" type="text"/>');
const No = {
  hash: "svelte-ziilla",
  code: ".xi-text.svelte-ziilla {box-sizing:border-box;width:100%;font:var(--xi-font, 13px system-ui, sans-serif);padding:0.3em 0.5em;color:var(--xi-fg, inherit);background:var(--xi-bg, #fff);border:1px solid var(--xi-border, #ccc);border-radius:var(--xi-radius, 3px);}.xi-text.svelte-ziilla:focus {outline:1px solid var(--xi-accent, #3b82f6);outline-offset:-1px;}"
};
function So(t, e) {
  ae(e, !0), ue(t, No);
  let n = A(e, "value", 15, ""), r = A(e, "placeholder", 7, ""), i = A(e, "disabled", 7, !1);
  const s = e.$$host, a = (d) => s.dispatchEvent(new CustomEvent(d, { detail: { value: n() }, bubbles: !0, composed: !0 }));
  function l(d) {
    n(d.target.value), a("input");
  }
  function o(d) {
    n(d.target.value), a("change");
  }
  var c = {
    get value() {
      return n();
    },
    set value(d = "") {
      n(d), S();
    },
    get placeholder() {
      return r();
    },
    set placeholder(d = "") {
      r(d), S();
    },
    get disabled() {
      return i();
    },
    set disabled(d = !1) {
      i(d), S();
    }
  }, u = $o();
  return nt(u), U(() => {
    gt(u, n()), Z(u, "placeholder", r()), u.disabled = i();
  }), W("input", u, l), W("change", u, o), Y(t, u), le(c);
}
Ae(["input", "change"]);
customElements.define("xi-text", fe(So, { value: { reflect: !0 }, placeholder: {}, disabled: {} }, [], [], { mode: "open" }));
var Ao = /* @__PURE__ */ q('<span class="lbl"> </span>'), Mo = /* @__PURE__ */ q('<label class="xi-file svelte-1if66th"><!> <span class="wrap svelte-1if66th"><input type="text" placeholder="path…" class="svelte-1if66th"/> <button type="button" class="svelte-1if66th">Browse…</button></span></label>');
const Io = {
  hash: "svelte-1if66th",
  code: ".xi-file.svelte-1if66th {display:inline-flex;align-items:center;gap:0.5rem;font:var(--xi-font, 13px system-ui, sans-serif);color:var(--xi-fg, inherit);}.wrap.svelte-1if66th {display:inline-flex;align-items:stretch;gap:0.3rem;min-width:0;}input.svelte-1if66th {flex:1;min-width:0;padding:0.15rem 0.3rem;color:var(--xi-fg, inherit);background:var(--xi-bg, #fff);border:1px solid var(--xi-border, #ccc);border-radius:var(--xi-radius, 3px);}input.svelte-1if66th:focus {outline:1px solid var(--xi-accent, #3b82f6);outline-offset:-1px;}button.svelte-1if66th {min-height:2.2rem;padding:0 0.7rem;cursor:pointer;color:var(--xi-fg, inherit);background:var(--xi-bg, #fff);border:1px solid var(--xi-border, #ccc);border-radius:var(--xi-radius, 3px);}button.svelte-1if66th:disabled {opacity:0.5;cursor:default;}"
};
function Do(t, e) {
  ae(e, !0), ue(t, Io);
  let n = A(e, "value", 15, ""), r = A(e, "label", 7, ""), i = A(e, "disabled", 7, !1), s = A(e, "accept", 7, "");
  const a = e.$$host, l = (b, m) => a.dispatchEvent(new CustomEvent(b, { detail: m, bubbles: !0, composed: !0 }));
  function o(b) {
    n(b.target.value), l("change", { value: n() });
  }
  function c() {
    i() || l("browse", { value: n(), accept: s() });
  }
  var u = {
    get value() {
      return n();
    },
    set value(b = "") {
      n(b), S();
    },
    get label() {
      return r();
    },
    set label(b = "") {
      r(b), S();
    },
    get disabled() {
      return i();
    },
    set disabled(b = !1) {
      i(b), S();
    },
    get accept() {
      return s();
    },
    set accept(b = "") {
      s(b), S();
    }
  }, d = Mo(), f = P(d);
  {
    var g = (b) => {
      var m = Ao(), p = P(m, !0);
      O(m), U(() => ne(p, r())), Y(b, m);
    };
    it(f, (b) => {
      r() && b(g);
    });
  }
  var x = V(f, 2), y = P(x);
  nt(y);
  var h = V(y, 2);
  return O(x), O(d), U(() => {
    gt(y, n()), y.disabled = i(), h.disabled = i();
  }), W("change", y, o), W("click", h, c), Y(t, d), le(u);
}
Ae(["change", "click"]);
customElements.define("xi-file", fe(
  Do,
  {
    value: { reflect: !0, type: "String" },
    label: {},
    disabled: {},
    accept: {}
  },
  [],
  [],
  { mode: "open" }
));
var Lo = /* @__PURE__ */ q('<span class="lbl"> </span>'), Oo = /* @__PURE__ */ q('<label class="xi-color svelte-hifq2s"><!> <span class="wrap svelte-hifq2s"><input type="color" class="svelte-hifq2s"/> <code class="hex svelte-hifq2s"> </code></span></label>');
const Ro = {
  hash: "svelte-hifq2s",
  code: `.xi-color.svelte-hifq2s {display:inline-flex;align-items:center;gap:0.5rem;font:var(--xi-font, 13px system-ui, sans-serif);color:var(--xi-fg, inherit);}.wrap.svelte-hifq2s {display:inline-flex;align-items:center;gap:0.4rem;}input[type="color"].svelte-hifq2s {
    /* a big square swatch — touch-friendly, and reads as a colour, not a field */width:2.2rem;height:2.2rem;padding:0;cursor:pointer;background:none;border:1px solid var(--xi-border, #ccc);border-radius:var(--xi-radius, 3px);}.hex.svelte-hifq2s {font-variant-numeric:tabular-nums;opacity:0.8;}`
};
function Po(t, e) {
  ae(e, !0), ue(t, Ro);
  let n = A(e, "value", 15, "#000000"), r = A(e, "label", 7, ""), i = A(e, "disabled", 7, !1);
  const s = e.$$host, a = (b) => s.dispatchEvent(new CustomEvent(b, { detail: { value: n() }, bubbles: !0, composed: !0 }));
  function l(b) {
    n(b.target.value), a("input");
  }
  function o(b) {
    n(b.target.value), a("change");
  }
  var c = {
    get value() {
      return n();
    },
    set value(b = "#000000") {
      n(b), S();
    },
    get label() {
      return r();
    },
    set label(b = "") {
      r(b), S();
    },
    get disabled() {
      return i();
    },
    set disabled(b = !1) {
      i(b), S();
    }
  }, u = Oo(), d = P(u);
  {
    var f = (b) => {
      var m = Lo(), p = P(m, !0);
      O(m), U(() => ne(p, r())), Y(b, m);
    };
    it(d, (b) => {
      r() && b(f);
    });
  }
  var g = V(d, 2), x = P(g);
  nt(x);
  var y = V(x, 2), h = P(y, !0);
  return O(y), O(g), O(u), U(() => {
    gt(x, n()), x.disabled = i(), ne(h, n());
  }), W("input", x, l), W("change", x, o), Y(t, u), le(c);
}
Ae(["input", "change"]);
customElements.define("xi-color", fe(
  Po,
  {
    value: { reflect: !0, type: "String" },
    label: {},
    disabled: {}
  },
  [],
  [],
  { mode: "open" }
));
function ea() {
  return { scale: 1, panX: 0, panY: 0, imgW: 0, imgH: 0, viewW: 0, viewH: 0 };
}
function ii(t, e, n) {
  return { x: (e - t.panX) / t.scale, y: (n - t.panY) / t.scale };
}
function Ho(t, e, n) {
  return { x: t.panX + e * t.scale, y: t.panY + n * t.scale };
}
const Bo = 0.05, Fo = 64, zo = (t) => Math.max(Bo, Math.min(Fo, t));
function Lr(t) {
  return !t.imgW || !t.imgH || !t.viewW || !t.viewH || (t.scale = Math.min(t.viewW / t.imgW, t.viewH / t.imgH) * 0.95, t.panX = (t.viewW - t.imgW * t.scale) / 2, t.panY = (t.viewH - t.imgH * t.scale) / 2), t;
}
function jo(t) {
  return t.scale = 1, t.panX = (t.viewW - t.imgW) / 2, t.panY = (t.viewH - t.imgH) / 2, t;
}
function ta(t, e, n, r) {
  const { x: i, y: s } = ii(t, e, n);
  return t.scale = zo(t.scale * r), t.panX = e - i * t.scale, t.panY = n - s * t.scale, t;
}
function Wo(t, e, n) {
  return t.panX += e, t.panY += n, t;
}
var Vo = /* @__PURE__ */ q('<canvas class="svelte-1hjcbur"></canvas>');
const Uo = {
  hash: "svelte-1hjcbur",
  code: ":host {display:block;width:100%;height:100%;}canvas.svelte-1hjcbur {width:100%;height:100%;display:block;background:var(--xi-viewer-bg, #111);cursor:grab;touch-action:none;image-rendering:pixelated;}canvas.svelte-1hjcbur:active {cursor:grabbing;}"
};
function Yo(t, e) {
  ae(e, !0), ue(t, Uo);
  const n = e.$$host;
  let r;
  const i = ea();
  let s = null, a = null;
  function l() {
    if (!r) return;
    const w = r.getContext("2d");
    w.imageSmoothingEnabled = !1, w.clearRect(0, 0, r.width, r.height), s && (w.setTransform(i.scale, 0, 0, i.scale, i.panX, i.panY), w.drawImage(s, 0, 0), w.setTransform(1, 0, 0, 1, 0, 0));
  }
  function o() {
    if (!r) return;
    const w = r.getBoundingClientRect();
    r.width = Math.max(1, Math.round(w.width)), r.height = Math.max(1, Math.round(w.height)), i.viewW = r.width, i.viewH = r.height, l();
  }
  function c(w, E) {
    n.dispatchEvent(new CustomEvent(w, { detail: E, bubbles: !0, composed: !0 }));
  }
  function u(w) {
    return !!w && typeof w != "string" && !("dataUrl" in w) && (typeof HTMLImageElement < "u" && w instanceof HTMLImageElement || typeof HTMLCanvasElement < "u" && w instanceof HTMLCanvasElement || typeof OffscreenCanvas < "u" && w instanceof OffscreenCanvas || typeof ImageBitmap < "u" && w instanceof ImageBitmap);
  }
  function d(w) {
    if (u(w)) {
      f(w);
      return;
    }
    const E = new Image();
    E.onload = () => f(E), E.src = typeof w == "string" ? w : w.dataUrl;
  }
  function f(w) {
    const E = !i.imgW;
    s = w, i.imgW = w.naturalWidth || w.width, i.imgH = w.naturalHeight || w.height, a = document.createElement("canvas"), a.width = i.imgW, a.height = i.imgH, a.getContext("2d").drawImage(w, 0, 0), E && Lr(i), l();
  }
  function g(w) {
    if (!s) return;
    w.preventDefault();
    const E = r.getBoundingClientRect();
    ta(i, w.clientX - E.left, w.clientY - E.top, w.deltaY < 0 ? 1.15 : 1 / 1.15), l(), c("viewchange", { scale: i.scale });
  }
  let x = null, y = !1;
  function h(w) {
    var E;
    s && (x = { x: w.clientX, y: w.clientY }, y = !1, (E = r.setPointerCapture) == null || E.call(r, w.pointerId));
  }
  function b(w) {
    if (!x) return;
    const E = w.clientX - x.x, M = w.clientY - x.y;
    (E || M) && (y = !0), Wo(i, E, M), x = { x: w.clientX, y: w.clientY }, l();
  }
  function m(w) {
    x && !y && p(w), x = null;
  }
  function p(w) {
    if (!s || !a) return;
    const E = r.getBoundingClientRect(), M = ii(i, w.clientX - E.left, w.clientY - E.top), k = Math.floor(M.x), T = Math.floor(M.y);
    let C = null;
    if (k >= 0 && T >= 0 && k < i.imgW && T < i.imgH) {
      const $ = a.getContext("2d").getImageData(k, T, 1, 1).data;
      C = [$[0], $[1], $[2]];
    }
    c("pixelpick", { x: k, y: T, rgb: C });
  }
  or(() => {
    n.setFrame = d, n.fit = () => {
      Lr(i), l(), c("viewchange", { scale: i.scale });
    }, n.oneToOne = () => {
      jo(i), l(), c("viewchange", { scale: i.scale });
    }, o();
    const w = new ResizeObserver(o);
    return w.observe(r), () => w.disconnect();
  });
  var _ = Vo();
  ri(_, (w) => r = w, () => r), cr("wheel", _, g), W("pointerdown", _, h), W("pointermove", _, b), W("pointerup", _, m), Y(t, _), le();
}
Ae(["pointerdown", "pointermove", "pointerup"]);
customElements.define("xi-image-viewer", fe(Yo, {}, [], [], { mode: "open" }));
function Xo() {
  let t = null;
  return {
    type: "point",
    onDown(e) {
      t = { x: Math.round(e.x), y: Math.round(e.y) };
    },
    onMove() {
    },
    onUp() {
    },
    onDbl() {
    },
    done() {
      return !!t;
    },
    result() {
      return t ? { ...t } : null;
    },
    draw(e, n) {
      if (!t) return;
      const r = n(t);
      e.fillStyle = "#f59e0b", e.beginPath(), e.arc(r.x, r.y, 4, 0, Math.PI * 2), e.fill();
    }
  };
}
function qo() {
  let t = null, e = null, n = !1;
  const r = () => ({
    x: Math.round(Math.min(t.x, e.x)),
    y: Math.round(Math.min(t.y, e.y)),
    w: Math.round(Math.abs(t.x - e.x)),
    h: Math.round(Math.abs(t.y - e.y))
  });
  return {
    type: "rect",
    onDown(i) {
      t = { ...i }, e = { ...i }, n = !0;
    },
    onMove(i) {
      n && (e = { ...i });
    },
    onUp(i) {
      n && (e = { ...i }, n = !1);
    },
    onDbl() {
    },
    done() {
      return !!(t && e) && (r().w > 0 || r().h > 0);
    },
    result() {
      return t && e ? r() : null;
    },
    draw(i, s) {
      if (!t || !e) return;
      const a = s(t), l = s(e);
      i.strokeStyle = "#f59e0b", i.lineWidth = 1.5, i.strokeRect(Math.min(a.x, l.x), Math.min(a.y, l.y), Math.abs(l.x - a.x), Math.abs(l.y - a.y));
    }
  };
}
function Go() {
  let t = [], e = !1;
  return {
    type: "polygon",
    onDown(n) {
      e || t.push([Math.round(n.x), Math.round(n.y)]);
    },
    onMove() {
    },
    onUp() {
    },
    onDbl() {
      t.length >= 3 && (e = !0);
    },
    done() {
      return e && t.length >= 3;
    },
    result() {
      return t.length >= 3 ? { points: t.map((n) => [...n]), closed: e } : null;
    },
    draw(n, r) {
      if (t.length) {
        n.strokeStyle = "#f59e0b", n.fillStyle = "#f59e0b", n.lineWidth = 1.5, n.beginPath(), t.forEach((i, s) => {
          const a = r({ x: i[0], y: i[1] });
          s ? n.lineTo(a.x, a.y) : n.moveTo(a.x, a.y);
        }), e && n.closePath(), n.stroke();
        for (const i of t) {
          const s = r({ x: i[0], y: i[1] });
          n.beginPath(), n.arc(s.x, s.y, 3, 0, Math.PI * 2), n.fill();
        }
      }
    }
  };
}
const Or = { point: Xo, rect: qo, polygon: Go };
function nu(t, e) {
  Or[t] = e;
}
function Ci(t) {
  return Or[t] ? Or[t]() : null;
}
var Ko = /* @__PURE__ */ q('<div class="xi-editor svelte-ob1m0k"><div class="bar svelte-ob1m0k"><span class="lbl"> </span> <span class="spacer svelte-ob1m0k"></span> <button class="cancel svelte-ob1m0k">Cancel</button> <button class="commit svelte-ob1m0k">Commit</button></div> <canvas class="svelte-ob1m0k"></canvas></div>');
const Jo = {
  hash: "svelte-ob1m0k",
  code: ":host {display:block;width:100%;height:100%;}.xi-editor.svelte-ob1m0k {display:flex;flex-direction:column;height:100%;}.bar.svelte-ob1m0k {display:flex;align-items:center;gap:0.5rem;padding:0.35rem 0.5rem;font:13px system-ui, sans-serif;background:var(--xi-bar-bg, #1f2937);color:#e5e7eb;}.spacer.svelte-ob1m0k {flex:1;}button.svelte-ob1m0k {padding:0.2rem 0.7rem;border:0;border-radius:4px;cursor:pointer;font:inherit;}.commit.svelte-ob1m0k {background:var(--xi-accent, #6366f1);color:#fff;}.cancel.svelte-ob1m0k {background:#374151;color:#e5e7eb;}canvas.svelte-ob1m0k {flex:1;width:100%;display:block;background:var(--xi-viewer-bg, #111);cursor:crosshair;touch-action:none;image-rendering:pixelated;}"
};
function Zo(t, e) {
  ae(e, !0), ue(t, Jo);
  let n = A(e, "tool", 7, "rect"), r = A(e, "label", 7, "");
  const i = e.$$host;
  let s;
  const a = ea();
  let l = null, o = Ci(n());
  const c = (N) => Ho(a, N.x, N.y);
  function u() {
    if (!s) return;
    const N = s.getContext("2d");
    N && (N.imageSmoothingEnabled = !1, N.setTransform(1, 0, 0, 1, 0, 0), N.clearRect(0, 0, s.width, s.height), l && (N.setTransform(a.scale, 0, 0, a.scale, a.panX, a.panY), N.drawImage(l, 0, 0), N.setTransform(1, 0, 0, 1, 0, 0)), o && o.draw(N, c));
  }
  function d() {
    if (!s) return;
    const N = s.getBoundingClientRect();
    s.width = Math.max(1, Math.round(N.width)), s.height = Math.max(1, Math.round(N.height)), a.viewW = s.width, a.viewH = s.height, u();
  }
  function f(N) {
    return !!N && typeof N != "string" && !("dataUrl" in N) && (typeof HTMLImageElement < "u" && N instanceof HTMLImageElement || typeof HTMLCanvasElement < "u" && N instanceof HTMLCanvasElement || typeof OffscreenCanvas < "u" && N instanceof OffscreenCanvas || typeof ImageBitmap < "u" && N instanceof ImageBitmap);
  }
  function g(N) {
    if (f(N)) {
      x(N);
      return;
    }
    const be = new Image();
    be.onload = () => x(be), be.src = typeof N == "string" ? N : N.dataUrl;
  }
  function x(N) {
    const be = !a.imgW;
    l = N, a.imgW = N.naturalWidth || N.width, a.imgH = N.naturalHeight || N.height, be && Lr(a), u();
  }
  function y(N) {
    o = Ci(N) || o, u();
  }
  const h = (N) => {
    const be = s.getBoundingClientRect();
    return ii(a, N.clientX - be.left, N.clientY - be.top);
  };
  function b(N) {
    o && (o.onDown(h(N)), u());
  }
  function m(N) {
    o && N.buttons && (o.onMove(h(N)), u());
  }
  function p(N) {
    o && (o.onUp(h(N)), u());
  }
  function _(N) {
    o && (o.onDbl(h(N)), u());
  }
  function w(N) {
    if (!l) return;
    N.preventDefault();
    const be = s.getBoundingClientRect();
    ta(a, N.clientX - be.left, N.clientY - be.top, N.deltaY < 0 ? 1.15 : 1 / 1.15), u();
  }
  function E() {
    !o || !o.done() || i.dispatchEvent(new CustomEvent("commit", {
      detail: { tool: o.type, result: o.result() },
      bubbles: !0,
      composed: !0
    }));
  }
  function M() {
    i.dispatchEvent(new CustomEvent("cancel", { detail: {}, bubbles: !0, composed: !0 }));
  }
  or(() => {
    i.setFrame = g, i.setTool = y, i.getResult = () => o && o.done() ? o.result() : null, d();
    const N = new ResizeObserver(d);
    return N.observe(s), () => N.disconnect();
  });
  var k = {
    get tool() {
      return n();
    },
    set tool(N = "rect") {
      n(N), S();
    },
    get label() {
      return r();
    },
    set label(N = "") {
      r(N), S();
    }
  }, T = Ko(), C = P(T), $ = P(C), D = P($, !0);
  O($);
  var ee = V($, 4), Be = V(ee, 2);
  O(C);
  var Fe = V(C, 2);
  return ri(Fe, (N) => s = N, () => s), O(T), U(() => ne(D, r() || n())), W("click", ee, M), W("click", Be, E), W("pointerdown", Fe, b), W("pointermove", Fe, m), W("pointerup", Fe, p), W("dblclick", Fe, _), cr("wheel", Fe, w), Y(t, T), le(k);
}
Ae([
  "click",
  "pointerdown",
  "pointermove",
  "pointerup",
  "dblclick"
]);
customElements.define("xi-image-editor", fe(Zo, { tool: {}, label: {} }, [], [], { mode: "open" }));
var Qo = /* @__PURE__ */ q('<span class="ico svelte-1v6o256" aria-hidden="true"> </span>'), ec = /* @__PURE__ */ q("<button><!> <!></button>");
const tc = {
  hash: "svelte-1v6o256",
  code: ".xi-button.svelte-1v6o256 {display:inline-flex;align-items:center;gap:0.4em;font:var(--xi-font, 13px system-ui, sans-serif);padding:0.35em 0.9em;border:1px solid transparent;border-radius:var(--xi-radius, 3px);background:var(--xi-btn-bg, #3b82f6);color:var(--xi-btn-fg, #fff);cursor:pointer;}.xi-button.svelte-1v6o256:hover {background:var(--xi-btn-hover-bg, #2f6fe0);}.xi-button.svelte-1v6o256:focus-visible {outline:1px solid var(--xi-accent, #3b82f6);outline-offset:2px;}.xi-button.secondary.svelte-1v6o256 {background:var(--xi-btn-secondary-bg, #444);color:var(--xi-btn-secondary-fg, #fff);}.xi-button.secondary.svelte-1v6o256:hover {background:var(--xi-btn-secondary-hover-bg, #4f4f4f);}.xi-button.svelte-1v6o256:disabled {opacity:0.5;cursor:default;}.ico.svelte-1v6o256 {font-size:0.9em;line-height:1;}"
};
function nc(t, e) {
  ae(e, !0), ue(t, tc);
  let n = A(e, "secondary", 7, !1), r = A(e, "disabled", 7, !1), i = A(e, "icon", 7, "");
  const s = { add: "＋", play: "▶", "debug-stop": "■", stop: "■" }, a = /* @__PURE__ */ Tt(() => i() ? s[i()] ?? "" : "");
  var l = {
    get secondary() {
      return n();
    },
    set secondary(g = !1) {
      n(g), S();
    },
    get disabled() {
      return r();
    },
    set disabled(g = !1) {
      r(g), S();
    },
    get icon() {
      return i();
    },
    set icon(g = "") {
      i(g), S();
    }
  }, o = ec();
  let c;
  var u = P(o);
  {
    var d = (g) => {
      var x = Qo(), y = P(x, !0);
      O(x), U(() => ne(y, I(a))), Y(g, x);
    };
    it(u, (g) => {
      I(a) && g(d);
    });
  }
  var f = V(u, 2);
  return Ys(f, e, "default", {}), O(o), U(() => {
    c = ti(o, 1, "xi-button svelte-1v6o256", null, c, { secondary: n() }), o.disabled = r();
  }), Y(t, o), le(l);
}
customElements.define("xi-button", fe(
  nc,
  {
    secondary: { reflect: !0, type: "Boolean" },
    disabled: { reflect: !0, type: "Boolean" },
    icon: {}
  },
  ["default"],
  [],
  { mode: "open" }
));
var rc = /* @__PURE__ */ q("<span><!></span>");
const ic = {
  hash: "svelte-e9efnj",
  code: ".xi-badge.svelte-e9efnj {display:inline-flex;align-items:center;font:var(--xi-font, 11px system-ui, sans-serif);font-size:0.85em;line-height:1;padding:0.2em 0.55em;border-radius:var(--xi-radius, 3px);background:var(--xi-badge-bg, #4d4d4d);color:var(--xi-badge-fg, #fff);white-space:nowrap;}.xi-badge.counter.svelte-e9efnj {border-radius:999px;padding:0.2em 0.6em;}"
};
function sc(t, e) {
  ae(e, !0), ue(t, ic);
  let n = A(e, "variant", 7, "");
  var r = {
    get variant() {
      return n();
    },
    set variant(l = "") {
      n(l), S();
    }
  }, i = rc();
  let s;
  var a = P(i);
  return Ys(a, e, "default", {}), O(i), U(() => s = ti(i, 1, "xi-badge svelte-e9efnj", null, s, { counter: n() === "counter" })), Y(t, i), le(r);
}
customElements.define("xi-badge", fe(sc, { variant: { reflect: !0 } }, ["default"], [], { mode: "open" }));
var ac = /* @__PURE__ */ q('<div class="xi-trace svelte-1m1z2sd"><span class="lbl"> </span> <canvas class="svelte-1m1z2sd"></canvas> <span class="val svelte-1m1z2sd"> </span></div>');
const lc = {
  hash: "svelte-1m1z2sd",
  code: ".xi-trace.svelte-1m1z2sd {display:inline-flex;align-items:center;gap:0.5rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}canvas.svelte-1m1z2sd {width:120px;height:28px;display:block;}.val.svelte-1m1z2sd {min-width:3em;text-align:right;font-variant-numeric:tabular-nums;}"
};
function oc(t, e) {
  ae(e, !0), ue(t, lc);
  let n = A(e, "key", 7, ""), r = A(e, "label", 7, ""), i = A(e, "max", 7, 60);
  const s = e.$$host;
  let a, l = /* @__PURE__ */ je(null), o = /* @__PURE__ */ je(dt([]));
  function c() {
    if (!a) return;
    const p = a.getContext && a.getContext("2d");
    if (!p) return;
    const _ = a.width = a.clientWidth || 120, w = a.height = a.clientHeight || 28;
    if (p.clearRect(0, 0, _, w), I(o).length < 2) return;
    const E = Math.min(...I(o)), M = Math.max(...I(o)), k = M - E || 1;
    p.beginPath(), I(o).forEach((T, C) => {
      const $ = C / (I(o).length - 1) * (_ - 2) + 1, D = w - 2 - (T - E) / k * (w - 4);
      C ? p.lineTo($, D) : p.moveTo($, D);
    }), p.strokeStyle = getComputedStyle(s).getPropertyValue("--xi-accent") || "#3b82f6", p.lineWidth = 1.5, p.stroke();
  }
  function u(p) {
    const _ = p && p[n()];
    _ && ($e(l, _.value, !0), typeof _.value == "number" && Number.isFinite(_.value) && ($e(o, [...I(o), _.value].slice(-i()), !0), c()), s.dispatchEvent(new CustomEvent("sample", { detail: { value: _.value }, bubbles: !0, composed: !0 })));
  }
  or(() => {
    s.update = u, Object.defineProperty(s, "latest", { get: () => I(l), configurable: !0 }), Object.defineProperty(s, "history", { get: () => I(o).slice(), configurable: !0 }), c();
  });
  const d = (p) => p == null ? "—" : typeof p == "number" ? Number.isInteger(p) ? p : p.toFixed(3) : String(p);
  var f = {
    get key() {
      return n();
    },
    set key(p = "") {
      n(p), S();
    },
    get label() {
      return r();
    },
    set label(p = "") {
      r(p), S();
    },
    get max() {
      return i();
    },
    set max(p = 60) {
      i(p), S();
    }
  }, g = ac(), x = P(g), y = P(x, !0);
  O(x);
  var h = V(x, 2);
  ri(h, (p) => a = p, () => a);
  var b = V(h, 2), m = P(b, !0);
  return O(b), O(g), U(
    (p) => {
      ne(y, r() || n()), ne(m, p);
    },
    [() => d(I(l))]
  ), Y(t, g), le(f);
}
customElements.define("xi-trace", fe(oc, { key: {}, label: {}, max: { type: "Number" } }, [], [], { mode: "open" }));
const cc = 4003;
function Ti(t, e) {
  const n = t && typeof t.code == "number" ? t.code : null, r = t && t.reason || "";
  return e && e.busy ? { busy: !0, code: n, reason: "single-client-busy" } : n === cc || /single-client-busy/i.test(r) ? { busy: !0, code: n, reason: r || "single-client-busy" } : { busy: !1, code: n, reason: r };
}
class ru {
  /**
   * @param {string} url  e.g. "ws://127.0.0.1:7823/"
   * @param {{WebSocketImpl?: any}} [opts]  inject a WebSocket impl (node tests)
   */
  constructor(e, n = {}) {
    if (this.url = e, this._WS = n.WebSocketImpl || (typeof WebSocket < "u" ? WebSocket : null), !this._WS) throw new Error("no WebSocket implementation (pass opts.WebSocketImpl in node)");
    this.ws = null, this._id = 0, this._pending = /* @__PURE__ */ new Map(), this._listeners = {
      // type -> Set<cb>
      instances: /* @__PURE__ */ new Set(),
      log: /* @__PURE__ */ new Set(),
      event: /* @__PURE__ */ new Set(),
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
  connect(e = {}) {
    return new Promise((n, r) => {
      let i, s = !1;
      try {
        i = new this._WS(this.url);
      } catch (c) {
        r(c);
        return;
      }
      i.binaryType = "arraybuffer", this.ws = i;
      let a = null;
      typeof i.on == "function" && i.on("unexpected-response", (c, u) => {
        const d = u && u.headers && u.headers["x-xi-reason"];
        a = {
          statusCode: u && u.statusCode,
          reason: d,
          busy: u && u.statusCode === 503 && d === "single-client-busy"
        };
      }), i.onmessage = (c) => this._onMessage(c);
      let l = !1;
      const o = (c) => {
        if (l) return;
        l = !0;
        const u = Ti(c, a);
        this._emit("close", u);
        const d = new Error(u.busy ? "single-client-busy: another client owns the backend" : "connection failed before open");
        d.busy = u.busy, d.reason = u.reason, d.code = u.code, r(d);
      };
      i.onerror = () => {
        for (const { reject: c } of this._pending.values()) c(new Error("socket error"));
        this._pending.clear(), s || o(null);
      }, i.onclose = (c) => {
        for (const { reject: u } of this._pending.values()) u(new Error("socket closed"));
        if (this._pending.clear(), !s) {
          o(c);
          return;
        }
        this._emit("close", Ti(c, a));
      }, i.onopen = async () => {
        s = !0, this._emit("open", { url: this.url });
        try {
          if (e.checkVersion) {
            const c = await this.cmd("version"), u = c && c.version;
            if (!(typeof e.checkVersion == "function" ? e.checkVersion(c) : e.checkVersion instanceof RegExp ? e.checkVersion.test(u) : u === e.checkVersion)) {
              r(new Error(`backend version mismatch: got ${u}`)), i.close();
              return;
            }
          }
          n(this);
        } catch (c) {
          r(c);
        }
      };
    });
  }
  _onMessage(e) {
    const n = e.data;
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
  _parseData(e) {
    if (typeof e != "string") return e;
    const n = e.trim();
    if (n.startsWith("{") || n.startsWith("["))
      try {
        return JSON.parse(n);
      } catch {
      }
    return e;
  }
  // --- request/response ---------------------------------------------------
  cmd(e, n) {
    const r = ++this._id;
    return new Promise((i, s) => {
      this._pending.set(r, { resolve: i, reject: s });
      const a = { type: "cmd", id: r, name: e };
      n !== void 0 && (a.args = n);
      try {
        this.ws.send(JSON.stringify(a));
      } catch (l) {
        this._pending.delete(r), s(l);
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
  getInstanceDef(e) {
    return this.cmd("get_instance_def", { name: e });
  }
  setInstanceDef(e, n) {
    return this.cmd("set_instance_def", { name: e, def: n });
  }
  exchange(e, n) {
    return this.cmd("exchange_instance", { name: e, cmd: n });
  }
  // Single-parameter set (doc 31): a named script param → raw JSON value; the
  // plugin validates (rsp ok / error). Used by the manifest param-panel.
  setParam(e, n) {
    return this.cmd("set_param", { name: e, value: n });
  }
  getState(e) {
    return this.cmd("get_state", { name: e });
  }
  prepareInstance(e, n, r) {
    const i = { name: e, def: n };
    return r !== void 0 && (i.folder = r), this.cmd("prepare_instance", i);
  }
  // sel: { instances?, group?, plugin? }
  commitGroup(e) {
    return this.cmd("commit_group", e);
  }
  run(e) {
    return this.cmd("run", e);
  }
  // --- subscriptions (return an unsubscribe fn) ---------------------------
  on(e, n) {
    const r = this._listeners[e];
    if (!r) throw new Error(`unknown event type: ${e}`);
    return r.add(n), () => r.delete(n);
  }
  onInstances(e) {
    return this.on("instances", e);
  }
  onLog(e) {
    return this.on("log", e);
  }
  onEvent(e) {
    return this.on("event", e);
  }
  // Raw binary passthrough: handler(data) gets the ArrayBuffer/Buffer untouched.
  onBinary(e) {
    return this.on("binary", e);
  }
  // Connection lifecycle. onOpen(cb): cb({url}). onClose(cb): cb({busy, code,
  // reason}) — `busy` true means single-client rejection (retry when they leave).
  onOpen(e) {
    return this.on("open", e);
  }
  onClose(e) {
    return this.on("close", e);
  }
  _emit(e, n) {
    for (const r of this._listeners[e])
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
const $i = "__xi_vscode_state__";
function uc(t) {
  return t && typeof t == "object" && !Array.isArray(t) ? { ...t, type: "status" } : { type: "status", value: t };
}
function fc(t = {}) {
  const { client: e, instance: n, onSend: r } = t, i = t.win || globalThis;
  if (!e || typeof e.exchange != "function")
    throw new Error("createVsCodeApi: opts.client needs an exchange(instance, cmd) method");
  if (!n) throw new Error("createVsCodeApi: opts.instance (the panel's instance id) is required");
  return {
    // The panel's ONLY outbound channel. Non-exchange messages are ignored (the
    // real host bridge only forwards exchange; other verbs are host details).
    postMessage(s) {
      if (!(!s || s.type !== "exchange" || !s.cmd)) {
        if (r)
          try {
            r(s.cmd);
          } catch {
          }
        Promise.resolve().then(() => e.exchange(n, s.cmd)).then((a) => i.postMessage(uc(a), "*")).catch((a) => i.postMessage({ type: "status", error: String(a && a.message || a) }, "*"));
      }
    },
    getState() {
      try {
        const s = i.sessionStorage && i.sessionStorage.getItem($i);
        return s ? JSON.parse(s) : null;
      } catch {
        return null;
      }
    },
    setState(s) {
      try {
        i.sessionStorage && i.sessionStorage.setItem($i, JSON.stringify(s));
      } catch {
      }
      return s;
    }
  };
}
function iu(t = {}) {
  const e = t.win || globalThis, n = fc(t), r = e.acquireVsCodeApi;
  return e.acquireVsCodeApi = () => n, {
    api: n,
    uninstall() {
      if (r) e.acquireVsCodeApi = r;
      else
        try {
          delete e.acquireVsCodeApi;
        } catch {
          e.acquireVsCodeApi = void 0;
        }
    }
  };
}
const Ni = {
  slider: "xi-slider",
  numpad: "xi-number",
  // + touch keypad, see the numpad attribute below
  stepper: "xi-stepper",
  toggle: "xi-toggle",
  dropdown: "xi-dropdown",
  radio: "xi-radio",
  text: "xi-text",
  file: "xi-file",
  color: "xi-color",
  range: "xi-range"
}, si = /* @__PURE__ */ new Map();
function dc(t, e) {
  si.set(t, e);
}
function su(t) {
  si.delete(t);
}
function hc(t, e, { label: n, min: r, max: i, step: s }, a) {
  const l = t.createElement(e);
  n && l.setAttribute("label", n);
  for (const [o, c] of [["min", r], ["max", i], ["step", s]])
    c != null && l.setAttribute(o, String(c));
  return l.addEventListener("change", (o) => a(o.detail.value, l)), l;
}
async function au(t, e) {
  const { client: n, instance: r, widgets: i } = e, s = t.ownerDocument || globalThis.document, a = await n.getInstanceDef(r) || {}, l = a.$schema;
  if (!l) return null;
  const o = {};
  for (const [m, p] of Object.entries(a))
    m !== "$schema" && m !== "$v" && m !== "$rev" && (o[m] = p);
  const c = [], u = [], d = async (m, p) => {
    o[m] = p;
    try {
      await n.setInstanceDef(r, { ...o });
    } catch {
    }
    t.dispatchEvent(new CustomEvent("xi-change", { detail: { key: m, value: p }, bubbles: !0 }));
  }, f = (m, p) => {
    p.span && (m.style.gridColumn = `span ${p.span}`), p.rows && (m.style.gridRow = `span ${p.rows}`);
  }, g = (m, p) => {
    if (m) {
      if (m.type === "control") return b(m, p);
      if (m.type === "tabs" || m.children && m.children.some((_) => _.type === "tab"))
        return x(m, p);
      if (m.type === "grid") return y(m, p);
      if (m.type === "section") return h(m, p);
      for (const _ of m.children || []) g(_, p);
    }
  }, x = (m, p) => {
    const _ = (m.children || []).filter((k) => k.type === "tab"), w = s.createElement("div");
    w.className = "xi-tabs";
    const E = s.createElement("div");
    E.className = "xi-tabbar";
    const M = [];
    _.forEach((k, T) => {
      const C = s.createElement("button");
      C.className = "xi-tab", C.type = "button", C.textContent = k.title || `Tab ${T + 1}`;
      const $ = s.createElement("div");
      $.className = "xi-tabpanel", $.style.display = T === 0 ? "" : "none", T === 0 && C.classList.add("active"), C.addEventListener("click", () => {
        M.forEach((D, ee) => {
          D.panel.style.display = ee === T ? "" : "none", D.btn.classList.toggle("active", ee === T);
        });
      });
      for (const D of k.children || []) g(D, $);
      E.appendChild(C), w.appendChild($), M.push({ btn: C, panel: $ });
    }), w.insertBefore(E, w.firstChild), p.appendChild(w);
  }, y = (m, p) => {
    const _ = s.createElement("div");
    _.className = "xi-grid", _.style.display = "grid", _.style.gridTemplateColumns = `repeat(${m.columns || 12}, 1fr)`;
    for (const w of m.children || []) g(w, _);
    p.appendChild(_);
  }, h = (m, p) => {
    const _ = s.createElement("section");
    if (_.className = "xi-section", m.collapsed && (_.dataset.collapsed = "1"), m.title) {
      const E = s.createElement("h3");
      E.className = "xi-section-title", E.textContent = m.title, _.appendChild(E);
    }
    const w = s.createElement("div");
    w.className = "xi-section-body";
    for (const E of m.children || []) g(E, w);
    _.appendChild(w), f(_, m), p.appendChild(_);
  }, b = (m, p) => {
    const _ = m.widget, w = i && _ in i ? i[_] : si.get(_);
    if (typeof w == "function") {
      const C = w(m, { doc: s, client: n, instance: r, state: o, pushDef: d }), $ = C && C.el ? C : { el: C };
      if (!$.el) return;
      f($.el, m), p.appendChild($.el), $.update && c.push({ update: $.update }), $.destroy && u.push($.destroy);
      return;
    }
    const E = typeof w == "string";
    if (!E && (_ === "title" || _ === "label")) {
      const C = s.createElement(_ === "title" ? "h4" : "p");
      C.className = _ === "title" ? "xi-title" : "xi-label", C.textContent = m.label || "", f(C, m), p.appendChild(C);
      return;
    }
    if (!E && _ === "divider") {
      const C = s.createElement("hr");
      C.className = "xi-divider", p.appendChild(C);
      return;
    }
    if (!E && _ === "readout") {
      const C = s.createElement("div");
      C.className = "xi-readout";
      const $ = s.createElement("div");
      $.className = "xi-readout-k", $.textContent = m.label || m.key || "";
      const D = s.createElement("div");
      D.className = "xi-readout-v", D.textContent = String(o[m.key] ?? ""), C.appendChild($), C.appendChild(D), f(C, m), m.key && c.push({ el: D, key: m.key, readout: !0 }), p.appendChild(C);
      return;
    }
    if (!E && _ === "view") {
      const C = s.createElement("div");
      C.className = "xi-view", m.channel && (C.dataset.channel = m.channel), m.label && C.setAttribute("label", m.label), f(C, m), p.appendChild(C);
      return;
    }
    if (!E && _ === "button") {
      const C = s.createElement("button");
      C.className = "xi-button", C.type = "button", C.textContent = m.label || m.command || "", C.addEventListener("click", () => {
        n.exchangeInstance && n.exchangeInstance(r, { command: m.command });
      }), f(C, m), p.appendChild(C);
      return;
    }
    if (!E && _ === "range") {
      const C = s.createElement("xi-range");
      m.label && C.setAttribute("label", m.label);
      for (const [D, ee] of [["min", m.min], ["max", m.max], ["step", m.step]])
        ee != null && C.setAttribute(D, String(ee));
      m.sem && C.setAttribute("data-sem", m.sem), C.addEventListener("change", (D) => {
        const { low: ee, high: Be } = D.detail || {};
        m.key && ee != null && (o[m.key] = ee), m.key2 && Be != null && (o[m.key2] = Be), d(m.key, o[m.key]);
      });
      const $ = s.createElement("div");
      $.className = "xi-control", $.appendChild(C), f($, m), p.appendChild($), m.key in o && (C.low = o[m.key]), m.key2 in o && (C.high = o[m.key2]), c.push({ el: C, key: m.key, key2: m.key2, range: !0 });
      return;
    }
    if (!E && !(_ in Ni)) {
      const C = s.createElement("div");
      C.className = "xi-missing", C.style.cssText = "border:1px dashed #d97706;border-radius:6px;padding:8px 10px;font-size:12px;opacity:.85", C.textContent = `⚠ no component for widget "${_}"` + (m.key ? ` (key: ${m.key})` : "") + ` — wire it with registerWidget("${_}", …)`, f(C, m), p.appendChild(C);
      return;
    }
    const M = E ? w : Ni[_], k = hc(s, M, {
      label: m.label,
      min: m.min,
      max: m.max,
      step: m.step
    }, (C) => d(m.key, C));
    m.sem && k.setAttribute("data-sem", m.sem), M !== `xi-${_}` && k.setAttribute("data-widget", _), _ === "numpad" && k.setAttribute("numpad", "");
    const T = s.createElement("div");
    T.className = "xi-control", T.appendChild(k), f(T, m), p.appendChild(T), m.options != null && (k.options = m.options), m.key in o && (k.value = o[m.key]), c.push({ el: k, key: m.key });
  };
  return t.innerHTML = "", g(l, t), {
    async refresh() {
      const m = await n.getInstanceDef(r) || {};
      for (const [p, _] of Object.entries(m))
        p !== "$schema" && p !== "$v" && p !== "$rev" && (o[p] = _);
      for (const p of c) {
        if (p.update) {
          p.update(o);
          continue;
        }
        if (p.range) {
          p.key in o && (p.el.low = o[p.key]), p.key2 in o && (p.el.high = o[p.key2]);
          continue;
        }
        p.key in o && (p.readout ? p.el.textContent = String(o[p.key]) : p.el.value = o[p.key]);
      }
    },
    destroy() {
      for (const m of u)
        try {
          m();
        } catch {
        }
      t.innerHTML = "";
    }
  };
}
const pc = {
  slider: "xi-slider",
  number: "xi-number",
  toggle: "xi-toggle",
  radio: "xi-radio",
  dropdown: "xi-dropdown",
  text: "xi-text"
};
function na(t, e, n) {
  const r = pc[e.type] || "xi-number", i = t.createElement(r);
  e.label && i.setAttribute("label", e.label);
  for (const s of ["min", "max", "step"]) e[s] != null && i.setAttribute(s, String(e[s]));
  return i.addEventListener("change", (s) => n(s.detail.value, i)), i;
}
function vc(t, { section: e = "Config", tag: n = "control" } = {}) {
  const r = [];
  for (const [i, s] of Object.entries(t || {})) {
    let a = "number";
    if (typeof s == "boolean") a = "toggle";
    else if (typeof s == "string") a = "text";
    else if (typeof s == "number") a = "number";
    else continue;
    r.push({ type: a, key: i, label: i });
  }
  return r.length ? [{ section: e, tag: n, controls: r }] : [];
}
async function lu(t, e) {
  const { client: n, instance: r, sectionFilter: i } = e, s = t.ownerDocument || globalThis.document, a = await n.getInstanceDef(r) || {}, l = { ...a }, o = e.descriptor && e.descriptor.length ? e.descriptor : vc(a), c = [];
  t.innerHTML = "";
  for (const u of o) {
    if (i && !i(u)) continue;
    const d = s.createElement("section");
    if (d.className = "xi-section", d.dataset.tag = u.tag || "control", u.section) {
      const f = s.createElement("h3");
      f.className = "xi-section-title", f.textContent = u.section, d.appendChild(f);
    }
    for (const f of u.controls || []) {
      const g = na(s, f, async (y) => {
        l[f.key] = y;
        try {
          await n.setInstanceDef(r, { ...l });
        } catch {
        }
        t.dispatchEvent(new CustomEvent("xi-change", { detail: { key: f.key, value: y }, bubbles: !0 }));
      }), x = s.createElement("div");
      x.className = "xi-control", x.appendChild(g), d.appendChild(x), f.options != null && (g.options = f.options), f.key in l && (g.value = l[f.key]), c.push({ el: g, key: f.key });
    }
    t.appendChild(d);
  }
  return {
    // Re-read the def from the backend and push values back into the controls.
    async refresh() {
      const u = await n.getInstanceDef(r) || {};
      Object.assign(l, u);
      for (const { el: d, key: f } of c) f in l && (d.value = l[f]);
    },
    destroy() {
      t.innerHTML = "";
    }
  };
}
function mc(t = []) {
  return (t || []).map((e) => {
    let n = e.type;
    return n || (Array.isArray(e.options) ? n = "dropdown" : typeof e.default == "boolean" ? n = "toggle" : e.min != null && e.max != null ? n = "slider" : typeof e.default == "string" ? n = "text" : n = "number"), {
      type: n,
      key: e.name,
      label: e.label || e.name,
      min: e.min,
      max: e.max,
      step: e.step,
      options: e.options,
      default: e.default
    };
  });
}
function ou(t, e) {
  const { client: n, params: r, section: i = "Parameters", values: s = {} } = e, a = t.ownerDocument || globalThis.document, l = mc(r), o = { ...s };
  for (const d of l) !(d.key in o) && d.default !== void 0 && (o[d.key] = d.default);
  const c = [];
  t.innerHTML = "";
  const u = a.createElement("section");
  if (u.className = "xi-section", u.dataset.tag = "param", i) {
    const d = a.createElement("h3");
    d.className = "xi-section-title", d.textContent = i, u.appendChild(d);
  }
  for (const d of l) {
    const f = na(a, d, async (x) => {
      o[d.key] = x;
      try {
        await n.setParam(d.key, x);
      } catch {
      }
      t.dispatchEvent(new CustomEvent("xi-param", { detail: { name: d.key, value: x }, bubbles: !0 }));
    }), g = a.createElement("div");
    g.className = "xi-control", g.appendChild(f), u.appendChild(g), d.options != null && (f.options = d.options), d.key in o && (f.value = o[d.key]), c.push({ el: f, key: d.key });
  }
  return t.appendChild(u), {
    setValues(d) {
      Object.assign(o, d);
      for (const { el: f, key: g } of c) g in o && (f.value = o[g]);
    },
    values() {
      return { ...o };
    },
    destroy() {
      t.innerHTML = "";
    }
  };
}
const Si = -(2n ** 63n), Ai = 2n ** 63n - 1n, Mi = 2n ** 64n - 1n, gc = 64, bc = 2146959360, xc = 0;
class yc {
  constructor(e, n) {
    this.code = e, this.data = n;
  }
}
class ra extends Error {
  constructor(e) {
    super(e), this.name = new.target.name;
  }
}
class ia extends ra {
}
class _c extends ia {
}
class ht extends ra {
}
class fn extends ht {
}
class wc extends ht {
}
class Ii extends ht {
}
class Di extends ht {
}
class Ec extends ht {
}
new TextEncoder();
function Li(t, e) {
  const n = Number(t >> 32n & 0xffffffffn), r = Number(t & 0xffffffffn);
  e.u32(n), e.u32(r);
}
function Oi(t, e) {
  if (e.u8(203), Number.isNaN(t)) {
    e.u32(bc), e.u32(xc);
    return;
  }
  const n = new DataView(new ArrayBuffer(8));
  n.setFloat64(0, t, !1), e.bytes(new Uint8Array(n.buffer));
}
function kc(t, { maxDepth: e = gc, allowExt: n = [] } = {}) {
  const r = new Tc(t, e, new Set(n)), i = r.readValue(0);
  if (r.off !== r.b.length)
    throw new wc(`${r.b.length - r.off} trailing byte(s) after the top-level value`);
  return i;
}
const Cc = new TextDecoder("utf-8", { fatal: !1 });
class Tc {
  constructor(e, n, r) {
    this.b = e, this.dv = new DataView(e.buffer, e.byteOffset, e.byteLength), this.off = 0, this.maxDepth = n, this.allowExt = r;
  }
  _remaining() {
    return this.b.length - this.off;
  }
  _need(e) {
    if (e < 0 || this.off + e > this.b.length)
      throw new fn(`need ${e} byte(s) at offset ${this.off} but only ${this._remaining()} remain`);
    const n = this.off;
    return this.off += e, n;
  }
  _u8() {
    return this.b[this._need(1)];
  }
  _uint(e) {
    const n = this._need(e);
    let r = 0n;
    for (let i = 0; i < e; i++) r = r << 8n | BigInt(this.b[n + i]);
    return r;
  }
  _int(e) {
    const n = this._uint(e), r = BigInt(e * 8);
    return n >= 1n << r - 1n ? n - (1n << r) : n;
  }
  // Represent an integer BigInt under the number policy: number if safe, else BigInt.
  _num(e) {
    return e >= BigInt(Number.MIN_SAFE_INTEGER) && e <= BigInt(Number.MAX_SAFE_INTEGER) ? Number(e) : e;
  }
  readValue(e) {
    if (e > this.maxDepth) throw new Ii(`nesting exceeded maxDepth=${this.maxDepth}`);
    const n = this._u8();
    if (n <= 127) return n;
    if (n >= 224) return n - 256;
    if (n >= 128 && n <= 143) return this._readMap(n & 15, e);
    if (n >= 144 && n <= 159) return this._readArray(n & 15, e);
    if (n >= 160 && n <= 191) return this._readStr(n & 31);
    switch (n) {
      case 192:
        return null;
      case 194:
        return !1;
      case 195:
        return !0;
      case 196:
        return this._readBin(Number(this._uint(1)));
      case 197:
        return this._readBin(Number(this._uint(2)));
      case 198:
        return this._readBin(Number(this._uint(4)));
      case 202: {
        const r = this._need(4);
        return this.dv.getFloat32(r, !1);
      }
      case 203: {
        const r = this._need(8);
        return this.dv.getFloat64(r, !1);
      }
      case 204:
        return this._num(this._uint(1));
      case 205:
        return this._num(this._uint(2));
      case 206:
        return this._num(this._uint(4));
      case 207:
        return this._num(this._uint(8));
      case 208:
        return this._num(this._int(1));
      case 209:
        return this._num(this._int(2));
      case 210:
        return this._num(this._int(4));
      case 211:
        return this._num(this._int(8));
      case 217:
        return this._readStr(Number(this._uint(1)));
      case 218:
        return this._readStr(Number(this._uint(2)));
      case 219:
        return this._readStr(Number(this._uint(4)));
      case 220:
        return this._readArray(Number(this._uint(2)), e);
      case 221:
        return this._readArray(Number(this._uint(4)), e);
      case 222:
        return this._readMap(Number(this._uint(2)), e);
      case 223:
        return this._readMap(Number(this._uint(4)), e);
      case 212:
      case 213:
      case 214:
      case 215:
      case 216:
        return this._readExt(1 << n - 212);
      case 199:
        return this._readExt(Number(this._uint(1)));
      case 200:
        return this._readExt(Number(this._uint(2)));
      case 201:
        return this._readExt(Number(this._uint(4)));
      case 193:
        throw new ht("0xc1 is not a valid msgpack byte");
      default:
        throw new ht(`unsupported msgpack byte 0x${n.toString(16)}`);
    }
  }
  _readStr(e) {
    const n = this._need(e);
    return Cc.decode(this.b.subarray(n, n + e));
  }
  _readBin(e) {
    const n = this._need(e);
    return this.b.slice(n, n + e);
  }
  _readExt(e) {
    const n = Number(this._int(1)), r = this._need(e);
    if (!this.allowExt.has(n)) throw new Ec(`ext type ${n} rejected (not in allowExt)`);
    return new yc(n, this.b.slice(r, r + e));
  }
  _readArray(e, n) {
    if (e > this._remaining())
      throw new fn(`array claims ${e} element(s) but only ${this._remaining()} byte(s) remain`);
    const r = new Array(e);
    for (let i = 0; i < e; i++) r[i] = this.readValue(n + 1);
    return r;
  }
  _readMap(e, n) {
    if (e > Math.floor(this._remaining() / 2))
      throw new fn(`map claims ${e} pair(s) but only ${this._remaining()} byte(s) remain`);
    const r = {};
    for (let i = 0; i < e; i++) {
      const s = this.readValue(n + 1);
      if (typeof s != "string") throw new Di(`canonical map keys must be string, got ${typeof s}`);
      r[s] = this.readValue(n + 1);
    }
    return r;
  }
  // ---- transcode: read one value, re-emit canonically, preserving type ----
  transcodeValue(e, n) {
    if (e > this.maxDepth) throw new Ii(`nesting exceeded maxDepth=${this.maxDepth}`);
    const r = this._u8();
    if (r <= 127) {
      Ge(BigInt(r), n);
      return;
    }
    if (r >= 224) {
      Ge(BigInt(r - 256), n);
      return;
    }
    if (r >= 128 && r <= 143) {
      this._transMap(r & 15, e, n);
      return;
    }
    if (r >= 144 && r <= 159) {
      this._transArray(r & 15, e, n);
      return;
    }
    if (r >= 160 && r <= 191) {
      dn(this._strBytes(r & 31), n);
      return;
    }
    switch (r) {
      case 192:
        n.u8(192);
        return;
      case 194:
        n.u8(194);
        return;
      case 195:
        n.u8(195);
        return;
      case 196:
        mr(this._readBin(Number(this._uint(1))), n);
        return;
      case 197:
        mr(this._readBin(Number(this._uint(2))), n);
        return;
      case 198:
        mr(this._readBin(Number(this._uint(4))), n);
        return;
      case 202: {
        const i = this._need(4);
        Oi(this.dv.getFloat32(i, !1), n);
        return;
      }
      case 203: {
        const i = this._need(8);
        Oi(this.dv.getFloat64(i, !1), n);
        return;
      }
      case 204:
        Ge(this._uint(1), n);
        return;
      case 205:
        Ge(this._uint(2), n);
        return;
      case 206:
        Ge(this._uint(4), n);
        return;
      case 207:
        Ge(this._uint(8), n);
        return;
      case 208:
        Ge(this._int(1), n);
        return;
      case 209:
        Ge(this._int(2), n);
        return;
      case 210:
        Ge(this._int(4), n);
        return;
      case 211:
        Ge(this._int(8), n);
        return;
      case 217:
        dn(this._strBytes(Number(this._uint(1))), n);
        return;
      case 218:
        dn(this._strBytes(Number(this._uint(2))), n);
        return;
      case 219:
        dn(this._strBytes(Number(this._uint(4))), n);
        return;
      case 220:
        this._transArray(Number(this._uint(2)), e, n);
        return;
      case 221:
        this._transArray(Number(this._uint(4)), e, n);
        return;
      case 222:
        this._transMap(Number(this._uint(2)), e, n);
        return;
      case 223:
        this._transMap(Number(this._uint(4)), e, n);
        return;
      case 212:
      case 213:
      case 214:
      case 215:
      case 216:
        this._readExt(1 << r - 212);
        return;
      // throws unless allowed; ext can't be re-emitted
      case 199:
        this._readExt(Number(this._uint(1)));
        return;
      case 200:
        this._readExt(Number(this._uint(2)));
        return;
      case 201:
        this._readExt(Number(this._uint(4)));
        return;
      case 193:
        throw new ht("0xc1 is not a valid msgpack byte");
      default:
        throw new ht(`unsupported msgpack byte 0x${r.toString(16)}`);
    }
    throw new ia("the default canonical writer cannot emit ext types");
  }
  _strBytes(e) {
    const n = this._need(e);
    return this.b.slice(n, n + e);
  }
  _transArray(e, n, r) {
    if (e > this._remaining())
      throw new fn(`array claims ${e} element(s) but only ${this._remaining()} byte(s) remain`);
    r.u8(221), r.u32(e);
    for (let i = 0; i < e; i++) this.transcodeValue(n + 1, r);
  }
  _transMap(e, n, r) {
    if (e > Math.floor(this._remaining() / 2))
      throw new fn(`map claims ${e} pair(s) but only ${this._remaining()} byte(s) remain`);
    r.u8(223), r.u32(e);
    for (let i = 0; i < e; i++) {
      const s = this._u8();
      let a;
      if (s >= 160 && s <= 191) a = this._strBytes(s & 31);
      else if (s === 217) a = this._strBytes(Number(this._uint(1)));
      else if (s === 218) a = this._strBytes(Number(this._uint(2)));
      else if (s === 219) a = this._strBytes(Number(this._uint(4)));
      else throw new Di(`canonical map keys must be string (marker 0x${s.toString(16)})`);
      dn(a, r), this.transcodeValue(n + 1, r);
    }
  }
}
function Ge(t, e) {
  if (t >= Si && t <= Ai) {
    e.u8(211);
    const n = t < 0n ? (1n << 64n) + t : t;
    Li(n, e);
  } else if (t > Ai && t <= Mi)
    e.u8(207), Li(t, e);
  else
    throw new _c(`integer ${t} is outside the canonical range [${Si}, ${Mi}]`);
}
function dn(t, e) {
  e.u8(219), e.u32(t.length), e.bytes(t);
}
function mr(t, e) {
  e.u8(198), e.u32(t.length), e.bytes(t);
}
const $c = {
  u8: { size: 1, read: (t, e) => t.getUint8(e) },
  u16: { size: 2, read: (t, e) => t.getUint16(e, !0) },
  i32: { size: 4, read: (t, e) => t.getInt32(e, !0) },
  f32: { size: 4, read: (t, e) => t.getFloat32(e, !0) },
  f64: { size: 8, read: (t, e) => t.getFloat64(e, !0) }
};
function Dn(t) {
  if (t instanceof DataView) return t;
  if (t instanceof ArrayBuffer) return new DataView(t);
  if (ArrayBuffer.isView(t)) return new DataView(t.buffer, t.byteOffset, t.byteLength);
  throw new Error("payload must be an ArrayBuffer / TypedArray / DataView");
}
function sa(t, e, n) {
  const r = $c[e];
  if (!r) throw new Error(`readScalars: unsupported dt "${e}"`);
  const i = Dn(t);
  if (i.byteLength < n * r.size) throw new Error("readScalars: payload shorter than count*elem_size");
  const s = new Float64Array(n);
  for (let a = 0; a < n; a++) s[a] = r.read(i, a * r.size);
  return s;
}
function aa(t, e) {
  let n, r;
  if (e && e.length === 2)
    n = e[0], r = e[1];
  else {
    n = 1 / 0, r = -1 / 0;
    for (const a of t)
      a < n && (n = a), a > r && (r = a);
    isFinite(n) || (n = 0, r = 0);
  }
  const i = r - n, s = new Float64Array(t.length);
  for (let a = 0; a < t.length; a++)
    s[a] = i > 0 ? Math.min(1, Math.max(0, (t[a] - n) / i)) : 0;
  return { min: n, max: r, norm: s };
}
function gr(t, e, n) {
  return t + (e - t) * n;
}
function Ri(t, e) {
  const n = Math.min(1, Math.max(0, e)) * (t.length - 1), r = Math.floor(n), i = n - r, s = t[r], a = t[Math.min(t.length - 1, r + 1)];
  return [Math.round(gr(s[0], a[0], i)), Math.round(gr(s[1], a[1], i)), Math.round(gr(s[2], a[2], i))];
}
const Nc = [
  [68, 1, 84],
  [72, 40, 120],
  [62, 74, 137],
  [49, 104, 142],
  [38, 130, 142],
  [31, 158, 137],
  [53, 183, 121],
  [110, 206, 88],
  [181, 222, 43],
  [253, 231, 37]
], Sc = [[0, 0, 131], [0, 60, 170], [5, 255, 255], [255, 255, 0], [250, 0, 0], [128, 0, 0]], Pi = {
  gray: (t) => {
    const e = Math.round(Math.min(1, Math.max(0, t)) * 255);
    return [e, e, e];
  },
  viridis: (t) => Ri(Nc, t),
  jet: (t) => Ri(Sc, t)
};
function Ac(t, e, n, r) {
  const i = Dn(t);
  if (i.byteLength < e * n * r) throw new Error("imageRGBA: payload shorter than w*h*c");
  const s = new Uint8ClampedArray(e * n * 4);
  for (let a = 0; a < e * n; a++) {
    const l = a * r, o = a * 4;
    if (r === 1) {
      const c = i.getUint8(l);
      s[o] = s[o + 1] = s[o + 2] = c, s[o + 3] = 255;
    } else if (r === 2) {
      const c = i.getUint8(l);
      s[o] = s[o + 1] = s[o + 2] = c, s[o + 3] = i.getUint8(l + 1);
    } else
      s[o] = i.getUint8(l), s[o + 1] = i.getUint8(l + 1), s[o + 2] = i.getUint8(l + 2), s[o + 3] = r >= 4 ? i.getUint8(l + 3) : 255;
  }
  return s;
}
function Mc(t, e, n, r, { range: i, colormap: s = "viridis" } = {}) {
  const a = sa(t, r, e * n), { norm: l, min: o, max: c } = aa(a, i), u = Pi[s] || Pi.viridis, d = new Uint8ClampedArray(e * n * 4);
  for (let f = 0; f < e * n; f++) {
    const [g, x, y] = u(l[f]), h = f * 4;
    d[h] = g, d[h + 1] = x, d[h + 2] = y, d[h + 3] = 255;
  }
  return { rgba: d, min: o, max: c };
}
function Ic(t, { width: e, height: n, range: r, pad: i = 2 } = {}) {
  const s = t.length, { norm: a } = aa(t, r), l = Math.max(1, e - 2 * i), o = Math.max(1, n - 2 * i), c = new Array(s);
  for (let u = 0; u < s; u++) {
    const d = i + (s === 1 ? l / 2 : u / (s - 1) * l), f = i + (1 - a[u]) * o;
    c[u] = [d, f];
  }
  return c;
}
function Dc(t) {
  const e = t.w | 0, n = t.h | 0;
  return t.n ? t.n | 0 : n === 1 ? e : e === 1 ? n : Math.max(e, n);
}
function Lc(t, { w: e = 1, h: n = 1, vw: r = e, vh: i = n } = {}) {
  const s = r / e, a = i / n, l = (c, u) => [c * s, u * a], o = [];
  for (const c of t || []) {
    const u = c.color || "#39f";
    if (c.type === "point") {
      const [d, f] = l(c.x, c.y);
      o.push({ type: "point", x: d, y: f, r: c.r || 3, color: u });
    } else if (c.type === "box") {
      const [d, f] = l(c.x, c.y);
      o.push({ type: "box", x: d, y: f, w: c.w * s, h: c.h * a, color: u });
    } else c.type === "polyline" && o.push({ type: "polyline", points: (c.points || []).map(([d, f]) => l(d, f)), closed: !!c.closed, color: u });
  }
  return o;
}
function Oc(t) {
  const e = t && typeof t == "object" && !Array.isArray(t) ? t : t instanceof Map ? Object.fromEntries(t) : { value: t }, n = (r) => r === null || typeof r != "object" ? String(r) : JSON.stringify(r);
  return Object.entries(e).map(([r, i]) => [r, n(i)]);
}
function Rc(t, e = 16) {
  const n = Dn(t), r = Math.min(e, n.byteLength), i = [];
  for (let s = 0; s < r; s++) i.push(n.getUint8(s).toString(16).padStart(2, "0"));
  return i.join(" ") + (n.byteLength > r ? " …" : "");
}
function Pc(t, e) {
  const n = e ? Dn(e).byteLength : 0;
  return { type: t && t.t || "unknown", size: n, preview: e ? Rc(e) : "" };
}
function Hc(t = {}) {
  if (t.render && la[t.render]) return t.render;
  if (t.render === "table") return "table";
  const e = t.t, n = t.dt;
  if (e === "xi/image") {
    if (n === "u8") return "image";
    if (n === "f32" || n === "u16" || n === "f64")
      return t.h === 1 || t.w === 1 ? "profile" : "heatmap";
  }
  return "hex";
}
function Pn(t, e, n) {
  const i = (t.ownerDocument || globalThis.document).createElement("canvas");
  return i.width = e, i.height = n, t.innerHTML = "", t.appendChild(i), i;
}
function Hi(t, e, n, r) {
  const i = t.getContext && t.getContext("2d");
  if (!i || !i.putImageData) return !1;
  const s = t.ownerDocument || globalThis.document, a = i.createImageData ? i.createImageData(n, r) : new s.defaultView.ImageData(n, r);
  return a.data.set(e), i.putImageData(a, 0, 0), !0;
}
const la = {
  image(t, { desc: e, payload: n }) {
    const { w: r, h: i, c: s = 1 } = e, a = Ac(n, r, i, s);
    return Hi(Pn(t, r, i), a, r, i), { kind: "image", w: r, h: i, c: s };
  },
  heatmap(t, { desc: e, payload: n }) {
    const { w: r, h: i, dt: s = "f32" } = e, { rgba: a, min: l, max: o } = Mc(n, r, i, s, { range: e.range, colormap: e.colormap });
    return Hi(Pn(t, r, i), a, r, i), { kind: "heatmap", w: r, h: i, min: l, max: o, colormap: e.colormap || "viridis" };
  },
  profile(t, { desc: e, payload: n }) {
    const r = Dc(e), i = sa(n, e.dt || "f32", r), s = e.width || 240, a = e.height || 80, l = Ic(i, { width: s, height: a, range: e.range }), o = Pn(t, s, a), c = o.getContext && o.getContext("2d");
    return c && c.beginPath && (c.strokeStyle = e.color || "#39f", c.beginPath(), l.forEach(([u, d], f) => f ? c.lineTo(u, d) : c.moveTo(u, d)), c.stroke()), { kind: "profile", n: r, points: l };
  },
  overlay(t, { desc: e, refs: n = {} }) {
    const { w: r = 1, h: i = 1 } = e, s = e.width || r, a = e.height || i, l = Lc(e.shapes, { w: r, h: i, vw: s, vh: a }), o = Pn(t, s, a), c = o.getContext && o.getContext("2d");
    if (c && c.beginPath) {
      const u = e.image && n[e.image];
      u && c.drawImage && c.drawImage(u, 0, 0, s, a);
      for (const d of l)
        c.strokeStyle = d.color, c.fillStyle = d.color, c.beginPath(), d.type === "point" ? (c.arc(d.x, d.y, d.r, 0, Math.PI * 2), c.fill()) : d.type === "box" ? c.strokeRect(d.x, d.y, d.w, d.h) : d.type === "polyline" && (d.points.forEach(([f, g], x) => x ? c.lineTo(f, g) : c.moveTo(f, g)), d.closed && c.closePath(), c.stroke());
    }
    return { kind: "overlay", ops: l };
  },
  table(t, { desc: e, payload: n }) {
    let r = e.value;
    if (n)
      try {
        r = kc(n instanceof Uint8Array ? n : new Uint8Array(Dn(n).buffer));
      } catch {
      }
    const i = Oc(r ?? e), s = t.ownerDocument || globalThis.document;
    t.innerHTML = "";
    const a = s.createElement("table");
    a.className = "xi-render-table";
    for (const [l, o] of i) {
      const c = s.createElement("tr"), u = s.createElement("th");
      u.textContent = l;
      const d = s.createElement("td");
      d.textContent = o, c.appendChild(u), c.appendChild(d), a.appendChild(c);
    }
    return t.appendChild(a), { kind: "table", rows: i };
  },
  hex(t, { desc: e, payload: n }) {
    const r = Pc(e, n), i = t.ownerDocument || globalThis.document;
    t.innerHTML = "";
    const s = i.createElement("div");
    s.className = "xi-render-hex";
    const a = i.createElement("div");
    a.className = "xi-hex-type", a.textContent = r.type;
    const l = i.createElement("div");
    l.className = "xi-hex-size", l.textContent = `${r.size} bytes`;
    const o = i.createElement("code");
    return o.className = "xi-hex-preview", o.textContent = r.preview, s.appendChild(a), s.appendChild(l), s.appendChild(o), t.appendChild(s), { kind: "hex", ...r };
  }
};
function cu(t, e) {
  const n = Hc(e.desc || {});
  return la[n](t, e);
}
async function uu(t, e) {
  const { client: n, instance: r, pluginlets: i = [], registry: s, onMissing: a } = e, l = t.ownerDocument || globalThis.document, o = s || await Bc(), c = [];
  for (const u of i) {
    const d = o[u];
    if (typeof d != "function") {
      (a || ((x) => console.warn(`[pluginlet] no UI registered for "${x}"`)))(u);
      continue;
    }
    const f = l.createElement("div");
    f.className = "xi-plet", f.dataset.pluginlet = u, t.appendChild(f);
    const g = await d(f, { client: n, instance: r });
    g ? c.push({ name: u, panel: g }) : f.remove();
  }
  return {
    mounted: c,
    destroy() {
      var u, d;
      for (const f of c) (d = (u = f.panel) == null ? void 0 : u.destroy) == null || d.call(u);
      t.innerHTML = "";
    }
  };
}
let hn = null;
async function Bc() {
  if (hn) return hn;
  try {
    ({ PLUGINLET_UI: hn } = await import("./_virtual_xi-pluginlet-registry-D1QaCHN5.js"));
  } catch {
    hn = {};
  }
  return hn;
}
function Fc(t, e) {
  return { type: "viewport", channel: t, x: e.x | 0, y: e.y | 0, w: e.w | 0, h: e.h | 0 };
}
function fu(t, e, n, r = {}) {
  const i = r.viewportThrottleMs ?? 100, s = document.createElement("canvas");
  s.style.width = "100%", s.style.height = "100%", s.style.display = "block", t.appendChild(s);
  const a = s.getContext("2d");
  let l = 0, o = 0, c = { x: 0, y: 0, w: 0, h: 0 }, u = 0, d = null;
  const f = () => {
    d = null, !(c.w <= 0 || c.h <= 0) && (u = Bi(), n.send(Fc(e, c)));
  }, g = () => {
    if (d !== null) return;
    const E = Math.max(0, i - (Bi() - u));
    d = window.setTimeout(f, E);
  }, x = (E) => {
    const M = new Blob([E], { type: "image/jpeg" }), k = URL.createObjectURL(M), T = new Image();
    T.onload = () => {
      l === 0 && (l = T.naturalWidth, o = T.naturalHeight, c.w === 0 && (c = { x: 0, y: 0, w: l, h: o })), s.width = T.naturalWidth, s.height = T.naturalHeight, a.drawImage(T, 0, 0), URL.revokeObjectURL(k);
    }, T.src = k;
  }, y = (E, M, k) => {
    if (l === 0) return;
    const T = Ut(c.w * k, 16, l), C = Ut(c.h * k, 16, o), $ = E / s.clientWidth, D = M / s.clientHeight;
    c = {
      x: Ut(c.x + (c.w - T) * $, 0, l - T),
      y: Ut(c.y + (c.h - C) * D, 0, o - C),
      w: T,
      h: C
    }, g();
  }, h = (E) => {
    E.preventDefault(), y(E.offsetX, E.offsetY, E.deltaY > 0 ? 1.1 : 1 / 1.1);
  };
  let b = null;
  const m = (E) => b = { x: E.clientX, y: E.clientY }, p = () => b = null, _ = (E) => {
    if (!b || l === 0) return;
    const M = (E.clientX - b.x) / s.clientWidth * c.w, k = (E.clientY - b.y) / s.clientHeight * c.h;
    c = {
      ...c,
      x: Ut(c.x - M, 0, l - c.w),
      y: Ut(c.y - k, 0, o - c.h)
    }, b = { x: E.clientX, y: E.clientY }, g();
  };
  s.addEventListener("wheel", h, { passive: !1 }), s.addEventListener("mousedown", m), window.addEventListener("mouseup", p), window.addEventListener("mousemove", _);
  const w = n.onFrame(e, x);
  return () => {
    d !== null && window.clearTimeout(d), s.removeEventListener("wheel", h), s.removeEventListener("mousedown", m), window.removeEventListener("mouseup", p), window.removeEventListener("mousemove", _), w(), t.removeChild(s);
  };
}
function Ut(t, e, n) {
  return t < e ? e : t > n ? n : t;
}
function Bi() {
  return typeof performance < "u" ? performance.now() : Date.now();
}
var zc = /* @__PURE__ */ q('<div class="stage svelte-1i8due5"><div class="box svelte-1i8due5"><span class="tag svelte-1i8due5"> </span></div></div>');
const jc = {
  hash: "svelte-1i8due5",
  code: `.stage.svelte-1i8due5 {position:relative;width:100%;aspect-ratio:16 / 9;background:linear-gradient(45deg, var(--xi-border, #2b3444) 1px, transparent 1px) 0 0 / 24px 24px,
      var(--xi-bg, #05070c);border:1px solid var(--xi-border, #2b3444);border-radius:6px;overflow:hidden;}.box.svelte-1i8due5 {position:absolute;border:2px solid var(--xi-accent, #3b82f6);background:color-mix(in srgb, var(--xi-accent, #3b82f6) 15%, transparent);border-radius:2px;}.tag.svelte-1i8due5 {position:absolute;right:2px;bottom:2px;font-size:10px;color:var(--xi-accent, #3b82f6);font-variant-numeric:tabular-nums;}`
};
function oa(t, e) {
  ae(e, !0), ue(t, jc);
  let n = A(e, "roi", 7), r = A(e, "img", 23, () => ({ w: 320, h: 180 }));
  const i = (u, d) => Math.max(0, Math.min(100, 100 * u / d));
  var s = {
    get roi() {
      return n();
    },
    set roi(u) {
      n(u), S();
    },
    get img() {
      return r();
    },
    set img(u = { w: 320, h: 180 }) {
      r(u), S();
    }
  }, a = zc(), l = P(a), o = P(l), c = P(o);
  return O(o), O(l), O(a), U(
    (u, d, f, g) => {
      Xs(l, `left:${u ?? ""}%; top:${d ?? ""}%;
              width:${f ?? ""}%; height:${g ?? ""}%`), ne(c, `${n().w ?? ""}×${n().h ?? ""}`);
    },
    [
      () => i(n().x, r().w),
      () => i(n().y, r().h),
      () => i(n().w, r().w),
      () => i(n().h, r().h)
    ]
  ), Y(t, a), le(s);
}
fe(oa, { roi: {}, img: {} }, [], [], { mode: "open" });
var Wc = /* @__PURE__ */ q('<div class="teach svelte-19tk5a0"><div class="head svelte-19tk5a0"><span class="title svelte-19tk5a0">Teach ROI</span> <span class="key svelte-19tk5a0"> </span></div> <!> <div class="fields svelte-19tk5a0"><xi-stepper></xi-stepper> <xi-stepper></xi-stepper> <xi-stepper></xi-stepper> <xi-stepper></xi-stepper></div> <button class="apply svelte-19tk5a0">Apply ROI</button></div>', 2);
const Vc = {
  hash: "svelte-19tk5a0",
  code: ".teach.svelte-19tk5a0 {display:grid;gap:10px;font:var(--xi-font, 13px system-ui, sans-serif);color:var(--xi-fg, inherit);}.head.svelte-19tk5a0 {display:flex;justify-content:space-between;align-items:baseline;}.title.svelte-19tk5a0 {font-weight:600;}.key.svelte-19tk5a0 {font-size:10px;opacity:0.6;font-family:monospace;}.fields.svelte-19tk5a0 {display:grid;grid-template-columns:repeat(2, 1fr);gap:8px;}.apply.svelte-19tk5a0 {padding:8px;border-radius:7px;cursor:pointer;font-weight:600;color:#fff;background:var(--xi-accent, #3b82f6);border:none;}.apply.svelte-19tk5a0:disabled {opacity:0.35;cursor:default;}"
};
function ca(t, e) {
  var C;
  ae(e, !0), ue(t, Vc);
  let n = A(e, "values", 7), r = A(e, "pushDef", 7), i = A(e, "node", 7);
  const s = ((C = i()) == null ? void 0 : C.key) || "roi", a = ($) => {
    const [D, ee, Be, Fe] = String($ ?? "").split(",").map(Number);
    return { x: D || 0, y: ee || 0, w: Be || 120, h: Fe || 80 };
  };
  let l = /* @__PURE__ */ je(dt(a(n()[s])));
  or(() => {
    $e(l, a(n()[s]), !0);
  });
  const o = () => `${I(l).x},${I(l).y},${I(l).w},${I(l).h}` !== String(n()[s] ?? ""), c = ($) => (D) => {
    $e(l, { ...I(l), [$]: Number(D.detail.value) || 0 }, !0);
  };
  var u = {
    get values() {
      return n();
    },
    set values($) {
      n($), S();
    },
    get pushDef() {
      return r();
    },
    set pushDef($) {
      r($), S();
    },
    get node() {
      return i();
    },
    set node($) {
      i($), S();
    }
  }, d = Wc(), f = P(d), g = V(P(f), 2), x = P(g);
  O(g), O(f);
  var y = V(f, 2);
  oa(y, {
    get roi() {
      return I(l);
    }
  });
  var h = V(y, 2), b = P(h);
  te(b, "label", "X"), te(b, "min", "0"), te(b, "max", "320"), te(b, "step", "4"), U(() => te(b, "value", I(l).x));
  var m = /* @__PURE__ */ Tt(() => c("x")), p = V(b, 2);
  te(p, "label", "Y"), te(p, "min", "0"), te(p, "max", "180"), te(p, "step", "4"), U(() => te(p, "value", I(l).y));
  var _ = /* @__PURE__ */ Tt(() => c("y")), w = V(p, 2);
  te(w, "label", "W"), te(w, "min", "8"), te(w, "max", "320"), te(w, "step", "4"), U(() => te(w, "value", I(l).w));
  var E = /* @__PURE__ */ Tt(() => c("w")), M = V(w, 2);
  te(M, "label", "H"), te(M, "min", "8"), te(M, "max", "180"), te(M, "step", "4"), U(() => te(M, "value", I(l).h));
  var k = /* @__PURE__ */ Tt(() => c("h"));
  O(h);
  var T = V(h, 2);
  return O(d), U(
    ($) => {
      ne(x, `def: ${s ?? ""}`), T.disabled = $;
    },
    [() => !o()]
  ), W("change", b, function(...$) {
    var D;
    (D = I(m)) == null || D.apply(this, $);
  }), W("change", p, function(...$) {
    var D;
    (D = I(_)) == null || D.apply(this, $);
  }), W("change", w, function(...$) {
    var D;
    (D = I(E)) == null || D.apply(this, $);
  }), W("change", M, function(...$) {
    var D;
    (D = I(k)) == null || D.apply(this, $);
  }), W("click", T, () => r()(s, `${I(l).x},${I(l).y},${I(l).w},${I(l).h}`)), Y(t, d), le(u);
}
Ae(["change", "click"]);
fe(ca, { values: {}, pushDef: {}, node: {} }, [], [], { mode: "open" });
function du() {
  dc("teach_panel", (t, { instance: e, state: n, pushDef: r }) => {
    const i = document.createElement("div"), s = dt({ values: { ...n }, pushDef: r, node: t, instance: e }), a = ei(ca, { target: i, props: s });
    return {
      el: i,
      update: (l) => {
        s.values = { ...l };
      },
      // refresh → runes take it from here
      destroy: () => Ws(a)
    };
  });
}
const Uc = `
  :host { display:block; height:100%; box-sizing:border-box; background:#1e1e1e;
          border:1px solid #333; border-radius:6px; color:#ddd; overflow:hidden;
          font:13px/1.4 system-ui,sans-serif; }
  .hd { padding:4px 8px; font-size:11px; letter-spacing:.04em; text-transform:uppercase;
        color:#888; border-bottom:1px solid #2a2a2a; }
  .body { padding:8px; height:calc(100% - 24px); box-sizing:border-box; }
`;
function ur(t, e) {
  return t.attachShadow({ mode: "open" }), t.shadowRoot.innerHTML = `<style>${Uc}</style>
    <div class="hd">${e || ""}</div><div class="body"></div>`, t.shadowRoot.querySelector(".body");
}
const Yc = (t, e) => t.config && t.config.title || e;
function ua(t) {
  return t == null ? { kind: "none", label: "—", color: "#bbb" } : t <= -99e4 ? { kind: "sys", label: "SYS", color: "#c084fc" } : t > 0 ? { kind: "ok", label: t > 1 ? `OK${t}` : "OK", color: "#3ad17a" } : t < 0 ? { kind: "ng", label: t < -1 ? `NG${-t}` : "NG", color: "#ff5b5b" } : { kind: "na", label: "NA", color: "#ffb454" };
}
class Xc extends HTMLElement {
  connectedCallback() {
    this.body = ur(this, Yc(this, "Verdict")), this.body.style.cssText = "display:flex;flex-direction:column;align-items:center;justify-content:center;gap:6px;text-align:center", this.big = document.createElement("div"), this.big.style.cssText = "font-weight:800;font-size:clamp(20px,7vw,72px);line-height:1", this.sub = document.createElement("div"), this.sub.style.cssText = "font-size:12px;color:#888;max-width:96%;overflow:hidden;text-overflow:ellipsis;white-space:nowrap", this.body.append(this.big, this.sub);
  }
  feed(e) {
    const n = e.result, r = ua(n ? n.code : null);
    this.big.textContent = r.label, this.big.style.color = r.color, this.sub.textContent = n && n.msg ? n.msg : "";
  }
}
class qc extends HTMLElement {
  connectedCallback() {
    var e, n;
    this.body = ur(this, ((e = this.config) == null ? void 0 : e.title) || "Throughput"), this.windowSec = ((n = this.config) == null ? void 0 : n.windowSec) || 60, this.stamps = [], this.lastResult = -1, this.lastCompute = null, this.body.style.cssText = "display:flex;flex-direction:column;align-items:center;justify-content:center;gap:4px", this.big = document.createElement("div"), this.big.style.cssText = "font-size:clamp(18px,6vw,44px);font-weight:700;color:#9ad", this.sub = document.createElement("div"), this.sub.style.cssText = "font-size:12px;color:#888", this.body.append(this.big, this.sub), this.timer = setInterval(() => this.render(), 1e3);
  }
  disconnectedCallback() {
    this.timer && (clearInterval(this.timer), this.timer = 0);
  }
  feed(e) {
    const n = e.result;
    n && n.run_id != null && n.run_id !== this.lastResult && (this.lastResult = n.run_id, this.stamps.push(Date.now())), e.compute_ms != null && (this.lastCompute = e.compute_ms), this.render();
  }
  render() {
    var a, l;
    const e = Date.now(), n = e - this.windowSec * 1e3;
    for (; this.stamps.length && this.stamps[0] < n; ) this.stamps.shift();
    const r = this.stamps.length, i = r ? Math.max((e - this.stamps[0]) / 1e3, 1) : this.windowSec, s = r > 1 ? r / i * 60 : 0;
    this.big.textContent = `${s.toFixed(0)} /min`, this.sub.textContent = `${r} in ${this.windowSec}s` + (this.lastCompute != null ? ` · compute ${((l = (a = this.lastCompute).toFixed) == null ? void 0 : l.call(a, 1)) ?? this.lastCompute} ms` : "");
  }
}
class Gc extends HTMLElement {
  connectedCallback() {
    var e;
    this.body = ur(this, ((e = this.config) == null ? void 0 : e.title) || "Yield"), this.ok = 0, this.ng = 0, this.last = -1, this.body.style.cssText = "display:flex;flex-direction:column;align-items:center;justify-content:center;gap:4px", this.big = document.createElement("div"), this.big.style.cssText = "font-size:clamp(18px,6vw,44px);font-weight:700", this.sub = document.createElement("div"), this.sub.style.cssText = "font-size:12px;color:#888", this.body.append(this.big, this.sub);
  }
  feed(e) {
    var s;
    const n = e.result;
    if (n && n.run_id != null && n.run_id !== this.last) {
      this.last = n.run_id;
      const a = ua(n.code);
      a.kind === "ok" ? this.ok++ : a.kind === "ng" ? this.ng++ : a.kind === "na" && (this.na = (this.na || 0) + 1);
    }
    const r = this.ok + this.ng, i = r ? 100 * this.ok / r : 0;
    this.big.textContent = `${i.toFixed(1)}%`, this.big.style.color = i >= (((s = this.config) == null ? void 0 : s.warn) ?? 95) ? "#3ad17a" : "#ffb454", this.sub.textContent = `OK ${this.ok} / NG ${this.ng}` + (this.na ? ` · NA ${this.na}` : "");
  }
}
class Kc extends HTMLElement {
  connectedCallback() {
    var e;
    this.body = ur(this, ((e = this.config) == null ? void 0 : e.title) || "Dispatch groups"), this.body.style.cssText = "display:flex;flex-direction:column;gap:10px;padding:10px;overflow:auto", this.peak = {}, this.rows = {};
  }
  feed(e) {
    const n = e.groups || [];
    if (!n.length) {
      this.body.textContent = "no dispatch groups (legacy single pool)", this.body.style.color = "#888";
      return;
    }
    for (const r of n) {
      const i = r.max_parallel || 1, s = r.running || 0;
      this.peak[r.name] = Math.max(this.peak[r.name] || 0, s);
      let a = this.rows[r.name];
      if (!a) {
        a = document.createElement("div"), a.style.cssText = "display:flex;flex-direction:column;gap:3px";
        const l = document.createElement("div");
        l.style.cssText = "display:flex;justify-content:space-between;font-size:12px";
        const o = document.createElement("span");
        o.style.fontWeight = "600";
        const c = document.createElement("span");
        c.style.color = "#888", l.append(o, c);
        const u = document.createElement("div");
        u.style.cssText = "display:flex;gap:3px;height:18px", a.append(l, u), this.body.appendChild(a), this.rows[r.name] = a = { row: a, name: o, meta: c, bar: u, cells: [] };
      }
      if (a.name.textContent = `${r.name}  ${s}/${i}`, a.name.style.color = s >= i ? "#3ad17a" : s > 0 ? "#9ad" : "#bbb", a.meta.textContent = `q ${r.queue_now ?? 0} · drop ${r.dropped ?? 0} · peak ${this.peak[r.name]}`, a.cells.length !== i) {
        a.bar.replaceChildren(), a.cells = [];
        for (let l = 0; l < i; l++) {
          const o = document.createElement("div");
          o.style.cssText = "flex:1 1 0;border-radius:3px;border:1px solid #333;min-width:6px", a.bar.appendChild(o), a.cells.push(o);
        }
      }
      a.cells.forEach((l, o) => {
        l.style.background = o < s ? "#3ad17a" : "#1a1a1a";
      });
    }
  }
}
const fa = {
  verdict: Xc,
  throughput: qc,
  yield: Gc,
  groups: Kc
};
for (const [t, e] of Object.entries(fa)) customElements.define(`xi-card-${t}`, e);
const ai = (t) => !!(t && t.card), Wt = (t) => !!(t && (t.dir === "row" || t.dir === "col") && Array.isArray(t.children) && t.children.length >= 1), st = (t) => !!(t && Array.isArray(t.tabs) && t.tabs.length >= 1 && t.tabs.every((e) => e && e.child)), Ln = () => ({ type: "verdict", bind: {}, config: { title: "(empty)" } });
function li(t) {
  const e = t.children.length;
  return (Array.isArray(t.weights) && t.weights.length === e ? t.weights.slice() : Array(e).fill(1)).map((r) => typeof r == "number" && r > 0 ? r : 1);
}
function Jc(t) {
  const e = li(t), n = e.reduce((r, i) => r + i, 0) || 1;
  return e.map((r) => r / n);
}
function da(t, e) {
  return st(t) ? t.tabs[e].child : t.children[e];
}
function Zc(t, e, n) {
  if (st(t)) {
    const i = t.tabs.slice();
    return i[e] = { ...i[e], child: n }, { ...t, tabs: i };
  }
  const r = t.children.slice();
  return r[e] = n, { ...t, children: r };
}
function Rr(t, e, n = []) {
  if (ai(t)) {
    e(t.card, n);
    return;
  }
  Wt(t) ? t.children.forEach((r, i) => Rr(r, e, [...n, i])) : st(t) && t.tabs.forEach((r, i) => Rr(r.child, e, [...n, i]));
}
function hu(t) {
  let e = 0;
  return Rr(t, () => e++), e;
}
function Qc(t, e) {
  let n = t;
  for (const r of e)
    if (Wt(n) || st(n)) n = da(n, r);
    else return;
  return n;
}
function He(t, e, n) {
  if (e.length === 0) return n(t);
  const [r, ...i] = e;
  return Zc(t, r, He(da(t, r), i, n));
}
function pu(t, e, n, r = Ln()) {
  return He(t, e, (i) => ({ dir: n === "col" ? "col" : "row", children: [i, { card: r }], weights: [1, 1] }));
}
function vu(t, e, n, r = Ln()) {
  if (n = n === "col" ? "col" : "row", e.length === 0) return { dir: n, children: [t, { card: r }], weights: [1, 1] };
  const i = e.slice(0, -1), s = e[e.length - 1], a = Qc(t, i);
  return Wt(a) && a.dir === n ? He(t, i, (l) => {
    const o = l.children.slice();
    o.splice(s + 1, 0, { card: r });
    const c = li(l);
    return c.splice(s + 1, 0, c[s]), { ...l, children: o, weights: c };
  }) : He(t, e, (l) => ({ dir: n, children: [l, { card: r }], weights: [1, 1] }));
}
function mu(t, e) {
  if (e.length === 0) return { card: Ln() };
  const n = e.slice(0, -1), r = e[e.length - 1];
  return He(t, n, (i) => {
    if (!Wt(i)) return i;
    const s = i.children.filter((l, o) => o !== r), a = li(i).filter((l, o) => o !== r);
    return s.length === 1 ? s[0] : { ...i, children: s, weights: a };
  });
}
function gu(t, e, n) {
  return He(t, e, () => ({ card: n }));
}
function bu(t, e, n) {
  return He(t, e, (r) => Wt(r) ? { ...r, weights: n.slice() } : r);
}
function xu(t, e) {
  return He(t, e, (n) => ({ tabs: [{ name: "Page 1", child: n }, { name: "Page 2", child: { card: Ln() } }], active: 0 }));
}
function yu(t, e, n, r = { card: Ln() }) {
  return He(t, e, (i) => st(i) ? { ...i, tabs: [...i.tabs, { name: n || `Page ${i.tabs.length + 1}`, child: r }], active: i.tabs.length } : i);
}
function _u(t, e, n) {
  return He(t, e, (r) => {
    if (!st(r)) return r;
    const i = r.tabs.filter((s, a) => a !== n);
    return i.length === 1 ? i[0].child : { ...r, tabs: i, active: Math.max(0, Math.min(r.active || 0, i.length - 1)) };
  });
}
function wu(t, e, n, r) {
  return He(t, e, (i) => st(i) ? { ...i, tabs: i.tabs.map((s, a) => a === n ? { ...s, name: r } : s) } : i);
}
function Eu(t, e, n) {
  return He(t, e, (r) => st(r) ? { ...r, active: n } : r);
}
function Fi(t, e = "root") {
  return ai(t) ? t.card.type ? [] : [`${e}: leaf has no card.type`] : Wt(t) ? t.children.flatMap((n, r) => Fi(n, `${e}.${r}`)) : st(t) ? t.tabs.flatMap((n, r) => Fi(n.child, `${e}.${n.name || r}`)) : [`${e}: node is not a leaf {card}, split {dir,children}, or tabs {tabs}`];
}
function ku(t, { client: e, dashboard: n, pollStatsMs: r = 200 }) {
  const i = t.ownerDocument || globalThis.document, s = globalThis.requestAnimationFrame || ((h) => setTimeout(h, 16)), a = { run_id: -1, compute_ms: null, status: null, result: null, groups: [] };
  let l = [], o = 0;
  function c() {
    o || (o = s(() => {
      o = 0;
      for (const h of l)
        try {
          h.feed(a);
        } catch {
        }
    }));
  }
  function u(h) {
    const b = fa[h.type], m = i.createElement(b ? `xi-card-${h.type}` : "div");
    return b || (m.textContent = `unknown card: ${h.type}`, m.style.cssText = "color:#f88;padding:8px"), m.binding = h.bind || {}, m.config = h.config || {}, m.style.minWidth = "0", m.style.minHeight = "0", m.style.overflow = "hidden", b && l.push(m), m;
  }
  function d(h) {
    let b = Math.min(h.active || 0, h.tabs.length - 1);
    const m = i.createElement("div");
    m.style.cssText = "display:flex;flex-direction:column;min-width:0;min-height:0;width:100%;height:100%";
    const p = i.createElement("div");
    p.style.cssText = "display:flex;gap:2px;flex:0 0 auto;align-items:center;padding:2px 2px 0;overflow:auto";
    const _ = i.createElement("div");
    _.style.cssText = "flex:1 1 0;min-width:0;min-height:0;position:relative";
    const w = [], E = [], M = () => {
      w.forEach((k, T) => {
        const C = T === b;
        k.style.background = C ? "#1e1e1e" : "#121212", k.style.color = C ? "#ddd" : "#888";
      }), E.forEach((k, T) => {
        k.style.display = T === b ? "" : "none";
      });
    };
    return h.tabs.forEach((k, T) => {
      const C = i.createElement("div");
      C.style.cssText = "padding:4px 11px;cursor:pointer;border-radius:6px 6px 0 0;border:1px solid #333;border-bottom:none;font:12px system-ui,sans-serif", C.textContent = k.name || `Page ${T + 1}`, C.onclick = () => {
        b = T, M();
      }, w.push(C), p.appendChild(C);
      const $ = f(k.child);
      $.style.cssText += ";position:absolute;inset:0;min-width:0;min-height:0;border:1px solid #333;border-radius:0 6px 6px 6px;overflow:hidden", E.push($), _.appendChild($);
    }), M(), m.append(p, _), m;
  }
  function f(h) {
    if (ai(h)) return u(h.card);
    if (st(h)) return d(h);
    if (!Wt(h)) {
      const _ = i.createElement("div");
      return _.textContent = "bad layout node", _.style.color = "#f88", _;
    }
    const b = h.dir === "col", m = i.createElement("div");
    m.style.cssText = `display:flex;flex-direction:${b ? "column" : "row"};min-width:0;min-height:0;width:100%;height:100%`;
    const p = Jc(h);
    return h.children.forEach((_, w) => {
      const E = f(_);
      E.style.flex = `${p[w]} 1 0`, E.style.minWidth = "0", E.style.minHeight = "0", m.appendChild(E);
    }), m;
  }
  function g() {
    l = [], t.replaceChildren(), t.style.cssText += ";display:flex;min-width:0;min-height:0";
    const h = n && n.layout;
    if (!h) return;
    const b = f(h);
    b.style.flex = "1 1 0", b.style.minWidth = "0", b.style.minHeight = "0", t.appendChild(b), c();
  }
  const x = [
    e.onEvent((h) => {
      h.name === "run_finished" && h.data ? (typeof h.data.run_id == "number" && (a.run_id = h.data.run_id), typeof h.data.inspect_compute_us == "number" ? a.compute_ms = h.data.inspect_compute_us / 1e3 : typeof h.data.ms == "number" && (a.compute_ms = h.data.ms), c()) : h.name === "run_result" && h.data ? (a.result = h.data, c()) : h.name === "status" && (a.status = h.data, c());
    })
  ], y = setInterval(() => {
    e.cmd("dispatch_stats").then((h) => {
      h && Array.isArray(h.groups) && (a.groups = h.groups, c());
    }).catch(() => {
    });
  }, r);
  return g(), {
    setDashboard(h) {
      n = h, g();
    },
    state: a,
    destroy() {
      x.forEach((h) => h()), clearInterval(y), t.replaceChildren();
    }
  };
}
const Cu = [
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
  cc as BUSY_CLOSE_CODE,
  fa as CARDS,
  Pi as COLORMAPS,
  pc as CONTROL_TAGS,
  la as RENDERERS,
  Or as TOOLS,
  Cu as XI_COMPONENTS,
  ru as XiClient,
  vu as addSibling,
  yu as addTab,
  hu as countLeaves,
  fc as createVsCodeApi,
  Rr as eachLeaf,
  Ln as emptyCard,
  Qc as getNode,
  Mc as heatmapRGBA,
  Ac as imageRGBA,
  vc as inferDescriptor,
  iu as installVsCodeShim,
  ai as isLeaf,
  Wt as isSplit,
  st as isTabs,
  Ci as makeTool,
  ku as mountDashboard,
  fu as mountLiveView,
  lu as mountPanel,
  ou as mountParamPanel,
  uu as mountPluginlets,
  au as mountSchema,
  aa as normalize,
  Lc as overlayOps,
  mc as paramsToControls,
  Hc as pickRenderer,
  Ic as profilePoints,
  sa as readScalars,
  du as registerTeachPanelDemo,
  nu as registerTool,
  dc as registerWidget,
  mu as removePane,
  _u as removeTab,
  wu as renameTab,
  cu as renderDescriptor,
  Eu as setActive,
  gu as setCard,
  bu as setWeights,
  pu as splitLeaf,
  Oc as tableRows,
  su as unregisterWidget,
  Fi as validate,
  Jc as weightsOf,
  xu as wrapInTabs
};
