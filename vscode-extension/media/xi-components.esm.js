var Ks = Object.defineProperty;
var Zr = (t) => {
  throw TypeError(t);
};
var Js = (t, e, n) => e in t ? Ks(t, e, { enumerable: !0, configurable: !0, writable: !0, value: n }) : t[e] = n;
var F = (t, e, n) => Js(t, typeof e != "symbol" ? e + "" : e, n), tr = (t, e, n) => e.has(t) || Zr("Cannot " + n);
var d = (t, e, n) => (tr(t, e, "read from private field"), n ? n.call(t) : e.get(t)), C = (t, e, n) => e.has(t) ? Zr("Cannot add the same private member more than once") : e instanceof WeakSet ? e.add(t) : e.set(t, n), $ = (t, e, n, r) => (tr(t, e, "write to private field"), r ? r.call(t, n) : e.set(t, n), n), O = (t, e, n) => (tr(t, e, "access private method"), n);
var Ci;
typeof window < "u" && ((Ci = window.__svelte ?? (window.__svelte = {})).v ?? (Ci.v = /* @__PURE__ */ new Set())).add("5");
const Zs = 1, Qs = 2, Mi = 4, ea = 8, ta = 16, na = 1, ra = 4, ia = 8, sa = 16, aa = 2, Ni = "[", Tr = "[!", Qr = "[?", Cr = "]", Xt = {}, X = Symbol("uninitialized"), oa = "http://www.w3.org/1999/xhtml", Ii = !1;
var Sr = Array.isArray, la = Array.prototype.indexOf, Pn = Array.prototype.includes, Gn = Array.from, jn = Object.keys, Bn = Object.defineProperty, Tt = Object.getOwnPropertyDescriptor, ua = Object.getOwnPropertyDescriptors, ca = Object.prototype, fa = Array.prototype, Oi = Object.getPrototypeOf, ei = Object.isExtensible;
const da = () => {
};
function ha(t) {
  for (var e = 0; e < t.length; e++)
    t[e]();
}
function Di() {
  var t, e, n = new Promise((r, i) => {
    t = r, e = i;
  });
  return { promise: n, resolve: t, reject: e };
}
const ne = 2, Gt = 4, Kn = 8, Ri = 1 << 24, Oe = 16, Re = 32, at = 64, lr = 128, Ee = 512, G = 1024, Z = 2048, Ue = 4096, ie = 8192, pe = 16384, Ot = 32768, ur = 1 << 25, Kt = 65536, Hn = 1 << 17, va = 1 << 18, Dt = 1 << 19, pa = 1 << 20, Ve = 1 << 25, Nt = 65536, Fn = 1 << 21, Ht = 1 << 22, ht = 1 << 23, Ct = Symbol("$state"), Li = Symbol("legacy props"), ma = Symbol(""), Nn = Symbol("attributes"), cr = Symbol("class"), ga = Symbol("style"), sn = Symbol("text"), Pi = Symbol("form reset"), Jn = new class extends Error {
  constructor() {
    super(...arguments);
    F(this, "name", "StaleReactionError");
    F(this, "message", "The reaction that called `getAbortSignal()` was re-run or destroyed");
  }
}();
var Si;
const ji = (
  // We gotta write it like this because after downleveling the pure comment may end up in the wrong location
  !!((Si = globalThis.document) != null && Si.contentType) && /* @__PURE__ */ globalThis.document.contentType.includes("xml")
), Ar = 3, wn = 8;
function _a() {
  throw new Error("https://svelte.dev/e/async_derived_orphan");
}
function ba(t, e, n) {
  throw new Error("https://svelte.dev/e/each_key_duplicate");
}
function xa(t) {
  throw new Error("https://svelte.dev/e/effect_in_teardown");
}
function ya() {
  throw new Error("https://svelte.dev/e/effect_in_unowned_derived");
}
function wa(t) {
  throw new Error("https://svelte.dev/e/effect_orphan");
}
function Ea() {
  throw new Error("https://svelte.dev/e/effect_update_depth_exceeded");
}
function ka() {
  throw new Error("https://svelte.dev/e/hydration_failed");
}
function $a(t) {
  throw new Error("https://svelte.dev/e/props_invalid_value");
}
function Ta() {
  throw new Error("https://svelte.dev/e/state_descriptors_fixed");
}
function Ca() {
  throw new Error("https://svelte.dev/e/state_prototype_fixed");
}
function Sa() {
  throw new Error("https://svelte.dev/e/state_unsafe_mutation");
}
function Aa() {
  throw new Error("https://svelte.dev/e/svelte_boundary_reset_onerror");
}
function Ma() {
  console.warn("https://svelte.dev/e/derived_inert");
}
function Zn(t) {
  console.warn("https://svelte.dev/e/hydration_mismatch");
}
function Na() {
  console.warn("https://svelte.dev/e/select_multiple_invalid_value");
}
function Ia() {
  console.warn("https://svelte.dev/e/svelte_boundary_reset_noop");
}
let R = !1;
function rt(t) {
  R = t;
}
let j;
function me(t) {
  if (t === null)
    throw Zn(), Xt;
  return j = t;
}
function En() {
  return me(/* @__PURE__ */ lt(j));
}
function V(t) {
  if (R) {
    if (/* @__PURE__ */ lt(j) !== null)
      throw Zn(), Xt;
    j = t;
  }
}
function Oa(t = 1) {
  if (R) {
    for (var e = t, n = j; e--; )
      n = /** @type {TemplateNode} */
      /* @__PURE__ */ lt(n);
    j = n;
  }
}
function Vn(t = !0) {
  for (var e = 0, n = j; ; ) {
    if (n.nodeType === wn) {
      var r = (
        /** @type {Comment} */
        n.data
      );
      if (r === Cr) {
        if (e === 0) return n;
        e -= 1;
      } else (r === Ni || r === Tr || // "[1", "[2", etc. for if blocks
      r[0] === "[" && !isNaN(Number(r.slice(1)))) && (e += 1);
    }
    var i = (
      /** @type {TemplateNode} */
      /* @__PURE__ */ lt(n)
    );
    t && n.remove(), n = i;
  }
}
function Bi(t) {
  if (!t || t.nodeType !== wn)
    throw Zn(), Xt;
  return (
    /** @type {Comment} */
    t.data
  );
}
function Hi(t) {
  return t === this.v;
}
function Da(t, e) {
  return t != t ? e == e : t !== e || t !== null && typeof t == "object" || typeof t == "function";
}
function Fi(t) {
  return !Da(t, this.v);
}
let Ra = !1, se = null;
function Jt(t) {
  se = t;
}
function Ce(t, e = !1, n) {
  se = {
    p: se,
    i: !1,
    c: null,
    e: null,
    s: t,
    x: null,
    r: (
      /** @type {Effect} */
      S
    ),
    l: null
  };
}
function Se(t) {
  var e = (
    /** @type {ComponentContext} */
    se
  ), n = e.e;
  if (n !== null) {
    e.e = null;
    for (var r of n)
      vs(r);
  }
  return t !== void 0 && (e.x = t), e.i = !0, se = e.p, t ?? /** @type {T} */
  {};
}
function Vi() {
  return !0;
}
let gt = [];
function zi() {
  var t = gt;
  gt = [], ha(t);
}
function st(t) {
  if (gt.length === 0 && !fn) {
    var e = gt;
    queueMicrotask(() => {
      e === gt && zi();
    });
  }
  gt.push(t);
}
function La() {
  for (; gt.length > 0; )
    zi();
}
function Wi(t) {
  var e = S;
  if (e === null)
    return A.f |= ht, t;
  if ((e.f & Ot) === 0 && (e.f & Gt) === 0)
    throw t;
  dt(t, e);
}
function dt(t, e) {
  if (!(e !== null && (e.f & pe) !== 0)) {
    for (; e !== null; ) {
      if ((e.f & lr) !== 0) {
        if ((e.f & Ot) === 0)
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
const Pa = -7169;
function U(t, e) {
  t.f = t.f & Pa | e;
}
function Mr(t) {
  (t.f & Ee) !== 0 || t.deps === null ? U(t, G) : U(t, Ue);
}
function qi(t) {
  if (t !== null)
    for (const e of t)
      (e.f & ne) === 0 || (e.f & Nt) === 0 || (e.f ^= Nt, qi(
        /** @type {Derived} */
        e.deps
      ));
}
function Ui(t, e, n) {
  (t.f & Z) !== 0 ? e.add(t) : (t.f & Ue) !== 0 && n.add(t), qi(t.deps), U(t, G);
}
let Sn = !1;
function ja(t) {
  var e = Sn;
  try {
    return Sn = !1, [t(), Sn];
  } finally {
    Sn = e;
  }
}
function Ba(t) {
  let e = 0, n = It(0), r;
  return () => {
    Pr() && (N(n), Hr(() => (e === 0 && (r = Wr(() => t(() => dn(n)))), e += 1, () => {
      st(() => {
        e -= 1, e === 0 && (r == null || r(), r = void 0, dn(n));
      });
    })));
  };
}
var Ha = Kt | Dt;
function Fa(t, e, n, r) {
  new Va(t, e, n, r);
}
var de, mn, be, yt, le, xe, re, he, Ze, wt, ct, Ft, gn, _n, Qe, Un, H, Yi, Xi, Gi, fr, In, On, dr, hr;
class Va {
  /**
   * @param {TemplateNode} node
   * @param {BoundaryProps} props
   * @param {((anchor: Node) => void)} children
   * @param {((error: unknown) => unknown) | undefined} [transform_error]
   */
  constructor(e, n, r, i) {
    C(this, H);
    /** @type {Boundary | null} */
    F(this, "parent");
    F(this, "is_pending", !1);
    /**
     * API-level transformError transform function. Transforms errors before they reach the `failed` snippet.
     * Inherited from parent boundary, or defaults to identity.
     * @type {(error: unknown) => unknown}
     */
    F(this, "transform_error");
    /** @type {TemplateNode} */
    C(this, de);
    /** @type {TemplateNode | null} */
    C(this, mn, R ? j : null);
    /** @type {BoundaryProps} */
    C(this, be);
    /** @type {((anchor: Node) => void)} */
    C(this, yt);
    /** @type {Effect} */
    C(this, le);
    /** @type {Effect | null} */
    C(this, xe, null);
    /** @type {Effect | null} */
    C(this, re, null);
    /** @type {Effect | null} */
    C(this, he, null);
    /** @type {DocumentFragment | null} */
    C(this, Ze, null);
    C(this, wt, 0);
    C(this, ct, 0);
    C(this, Ft, !1);
    /** @type {Set<Effect>} */
    C(this, gn, /* @__PURE__ */ new Set());
    /** @type {Set<Effect>} */
    C(this, _n, /* @__PURE__ */ new Set());
    /**
     * A source containing the number of pending async deriveds/expressions.
     * Only created if `$effect.pending()` is used inside the boundary,
     * otherwise updating the source results in needless `Batch.ensure()`
     * calls followed by no-op flushes
     * @type {Source<number> | null}
     */
    C(this, Qe, null);
    C(this, Un, Ba(() => ($(this, Qe, It(d(this, wt))), () => {
      $(this, Qe, null);
    })));
    var s;
    $(this, de, e), $(this, be, n), $(this, yt, (a) => {
      var o = (
        /** @type {Effect} */
        S
      );
      o.b = this, o.f |= lr, r(a);
    }), this.parent = /** @type {Effect} */
    S.b, this.transform_error = i ?? ((s = this.parent) == null ? void 0 : s.transform_error) ?? ((a) => a), $(this, le, Fr(() => {
      if (R) {
        const a = (
          /** @type {Comment} */
          d(this, mn)
        );
        En();
        const o = a.data === Tr;
        if (a.data.startsWith(Qr)) {
          const u = JSON.parse(a.data.slice(Qr.length));
          O(this, H, Xi).call(this, u);
        } else o ? O(this, H, Gi).call(this) : O(this, H, Yi).call(this);
      } else
        O(this, H, fr).call(this);
    }, Ha)), R && $(this, de, j);
  }
  /**
   * Defer an effect inside a pending boundary until the boundary resolves
   * @param {Effect} effect
   */
  defer_effect(e) {
    Ui(e, d(this, gn), d(this, _n));
  }
  /**
   * Returns `false` if the effect exists inside a boundary whose pending snippet is shown
   * @returns {boolean}
   */
  is_rendered() {
    return !this.is_pending && (!this.parent || this.parent.is_rendered());
  }
  has_pending_snippet() {
    return !!d(this, be).pending;
  }
  /**
   * Update the source that powers `$effect.pending()` inside this boundary,
   * and controls when the current `pending` snippet (if any) is removed.
   * Do not call from inside the class
   * @param {1 | -1} d
   * @param {Batch} batch
   */
  update_pending_count(e, n) {
    O(this, H, dr).call(this, e, n), $(this, wt, d(this, wt) + e), !(!d(this, Qe) || d(this, Ft)) && ($(this, Ft, !0), st(() => {
      $(this, Ft, !1), d(this, Qe) && Zt(d(this, Qe), d(this, wt));
    }));
  }
  get_effect_pending() {
    return d(this, Un).call(this), N(
      /** @type {Source<number>} */
      d(this, Qe)
    );
  }
  /** @param {unknown} error */
  error(e) {
    if (!d(this, be).onerror && !d(this, be).failed)
      throw e;
    T != null && T.is_fork ? (d(this, xe) && T.skip_effect(d(this, xe)), d(this, re) && T.skip_effect(d(this, re)), d(this, he) && T.skip_effect(d(this, he)), T.oncommit(() => {
      O(this, H, hr).call(this, e);
    })) : O(this, H, hr).call(this, e);
  }
}
de = new WeakMap(), mn = new WeakMap(), be = new WeakMap(), yt = new WeakMap(), le = new WeakMap(), xe = new WeakMap(), re = new WeakMap(), he = new WeakMap(), Ze = new WeakMap(), wt = new WeakMap(), ct = new WeakMap(), Ft = new WeakMap(), gn = new WeakMap(), _n = new WeakMap(), Qe = new WeakMap(), Un = new WeakMap(), H = new WeakSet(), Yi = function() {
  try {
    $(this, xe, we(() => d(this, yt).call(this, d(this, de))));
  } catch (e) {
    this.error(e);
  }
}, /**
 * @param {unknown} error The deserialized error from the server's hydration comment
 */
Xi = function(e) {
  const n = d(this, be).failed;
  n && $(this, he, we(() => {
    n(
      d(this, de),
      () => e,
      () => () => {
      }
    );
  }));
}, Gi = function() {
  const e = d(this, be).pending;
  e && (this.is_pending = !0, $(this, re, we(() => e(d(this, de)))), st(() => {
    var n = $(this, Ze, document.createDocumentFragment()), r = ze();
    n.append(r), $(this, xe, O(this, H, On).call(this, () => we(() => d(this, yt).call(this, r)))), d(this, ct) === 0 && (d(this, de).before(n), $(this, Ze, null), At(
      /** @type {Effect} */
      d(this, re),
      () => {
        $(this, re, null);
      }
    ), O(this, H, In).call(
      this,
      /** @type {Batch} */
      T
    ));
  }));
}, fr = function() {
  try {
    if (this.is_pending = this.has_pending_snippet(), $(this, ct, 0), $(this, wt, 0), $(this, xe, we(() => {
      d(this, yt).call(this, d(this, de));
    })), d(this, ct) > 0) {
      var e = $(this, Ze, document.createDocumentFragment());
      zr(d(this, xe), e);
      const n = (
        /** @type {(anchor: Node) => void} */
        d(this, be).pending
      );
      $(this, re, we(() => n(d(this, de))));
    } else
      O(this, H, In).call(
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
In = function(e) {
  this.is_pending = !1, e.transfer_effects(d(this, gn), d(this, _n));
}, /**
 * @template T
 * @param {() => T} fn
 */
On = function(e) {
  var n = S, r = A, i = se;
  Ye(d(this, le)), ke(d(this, le)), Jt(d(this, le).ctx);
  try {
    return vt.ensure(), e();
  } catch (s) {
    return Wi(s), null;
  } finally {
    Ye(n), ke(r), Jt(i);
  }
}, /**
 * Updates the pending count associated with the currently visible pending snippet,
 * if any, such that we can replace the snippet with content once work is done
 * @param {1 | -1} d
 * @param {Batch} batch
 */
dr = function(e, n) {
  var r;
  if (!this.has_pending_snippet()) {
    this.parent && O(r = this.parent, H, dr).call(r, e, n);
    return;
  }
  $(this, ct, d(this, ct) + e), d(this, ct) === 0 && (O(this, H, In).call(this, n), d(this, re) && At(d(this, re), () => {
    $(this, re, null);
  }), d(this, Ze) && (d(this, de).before(d(this, Ze)), $(this, Ze, null)));
}, /**
 * @param {unknown} error
 */
hr = function(e) {
  d(this, xe) && (ae(d(this, xe)), $(this, xe, null)), d(this, re) && (ae(d(this, re)), $(this, re, null)), d(this, he) && (ae(d(this, he)), $(this, he, null)), R && (me(
    /** @type {TemplateNode} */
    d(this, mn)
  ), Oa(), me(Vn()));
  var n = d(this, be).onerror;
  let r = d(this, be).failed;
  var i = !1, s = !1;
  const a = () => {
    if (i) {
      Ia();
      return;
    }
    i = !0, s && Aa(), d(this, he) !== null && At(d(this, he), () => {
      $(this, he, null);
    }), O(this, H, On).call(this, () => {
      O(this, H, fr).call(this);
    });
  }, o = (l) => {
    try {
      s = !0, n == null || n(l, a), s = !1;
    } catch (u) {
      dt(u, d(this, le) && d(this, le).parent);
    }
    r && $(this, he, O(this, H, On).call(this, () => {
      try {
        return we(() => {
          var u = (
            /** @type {Effect} */
            S
          );
          u.b = this, u.f |= lr, r(
            d(this, de),
            () => l,
            () => a
          );
        });
      } catch (u) {
        return dt(
          u,
          /** @type {Effect} */
          d(this, le).parent
        ), null;
      }
    }));
  };
  st(() => {
    var l;
    try {
      l = this.transform_error(e);
    } catch (u) {
      dt(u, d(this, le) && d(this, le).parent);
      return;
    }
    l !== null && typeof l == "object" && typeof /** @type {any} */
    l.then == "function" ? l.then(
      o,
      /** @param {unknown} e */
      (u) => dt(u, d(this, le) && d(this, le).parent)
    ) : o(l);
  });
};
function za(t, e, n, r) {
  const i = hn;
  var s = t.filter((p) => !p.settled), a = e.map(i);
  if (n.length === 0 && s.length === 0) {
    r(a);
    return;
  }
  var o = (
    /** @type {Effect} */
    S
  ), l = Wa(), u = s.length === 1 ? s[0].promise : s.length > 1 ? Promise.all(s.map((p) => p.promise)) : null;
  function c(p) {
    if ((o.f & pe) === 0) {
      l();
      try {
        r([...a, ...p]);
      } catch (m) {
        dt(m, o);
      }
      zn();
    }
  }
  var h = Ki();
  if (n.length === 0) {
    u.then(() => c([])).finally(h);
    return;
  }
  function f() {
    Promise.all(n.map((p) => /* @__PURE__ */ qa(p))).then(c).catch((p) => dt(p, o)).finally(h);
  }
  u ? u.then(() => {
    l(), f(), zn();
  }) : f();
}
function Wa() {
  var t = (
    /** @type {Effect} */
    S
  ), e = A, n = se, r = (
    /** @type {Batch} */
    T
  );
  return function(s = !0) {
    Ye(t), ke(e), Jt(n), s && (t.f & pe) === 0 && (r == null || r.activate(), r == null || r.apply());
  };
}
function zn(t = !0) {
  Ye(null), ke(null), Jt(null), t && (T == null || T.deactivate());
}
function Ki() {
  var t = (
    /** @type {Effect} */
    S
  ), e = t.b, n = (
    /** @type {Batch} */
    T
  ), r = !!(e != null && e.is_rendered());
  return e == null || e.update_pending_count(1, n), n.increment(r, t), () => {
    e == null || e.update_pending_count(-1, n), n.decrement(r, t);
  };
}
// @__NO_SIDE_EFFECTS__
function hn(t) {
  var e = ne | Z;
  return S !== null && (S.f |= Dt), {
    ctx: se,
    deps: null,
    effects: null,
    equals: Hi,
    f: e,
    fn: t,
    reactions: null,
    rv: 0,
    v: (
      /** @type {V} */
      X
    ),
    wv: 0,
    parent: S,
    ac: null
  };
}
const an = Symbol("obsolete");
// @__NO_SIDE_EFFECTS__
function qa(t, e, n) {
  let r = (
    /** @type {Effect | null} */
    S
  );
  r === null && _a();
  var i = (
    /** @type {Promise<V>} */
    /** @type {unknown} */
    void 0
  ), s = It(
    /** @type {V} */
    X
  ), a = !A, o = /* @__PURE__ */ new Set();
  return io(() => {
    var p, m;
    var l = (
      /** @type {Effect} */
      S
    ), u = Di();
    i = u.promise;
    try {
      Promise.resolve(t()).then(u.resolve, (g) => {
        g !== Jn && u.reject(g);
      }).finally(zn);
    } catch (g) {
      u.reject(g), zn();
    }
    var c = (
      /** @type {Batch} */
      T
    );
    if (a) {
      if ((l.f & Ot) !== 0)
        var h = Ki();
      if (
        // boundary can be null if the async derived is inside an $effect.root not connected to the component render tree
        (p = r.b) != null && p.is_rendered()
      )
        (m = c.async_deriveds.get(l)) == null || m.reject(an);
      else
        for (const g of o.values())
          g.reject(an);
      o.add(u), c.async_deriveds.set(l, u);
    }
    const f = (g, v = void 0) => {
      h == null || h(), o.delete(u), v !== an && (c.activate(), v ? (s.f |= ht, Zt(s, v)) : ((s.f & ht) !== 0 && (s.f ^= ht), Zt(s, g)), c.deactivate());
    };
    u.promise.then(f, (g) => f(null, g || "unknown"));
  }), jr(() => {
    for (const l of o)
      l.reject(an);
  }), new Promise((l) => {
    function u(c) {
      function h() {
        c === i ? l(s) : u(i);
      }
      c.then(h, h);
    }
    u(i);
  });
}
// @__NO_SIDE_EFFECTS__
function Nr(t) {
  const e = /* @__PURE__ */ hn(t);
  return xs(e), e;
}
// @__NO_SIDE_EFFECTS__
function Ji(t) {
  const e = /* @__PURE__ */ hn(t);
  return e.equals = Fi, e;
}
function Ua(t) {
  var e = t.effects;
  if (e !== null) {
    t.effects = null;
    for (var n = 0; n < e.length; n += 1)
      ae(
        /** @type {Effect} */
        e[n]
      );
  }
}
function Ir(t) {
  var e, n = S, r = t.parent;
  if (!ot && r !== null && t.v !== X && // if it was never evaluated before, it's guaranteed to fail downstream, so we try to execute instead
  (r.f & (pe | ie)) !== 0)
    return Ma(), t.v;
  Ye(r);
  try {
    t.f &= ~Nt, Ua(t), e = ks(t);
  } finally {
    Ye(n);
  }
  return e;
}
function Zi(t) {
  var e = Ir(t);
  if (!t.equals(e) && (t.wv = ws(), (!(T != null && T.is_fork) || t.deps === null) && (T !== null ? (T.capture(t, e, !0), cn == null || cn.capture(t, e, !0)) : t.v = e, t.deps === null))) {
    U(t, G);
    return;
  }
  ot || (ee !== null ? (Pr() || T != null && T.is_fork) && ee.set(t, e) : Mr(t));
}
function Ya(t) {
  var e, n;
  if (t.effects !== null)
    for (const r of t.effects)
      (r.teardown || r.ac) && ((e = r.teardown) == null || e.call(r), (n = r.ac) == null || n.abort(Jn), r.fn !== null && (r.teardown = da), r.ac = null, pn(r, 0), Vr(r));
}
function Qi(t) {
  if (t.effects !== null)
    for (const e of t.effects)
      e.teardown && e.fn !== null && Qt(e);
}
let nr = null, Pt = null, T = null, cn = null, ee = null, vr = null, fn = !1, rr = !1, Bt = null, Dn = null;
var ti = 0;
let Xa = 1;
var Vt, ft, Et, zt, Wt, qt, et, Ut, ue, bn, tt, Me, He, Yt, kt, L, pr, on, mr, es, ts, jt, Ga, ln;
const Yn = class Yn {
  constructor() {
    C(this, L);
    F(this, "id", Xa++);
    /** True as soon as `#process` was called */
    C(this, Vt, !1);
    F(this, "linked", !0);
    /** @type {Batch | null} */
    C(this, ft, null);
    /** @type {Batch | null} */
    C(this, Et, null);
    /** @type {Map<Effect, ReturnType<typeof deferred<any>>>} */
    F(this, "async_deriveds", /* @__PURE__ */ new Map());
    /**
     * The current values of any signals that are updated in this batch.
     * Tuple format: [value, is_derived] (note: is_derived is false for deriveds, too, if they were overridden via assignment)
     * They keys of this map are identical to `this.#previous`
     * @type {Map<Value, [any, boolean]>}
     */
    F(this, "current", /* @__PURE__ */ new Map());
    /**
     * The values of any signals (sources and deriveds) that are updated in this batch _before_ those updates took place.
     * They keys of this map are identical to `this.#current`
     * @type {Map<Value, any>}
     */
    F(this, "previous", /* @__PURE__ */ new Map());
    /**
     * When the batch is committed (and the DOM is updated), we need to remove old branches
     * and append new ones by calling the functions added inside (if/each/key/etc) blocks
     * @type {Set<(batch: Batch) => void>}
     */
    C(this, zt, /* @__PURE__ */ new Set());
    /**
     * If a fork is discarded, we need to destroy any effects that are no longer needed
     * @type {Set<(batch: Batch) => void>}
     */
    C(this, Wt, /* @__PURE__ */ new Set());
    /**
     * The number of async effects that are currently in flight
     */
    C(this, qt, 0);
    /**
     * Async effects that are currently in flight, _not_ inside a pending boundary
     * @type {Map<Effect, number>}
     */
    C(this, et, /* @__PURE__ */ new Map());
    /**
     * A deferred that resolves when the batch is committed, used with `settled()`
     * TODO replace with Promise.withResolvers once supported widely enough
     * @type {{ promise: Promise<void>, resolve: (value?: any) => void, reject: (reason: unknown) => void } | null}
     */
    C(this, Ut, null);
    /**
     * The root effects that need to be flushed
     * @type {Effect[]}
     */
    C(this, ue, []);
    /**
     * Effects created while this batch was active.
     * @type {Effect[]}
     */
    C(this, bn, []);
    /**
     * Deferred effects (which run after async work has completed) that are DIRTY
     * @type {Set<Effect>}
     */
    C(this, tt, /* @__PURE__ */ new Set());
    /**
     * Deferred effects that are MAYBE_DIRTY
     * @type {Set<Effect>}
     */
    C(this, Me, /* @__PURE__ */ new Set());
    /**
     * A map of branches that still exist, but will be destroyed when this batch
     * is committed — we skip over these during `process`.
     * The value contains child effects that were dirty/maybe_dirty before being reset,
     * so they can be rescheduled if the branch survives.
     * @type {Map<Effect, { d: Effect[], m: Effect[] }>}
     */
    C(this, He, /* @__PURE__ */ new Map());
    /**
     * Inverse of #skipped_branches which we need to tell prior batches to unskip them when committing
     * @type {Set<Effect>}
     */
    C(this, Yt, /* @__PURE__ */ new Set());
    F(this, "is_fork", !1);
    C(this, kt, !1);
    Pt === null ? nr = Pt = this : ($(Pt, Et, this), $(this, ft, Pt)), Pt = this;
  }
  /**
   * Add an effect to the #skipped_branches map and reset its children
   * @param {Effect} effect
   */
  skip_effect(e) {
    d(this, He).has(e) || d(this, He).set(e, { d: [], m: [] }), d(this, Yt).delete(e);
  }
  /**
   * Remove an effect from the #skipped_branches map and reschedule
   * any tracked dirty/maybe_dirty child effects
   * @param {Effect} effect
   * @param {(e: Effect) => void} callback
   */
  unskip_effect(e, n = (r) => this.schedule(r)) {
    var r = d(this, He).get(e);
    if (r) {
      d(this, He).delete(e);
      for (var i of r.d)
        U(i, Z), n(i);
      for (i of r.m)
        U(i, Ue), n(i);
    }
    d(this, Yt).add(e);
  }
  /**
   * Associate a change to a given source with the current
   * batch, noting its previous and current values
   * @param {Value} source
   * @param {any} value
   * @param {boolean} [is_derived]
   */
  capture(e, n, r = !1) {
    e.v !== X && !this.previous.has(e) && this.previous.set(e, e.v), (e.f & ht) === 0 && (this.current.set(e, [n, r]), ee == null || ee.set(e, n)), this.is_fork || (e.v = n);
  }
  activate() {
    T = this;
  }
  deactivate() {
    T = null, ee = null;
  }
  flush() {
    try {
      rr = !0, T = this, O(this, L, on).call(this);
    } finally {
      ti = 0, vr = null, Bt = null, Dn = null, rr = !1, T = null, ee = null, St.clear();
    }
  }
  discard() {
    var e;
    for (const n of d(this, Wt)) n(this);
    d(this, Wt).clear();
    for (const n of this.async_deriveds.values())
      n.reject(an);
    O(this, L, ln).call(this), (e = d(this, Ut)) == null || e.resolve();
  }
  /**
   * @param {Effect} effect
   */
  register_created_effect(e) {
    d(this, bn).push(e);
  }
  /**
   * @param {boolean} blocking
   * @param {Effect} effect
   */
  increment(e, n) {
    if ($(this, qt, d(this, qt) + 1), e) {
      let r = d(this, et).get(n) ?? 0;
      d(this, et).set(n, r + 1);
    }
  }
  /**
   * @param {boolean} blocking
   * @param {Effect} effect
   */
  decrement(e, n) {
    if ($(this, qt, d(this, qt) - 1), e) {
      let r = d(this, et).get(n) ?? 0;
      r === 1 ? d(this, et).delete(n) : d(this, et).set(n, r - 1);
    }
    d(this, kt) || ($(this, kt, !0), st(() => {
      $(this, kt, !1), this.linked && this.flush();
    }));
  }
  /**
   * @param {Set<Effect>} dirty_effects
   * @param {Set<Effect>} maybe_dirty_effects
   */
  transfer_effects(e, n) {
    for (const r of e)
      d(this, tt).add(r);
    for (const r of n)
      d(this, Me).add(r);
    e.clear(), n.clear();
  }
  /** @param {(batch: Batch) => void} fn */
  oncommit(e) {
    d(this, zt).add(e);
  }
  /** @param {(batch: Batch) => void} fn */
  ondiscard(e) {
    d(this, Wt).add(e);
  }
  settled() {
    return (d(this, Ut) ?? $(this, Ut, Di())).promise;
  }
  static ensure() {
    if (T === null) {
      const e = T = new Yn();
      !rr && !fn && st(() => {
        d(e, Vt) || e.flush();
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
  schedule(e) {
    var i;
    if (vr = e, (i = e.b) != null && i.is_pending && (e.f & (Gt | Kn | Ri)) !== 0 && (e.f & Ot) === 0) {
      e.b.defer_effect(e);
      return;
    }
    for (var n = e; n.parent !== null; ) {
      n = n.parent;
      var r = n.f;
      if (Bt !== null && n === S && (A === null || (A.f & ne) === 0))
        return;
      if ((r & (at | Re)) !== 0) {
        if ((r & G) === 0)
          return;
        n.f ^= G;
      }
    }
    d(this, ue).push(n);
  }
};
Vt = new WeakMap(), ft = new WeakMap(), Et = new WeakMap(), zt = new WeakMap(), Wt = new WeakMap(), qt = new WeakMap(), et = new WeakMap(), Ut = new WeakMap(), ue = new WeakMap(), bn = new WeakMap(), tt = new WeakMap(), Me = new WeakMap(), He = new WeakMap(), Yt = new WeakMap(), kt = new WeakMap(), L = new WeakSet(), pr = function() {
  if (this.is_fork) return !0;
  for (const r of d(this, et).keys()) {
    for (var e = r, n = !1; e.parent !== null; ) {
      if (d(this, He).has(e)) {
        n = !0;
        break;
      }
      e = e.parent;
    }
    if (!n)
      return !0;
  }
  return !1;
}, on = function() {
  var l, u, c, h;
  $(this, Vt, !0), ti++ > 1e3 && (O(this, L, ln).call(this), Ka());
  for (const f of d(this, tt))
    d(this, Me).delete(f), U(f, Z), this.schedule(f);
  for (const f of d(this, Me))
    U(f, Ue), this.schedule(f);
  const e = d(this, ue);
  $(this, ue, []), this.apply();
  var n = Bt = [], r = [], i = Dn = [];
  for (const f of e)
    try {
      O(this, L, mr).call(this, f, n, r);
    } catch (p) {
      throw is(f), O(this, L, pr).call(this) || this.discard(), p;
    }
  if (T = null, i.length > 0) {
    var s = Yn.ensure();
    for (const f of i)
      s.schedule(f);
  }
  if (Bt = null, Dn = null, O(this, L, pr).call(this)) {
    O(this, L, jt).call(this, r), O(this, L, jt).call(this, n);
    for (const [f, p] of d(this, He))
      rs(f, p);
    i.length > 0 && /** @type {unknown} */
    O(l = T, L, on).call(l);
    return;
  }
  const a = O(this, L, es).call(this);
  if (a) {
    O(this, L, jt).call(this, r), O(this, L, jt).call(this, n), O(u = a, L, ts).call(u, this);
    return;
  }
  d(this, tt).clear(), d(this, Me).clear();
  for (const f of d(this, zt)) f(this);
  d(this, zt).clear(), cn = this, ni(r), ni(n), cn = null, (c = d(this, Ut)) == null || c.resolve();
  var o = (
    /** @type {Batch | null} */
    /** @type {unknown} */
    T
  );
  if (d(this, qt) === 0 && (d(this, ue).length === 0 || o !== null) && O(this, L, ln).call(this), d(this, ue).length > 0)
    if (o !== null) {
      const f = o;
      d(f, ue).push(...d(this, ue).filter((p) => !d(f, ue).includes(p)));
    } else
      o = this;
  o !== null && O(h = o, L, on).call(h);
}, /**
 * Traverse the effect tree, executing effects or stashing
 * them for later execution as appropriate
 * @param {Effect} root
 * @param {Effect[]} effects
 * @param {Effect[]} render_effects
 */
mr = function(e, n, r) {
  e.f ^= G;
  for (var i = e.first; i !== null; ) {
    var s = i.f, a = (s & (Re | at)) !== 0, o = a && (s & G) !== 0, l = o || (s & ie) !== 0 || d(this, He).has(i);
    if (!l && i.fn !== null) {
      a ? i.f ^= G : (s & Gt) !== 0 ? n.push(i) : kn(i) && ((s & Oe) !== 0 && d(this, Me).add(i), Qt(i));
      var u = i.first;
      if (u !== null) {
        i = u;
        continue;
      }
    }
    for (; i !== null; ) {
      var c = i.next;
      if (c !== null) {
        i = c;
        break;
      }
      i = i.parent;
    }
  }
}, es = function() {
  for (var e = d(this, ft); e !== null; ) {
    if (!e.is_fork) {
      for (const [n, [, r]] of this.current)
        if (e.current.has(n) && !r)
          return e;
    }
    e = d(e, ft);
  }
  return null;
}, /**
 * @param {Batch} batch
 */
ts = function(e) {
  var r;
  for (const [i, s] of e.current)
    !this.previous.has(i) && e.previous.has(i) && this.previous.set(i, e.previous.get(i)), this.current.set(i, s);
  for (const [i, s] of e.async_deriveds) {
    const a = this.async_deriveds.get(i);
    a && s.promise.then(a.resolve).catch(a.reject);
  }
  e.async_deriveds.clear(), this.transfer_effects(d(e, tt), d(e, Me));
  const n = (i) => {
    var s = i.reactions;
    if (s !== null)
      for (const l of s) {
        var a = l.f;
        if ((a & ne) !== 0)
          n(
            /** @type {Derived} */
            l
          );
        else {
          var o = (
            /** @type {Effect} */
            l
          );
          a & (Ht | Oe) && !this.async_deriveds.has(o) && (d(this, Me).delete(o), U(o, Z), this.schedule(o));
        }
      }
  };
  for (const i of this.current.keys())
    n(i);
  this.oncommit(() => e.discard()), O(r = e, L, ln).call(r), T = this, O(this, L, on).call(this);
}, /**
 * @param {Effect[]} effects
 */
jt = function(e) {
  for (var n = 0; n < e.length; n += 1)
    Ui(e[n], d(this, tt), d(this, Me));
}, Ga = function() {
  var h;
  for (let f = nr; f !== null; f = d(f, Et)) {
    var e = f.id < this.id, n = [];
    for (const [p, [m, g]] of this.current) {
      if (f.current.has(p)) {
        var r = (
          /** @type {[any, boolean]} */
          f.current.get(p)[0]
        );
        if (e && m !== r)
          f.current.set(p, [m, g]);
        else
          continue;
      }
      n.push(p);
    }
    if (e)
      for (const [p, m] of this.async_deriveds) {
        const g = f.async_deriveds.get(p);
        g && m.promise.then(g.resolve).catch(g.reject);
      }
    var i = [...f.current.keys()].filter(
      (p) => !/** @type {[any, boolean]} */
      f.current.get(p)[1]
    );
    if (!(!d(f, Vt) || i.length === 0)) {
      var s = i.filter((p) => !this.current.has(p));
      if (s.length === 0)
        e && f.discard();
      else if (n.length > 0) {
        if (e)
          for (const p of d(this, Yt))
            f.unskip_effect(p, (m) => {
              var g;
              (m.f & (Oe | Ht)) !== 0 ? f.schedule(m) : O(g = f, L, jt).call(g, [m]);
            });
        f.activate();
        var a = /* @__PURE__ */ new Set(), o = /* @__PURE__ */ new Map();
        for (var l of n)
          ns(l, s, a, o);
        o = /* @__PURE__ */ new Map();
        var u = [...f.current].filter(([p, m]) => {
          const g = this.current.get(p);
          return g ? g[0] !== m[0] || g[1] !== m[1] : !0;
        }).map(([p]) => p);
        if (u.length > 0)
          for (const p of d(this, bn))
            (p.f & (pe | ie | Hn)) === 0 && Or(p, u, o) && ((p.f & (Ht | Oe)) !== 0 ? (U(p, Z), f.schedule(p)) : d(f, tt).add(p));
        if (d(f, ue).length > 0 && !d(f, kt)) {
          f.apply();
          for (var c of d(f, ue))
            O(h = f, L, mr).call(h, c, [], []);
          $(f, ue, []);
        }
        f.deactivate();
      }
    }
  }
}, ln = function() {
  if (this.linked) {
    var e = d(this, ft), n = d(this, Et);
    e === null ? nr = n : $(e, Et, n), n === null ? Pt = e : $(n, ft, e), this.linked = !1;
  }
};
let vt = Yn;
function M(t) {
  var e = fn;
  fn = !0;
  try {
    for (var n; ; ) {
      if (La(), T === null)
        return (
          /** @type {T} */
          n
        );
      T.flush();
    }
  } finally {
    fn = e;
  }
}
function Ka() {
  try {
    Ea();
  } catch (t) {
    dt(t, vr);
  }
}
let Ae = null;
function ni(t) {
  var e = t.length;
  if (e !== 0) {
    for (var n = 0; n < e; ) {
      var r = t[n++];
      if ((r.f & (pe | ie)) === 0 && kn(r) && (Ae = /* @__PURE__ */ new Set(), Qt(r), r.deps === null && r.first === null && r.nodes === null && r.teardown === null && r.ac === null && gs(r), (Ae == null ? void 0 : Ae.size) > 0)) {
        St.clear();
        for (const i of Ae) {
          if ((i.f & (pe | ie)) !== 0) continue;
          const s = [i];
          let a = i.parent;
          for (; a !== null; )
            Ae.has(a) && (Ae.delete(a), s.push(a)), a = a.parent;
          for (let o = s.length - 1; o >= 0; o--) {
            const l = s[o];
            (l.f & (pe | ie)) === 0 && Qt(l);
          }
        }
        Ae.clear();
      }
    }
    Ae = null;
  }
}
function ns(t, e, n, r) {
  if (!n.has(t) && (n.add(t), t.reactions !== null))
    for (const i of t.reactions) {
      const s = i.f;
      (s & ne) !== 0 ? ns(
        /** @type {Derived} */
        i,
        e,
        n,
        r
      ) : (s & (Ht | Oe)) !== 0 && (s & Z) === 0 && Or(i, e, r) && (U(i, Z), Dr(
        /** @type {Effect} */
        i
      ));
    }
}
function Or(t, e, n) {
  const r = n.get(t);
  if (r !== void 0) return r;
  if (t.deps !== null)
    for (const i of t.deps) {
      if (Pn.call(e, i))
        return !0;
      if ((i.f & ne) !== 0 && Or(
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
function Dr(t) {
  T.schedule(t);
}
function rs(t, e) {
  if (!((t.f & Re) !== 0 && (t.f & G) !== 0)) {
    (t.f & Z) !== 0 ? e.d.push(t) : (t.f & Ue) !== 0 && e.m.push(t), U(t, G);
    for (var n = t.first; n !== null; )
      rs(n, e), n = n.next;
  }
}
function is(t) {
  U(t, G);
  for (var e = t.first; e !== null; )
    is(e), e = e.next;
}
let Wn = /* @__PURE__ */ new Set();
const St = /* @__PURE__ */ new Map();
let ss = !1;
function It(t, e) {
  var n = {
    f: 0,
    // TODO ideally we could skip this altogether, but it causes type errors
    v: t,
    reactions: null,
    equals: Hi,
    rv: 0,
    wv: 0
  };
  return n;
}
// @__NO_SIDE_EFFECTS__
function Be(t, e) {
  const n = It(t);
  return xs(n), n;
}
// @__NO_SIDE_EFFECTS__
function as(t, e = !1, n = !0) {
  const r = It(t);
  return e || (r.equals = Fi), r;
}
function Ie(t, e, n = !1) {
  A !== null && // since we are untracking the function inside `$inspect.with` we need to add this check
  // to ensure we error if state is set inside an inspect effect
  (!De || (A.f & Hn) !== 0) && Vi() && (A.f & (ne | Oe | Ht | Hn)) !== 0 && (We === null || !We.has(t)) && Sa();
  let r = n ? _t(e) : e;
  return Zt(t, r, Dn);
}
function Zt(t, e, n = null) {
  if (!t.equals(e)) {
    St.set(t, ot ? e : t.v);
    var r = vt.ensure();
    if (r.capture(t, e), (t.f & ne) !== 0) {
      const i = (
        /** @type {Derived} */
        t
      );
      (t.f & Z) !== 0 && Ir(i), ee === null && Mr(i);
    }
    t.wv = ws(), os(t, Z, n), S !== null && (S.f & G) !== 0 && (S.f & (Re | at)) === 0 && (_e === null ? oo([t]) : _e.push(t)), !r.is_fork && Wn.size > 0 && !ss && Ja();
  }
  return e;
}
function Ja() {
  ss = !1;
  for (const t of Wn) {
    (t.f & G) !== 0 && U(t, Ue);
    let e;
    try {
      e = kn(t);
    } catch {
      e = !0;
    }
    e && Qt(t);
  }
  Wn.clear();
}
function dn(t) {
  Ie(t, t.v + 1);
}
function os(t, e, n) {
  var r = t.reactions;
  if (r !== null)
    for (var i = r.length, s = 0; s < i; s++) {
      var a = r[s], o = a.f, l = (o & Z) === 0;
      if (l && U(a, e), (o & Hn) !== 0)
        Wn.add(
          /** @type {Effect} */
          a
        );
      else if ((o & ne) !== 0) {
        var u = (
          /** @type {Derived} */
          a
        );
        ee == null || ee.delete(u), (o & Nt) === 0 && (o & Ee && (S === null || (S.f & Fn) === 0) && (a.f |= Nt), os(u, Ue, n));
      } else if (l) {
        var c = (
          /** @type {Effect} */
          a
        );
        (o & Oe) !== 0 && Ae !== null && Ae.add(c), n !== null ? n.push(c) : Dr(c);
      }
    }
}
function _t(t) {
  if (typeof t != "object" || t === null || Ct in t)
    return t;
  const e = Oi(t);
  if (e !== ca && e !== fa)
    return t;
  var n = /* @__PURE__ */ new Map(), r = Sr(t), i = /* @__PURE__ */ Be(0), s = Mt, a = (o) => {
    if (Mt === s)
      return o();
    var l = A, u = Mt;
    ke(null), oi(s);
    var c = o();
    return ke(l), oi(u), c;
  };
  return r && n.set("length", /* @__PURE__ */ Be(
    /** @type {any[]} */
    t.length
  )), new Proxy(
    /** @type {any} */
    t,
    {
      defineProperty(o, l, u) {
        (!("value" in u) || u.configurable === !1 || u.enumerable === !1 || u.writable === !1) && Ta();
        var c = n.get(l);
        return c === void 0 ? a(() => {
          var h = /* @__PURE__ */ Be(u.value);
          return n.set(l, h), h;
        }) : Ie(c, u.value, !0), !0;
      },
      deleteProperty(o, l) {
        var u = n.get(l);
        if (u === void 0) {
          if (l in o) {
            const c = a(() => /* @__PURE__ */ Be(X));
            n.set(l, c), dn(i);
          }
        } else
          Ie(u, X), dn(i);
        return !0;
      },
      get(o, l, u) {
        var p;
        if (l === Ct)
          return t;
        var c = n.get(l), h = l in o;
        if (c === void 0 && (!h || (p = Tt(o, l)) != null && p.writable) && (c = a(() => {
          var m = _t(h ? o[l] : X), g = /* @__PURE__ */ Be(m);
          return g;
        }), n.set(l, c)), c !== void 0) {
          var f = N(c);
          return f === X ? void 0 : f;
        }
        return Reflect.get(o, l, u);
      },
      getOwnPropertyDescriptor(o, l) {
        var u = Reflect.getOwnPropertyDescriptor(o, l);
        if (u && "value" in u) {
          var c = n.get(l);
          c && (u.value = N(c));
        } else if (u === void 0) {
          var h = n.get(l), f = h == null ? void 0 : h.v;
          if (h !== void 0 && f !== X)
            return {
              enumerable: !0,
              configurable: !0,
              value: f,
              writable: !0
            };
        }
        return u;
      },
      has(o, l) {
        var f;
        if (l === Ct)
          return !0;
        var u = n.get(l), c = u !== void 0 && u.v !== X || Reflect.has(o, l);
        if (u !== void 0 || S !== null && (!c || (f = Tt(o, l)) != null && f.writable)) {
          u === void 0 && (u = a(() => {
            var p = c ? _t(o[l]) : X, m = /* @__PURE__ */ Be(p);
            return m;
          }), n.set(l, u));
          var h = N(u);
          if (h === X)
            return !1;
        }
        return c;
      },
      set(o, l, u, c) {
        var _;
        var h = n.get(l), f = l in o;
        if (r && l === "length")
          for (var p = u; p < /** @type {Source<number>} */
          h.v; p += 1) {
            var m = n.get(p + "");
            m !== void 0 ? Ie(m, X) : p in o && (m = a(() => /* @__PURE__ */ Be(X)), n.set(p + "", m));
          }
        if (h === void 0)
          (!f || (_ = Tt(o, l)) != null && _.writable) && (h = a(() => /* @__PURE__ */ Be(void 0)), Ie(h, _t(u)), n.set(l, h));
        else {
          f = h.v !== X;
          var g = a(() => _t(u));
          Ie(h, g);
        }
        var v = Reflect.getOwnPropertyDescriptor(o, l);
        if (v != null && v.set && v.set.call(c, u), !f) {
          if (r && typeof l == "string") {
            var b = (
              /** @type {Source<number>} */
              n.get("length")
            ), y = Number(l);
            Number.isInteger(y) && y >= b.v && Ie(b, y + 1);
          }
          dn(i);
        }
        return !0;
      },
      ownKeys(o) {
        N(i);
        var l = Reflect.ownKeys(o).filter((h) => {
          var f = n.get(h);
          return f === void 0 || f.v !== X;
        });
        for (var [u, c] of n)
          c.v !== X && !(u in o) && l.push(u);
        return l;
      },
      setPrototypeOf() {
        Ca();
      }
    }
  );
}
function ri(t) {
  try {
    if (t !== null && typeof t == "object" && Ct in t)
      return t[Ct];
  } catch {
  }
  return t;
}
function Za(t, e) {
  return Object.is(ri(t), ri(e));
}
var ii, ls, us, cs;
function gr() {
  if (ii === void 0) {
    ii = window, ls = /Firefox/.test(navigator.userAgent);
    var t = Element.prototype, e = Node.prototype, n = Text.prototype;
    us = Tt(e, "firstChild").get, cs = Tt(e, "nextSibling").get, ei(t) && (t[cr] = void 0, t[Nn] = null, t[ga] = void 0, t.__e = void 0), ei(n) && (n[sn] = void 0);
  }
}
function ze(t = "") {
  return document.createTextNode(t);
}
// @__NO_SIDE_EFFECTS__
function vn(t) {
  return (
    /** @type {TemplateNode | null} */
    us.call(t)
  );
}
// @__NO_SIDE_EFFECTS__
function lt(t) {
  return (
    /** @type {TemplateNode | null} */
    cs.call(t)
  );
}
function W(t, e) {
  if (!R)
    return /* @__PURE__ */ vn(t);
  var n = /* @__PURE__ */ vn(j);
  if (n === null)
    n = j.appendChild(ze());
  else if (e && n.nodeType !== Ar) {
    var r = ze();
    return n == null || n.before(r), me(r), r;
  }
  return e && hs(
    /** @type {Text} */
    n
  ), me(n), n;
}
function ge(t, e = 1, n = !1) {
  let r = R ? j : t;
  for (var i; e--; )
    i = r, r = /** @type {TemplateNode} */
    /* @__PURE__ */ lt(r);
  if (!R)
    return r;
  if (n) {
    if ((r == null ? void 0 : r.nodeType) !== Ar) {
      var s = ze();
      return r === null ? i == null || i.after(s) : r.before(s), me(s), s;
    }
    hs(
      /** @type {Text} */
      r
    );
  }
  return me(r), r;
}
function fs(t) {
  t.textContent = "";
}
function ds() {
  return !1;
}
function Rr(t, e, n) {
  return (
    /** @type {T extends keyof HTMLElementTagNameMap ? HTMLElementTagNameMap[T] : Element} */
    n ? document.createElement(t, { is: n }) : document.createElement(t)
  );
}
function hs(t) {
  if (
    /** @type {string} */
    t.nodeValue.length < 65536
  )
    return;
  let e = t.nextSibling;
  for (; e !== null && e.nodeType === Ar; )
    e.remove(), t.nodeValue += /** @type {string} */
    e.nodeValue, e = t.nextSibling;
}
let si = !1;
function Qa() {
  si || (si = !0, document.addEventListener(
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
            (e = n[Pi]) == null || e.call(n);
      });
    },
    // In the capture phase to guarantee we get noticed of it (no possibility of stopPropagation)
    { capture: !0 }
  ));
}
function Lr(t) {
  var e = A, n = S;
  ke(null), Ye(null);
  try {
    return t();
  } finally {
    ke(e), Ye(n);
  }
}
function eo(t) {
  S === null && (A === null && wa(), ya()), ot && xa();
}
function to(t, e) {
  var n = e.last;
  n === null ? e.last = e.first = t : (n.next = t, t.prev = n, e.last = t);
}
function Xe(t, e) {
  var n = S;
  n !== null && (n.f & ie) !== 0 && (t |= ie);
  var r = {
    ctx: se,
    deps: null,
    nodes: null,
    f: t | Z | Ee,
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
  T == null || T.register_created_effect(r);
  var i = r;
  if ((t & Gt) !== 0)
    Bt !== null ? Bt.push(r) : vt.ensure().schedule(r);
  else if (e !== null) {
    try {
      Qt(r);
    } catch (a) {
      throw ae(r), a;
    }
    i.deps === null && i.teardown === null && i.nodes === null && i.first === i.last && // either `null`, or a singular child
    (i.f & Dt) === 0 && (i = i.first, (t & Oe) !== 0 && (t & Kt) !== 0 && i !== null && (i.f |= Kt));
  }
  if (i !== null && (i.parent = n, n !== null && to(i, n), A !== null && (A.f & ne) !== 0 && (t & at) === 0)) {
    var s = (
      /** @type {Derived} */
      A
    );
    (s.effects ?? (s.effects = [])).push(i);
  }
  return r;
}
function Pr() {
  return A !== null && !De;
}
function jr(t) {
  const e = Xe(Kn, null);
  return U(e, G), e.teardown = t, e;
}
function Br(t) {
  eo();
  var e = (
    /** @type {Effect} */
    S.f
  ), n = !A && (e & Re) !== 0 && se !== null && !se.i;
  if (n) {
    var r = (
      /** @type {ComponentContext} */
      se
    );
    (r.e ?? (r.e = [])).push(t);
  } else
    return vs(t);
}
function vs(t) {
  return Xe(Gt | pa, t);
}
function no(t) {
  vt.ensure();
  const e = Xe(at | Dt, t);
  return () => {
    ae(e);
  };
}
function ro(t) {
  vt.ensure();
  const e = Xe(at | Dt, t);
  return (n = {}) => new Promise((r) => {
    n.outro ? At(e, () => {
      ae(e), r(void 0);
    }) : (ae(e), r(void 0));
  });
}
function ps(t) {
  return Xe(Gt, t);
}
function io(t) {
  return Xe(Ht | Dt, t);
}
function Hr(t, e = 0) {
  return Xe(Kn | e, t);
}
function te(t, e = [], n = [], r = []) {
  za(r, e, n, (i) => {
    Xe(Kn, () => {
      t(...i.map(N));
    });
  });
}
function Fr(t, e = 0) {
  var n = Xe(Oe | e, t);
  return n;
}
function we(t) {
  return Xe(Re | Dt, t);
}
function ms(t) {
  var e = t.teardown;
  if (e !== null) {
    const n = ot, r = A;
    ai(!0), ke(null);
    try {
      e.call(null);
    } finally {
      ai(n), ke(r);
    }
  }
}
function Vr(t, e = !1) {
  var n = t.first;
  for (t.first = t.last = null; n !== null; ) {
    const i = n.ac;
    i !== null && Lr(() => {
      i.abort(Jn);
    });
    var r = n.next;
    (n.f & at) !== 0 ? n.parent = null : ae(n, e), n = r;
  }
}
function so(t) {
  for (var e = t.first; e !== null; ) {
    var n = e.next;
    (e.f & Re) === 0 && ae(e), e = n;
  }
}
function ae(t, e = !0) {
  var n = !1;
  (e || (t.f & va) !== 0) && t.nodes !== null && t.nodes.end !== null && (ao(
    t.nodes.start,
    /** @type {TemplateNode} */
    t.nodes.end
  ), n = !0), t.f |= ur, Vr(t, e && !n), pn(t, 0);
  var r = t.nodes && t.nodes.t;
  if (r !== null)
    for (const s of r)
      s.stop();
  ms(t), t.f ^= ur, t.f |= pe;
  var i = t.parent;
  i !== null && i.first !== null && gs(t), t.next = t.prev = t.teardown = t.ctx = t.deps = t.fn = t.nodes = t.ac = t.b = null;
}
function ao(t, e) {
  for (; t !== null; ) {
    var n = t === e ? null : /* @__PURE__ */ lt(t);
    t.remove(), t = n;
  }
}
function gs(t) {
  var e = t.parent, n = t.prev, r = t.next;
  n !== null && (n.next = r), r !== null && (r.prev = n), e !== null && (e.first === t && (e.first = r), e.last === t && (e.last = n));
}
function At(t, e, n = !0) {
  var r = [];
  _s(t, r, !0);
  var i = () => {
    n && ae(t), e && e();
  }, s = r.length;
  if (s > 0) {
    var a = () => --s || i();
    for (var o of r)
      o.out(a);
  } else
    i();
}
function _s(t, e, n) {
  if ((t.f & ie) === 0) {
    t.f ^= ie;
    var r = t.nodes && t.nodes.t;
    if (r !== null)
      for (const o of r)
        (o.is_global || n) && e.push(o);
    for (var i = t.first; i !== null; ) {
      var s = i.next;
      if ((i.f & at) === 0) {
        var a = (i.f & Kt) !== 0 || // If this is a branch effect without a block effect parent,
        // it means the parent block effect was pruned. In that case,
        // transparency information was transferred to the branch effect.
        (i.f & Re) !== 0 && (t.f & Oe) !== 0;
        _s(i, e, a ? n : !1);
      }
      i = s;
    }
  }
}
function qn(t) {
  bs(t, !0);
}
function bs(t, e) {
  if ((t.f & ie) !== 0) {
    t.f ^= ie, (t.f & G) === 0 && (U(t, Z), vt.ensure().schedule(t));
    for (var n = t.first; n !== null; ) {
      var r = n.next, i = (n.f & Kt) !== 0 || (n.f & Re) !== 0;
      bs(n, i ? e : !1), n = r;
    }
    var s = t.nodes && t.nodes.t;
    if (s !== null)
      for (const a of s)
        (a.is_global || e) && a.in();
  }
}
function zr(t, e) {
  if (t.nodes)
    for (var n = t.nodes.start, r = t.nodes.end; n !== null; ) {
      var i = n === r ? null : /* @__PURE__ */ lt(n);
      e.append(n), n = i;
    }
}
let Rn = !1, ot = !1;
function ai(t) {
  ot = t;
}
let A = null, De = !1;
function ke(t) {
  A = t;
}
let S = null;
function Ye(t) {
  S = t;
}
let We = null;
function xs(t) {
  A !== null && (We ?? (We = /* @__PURE__ */ new Set())).add(t);
}
let ce = null, fe = 0, _e = null;
function oo(t) {
  _e = t;
}
let ys = 1, bt = 0, Mt = bt;
function oi(t) {
  Mt = t;
}
function ws() {
  return ++ys;
}
function kn(t) {
  var e = t.f;
  if ((e & Z) !== 0)
    return !0;
  if (e & ne && (t.f &= ~Nt), (e & Ue) !== 0) {
    for (var n = (
      /** @type {Value[]} */
      t.deps
    ), r = n.length, i = 0; i < r; i++) {
      var s = n[i];
      if (kn(
        /** @type {Derived} */
        s
      ) && Zi(
        /** @type {Derived} */
        s
      ), s.wv > t.wv)
        return !0;
    }
    (e & Ee) !== 0 && // During time traveling we don't want to reset the status so that
    // traversal of the graph in the other batches still happens
    ee === null && U(t, G);
  }
  return !1;
}
function Es(t, e, n = !0) {
  var r = t.reactions;
  if (r !== null && !(We !== null && We.has(t)))
    for (var i = 0; i < r.length; i++) {
      var s = r[i];
      (s.f & ne) !== 0 ? Es(
        /** @type {Derived} */
        s,
        e,
        !1
      ) : e === s && (n ? U(s, Z) : (s.f & G) !== 0 && U(s, Ue), Dr(
        /** @type {Effect} */
        s
      ));
    }
}
function ks(t) {
  var g;
  var e = ce, n = fe, r = _e, i = A, s = We, a = se, o = De, l = Mt, u = t.f;
  ce = /** @type {null | Value[]} */
  null, fe = 0, _e = null, A = (u & (Re | at)) === 0 ? t : null, We = null, Jt(t.ctx), De = !1, Mt = ++bt, t.ac !== null && (Lr(() => {
    t.ac.abort(Jn);
  }), t.ac = null);
  try {
    t.f |= Fn;
    var c = (
      /** @type {Function} */
      t.fn
    ), h = c();
    t.f |= Ot;
    var f = t.deps, p = T == null ? void 0 : T.is_fork;
    if (ce !== null) {
      var m;
      if (p || pn(t, fe), f !== null && fe > 0)
        for (f.length = fe + ce.length, m = 0; m < ce.length; m++)
          f[fe + m] = ce[m];
      else
        t.deps = f = ce;
      if (Pr() && (t.f & Ee) !== 0)
        for (m = fe; m < f.length; m++)
          ((g = f[m]).reactions ?? (g.reactions = [])).push(t);
    } else !p && f !== null && fe < f.length && (pn(t, fe), f.length = fe);
    if (Vi() && _e !== null && !De && f !== null && (t.f & (ne | Ue | Z)) === 0)
      for (m = 0; m < /** @type {Source[]} */
      _e.length; m++)
        Es(
          _e[m],
          /** @type {Effect} */
          t
        );
    if (i !== null && i !== t) {
      if (bt++, i.deps !== null)
        for (let v = 0; v < n; v += 1)
          i.deps[v].rv = bt;
      if (e !== null)
        for (const v of e)
          v.rv = bt;
      _e !== null && (r === null ? r = _e : r.push(.../** @type {Source[]} */
      _e));
    }
    return (t.f & ht) !== 0 && (t.f ^= ht), h;
  } catch (v) {
    return Wi(v);
  } finally {
    t.f ^= Fn, ce = e, fe = n, _e = r, A = i, We = s, Jt(a), De = o, Mt = l;
  }
}
function lo(t, e) {
  let n = e.reactions;
  if (n !== null) {
    var r = la.call(n, t);
    if (r !== -1) {
      var i = n.length - 1;
      i === 0 ? n = e.reactions = null : (n[r] = n[i], n.pop());
    }
  }
  if (n === null && (e.f & ne) !== 0 && // Destroying a child effect while updating a parent effect can cause a dependency to appear
  // to be unused, when in fact it is used by the currently-updating parent. Checking `new_deps`
  // allows us to skip the expensive work of disconnecting and immediately reconnecting it
  (ce === null || !Pn.call(ce, e))) {
    var s = (
      /** @type {Derived} */
      e
    );
    (s.f & Ee) !== 0 && (s.f ^= Ee, s.f &= ~Nt), s.v !== X && Mr(s), Ya(s), pn(s, 0);
  }
}
function pn(t, e) {
  var n = t.deps;
  if (n !== null)
    for (var r = e; r < n.length; r++)
      lo(t, n[r]);
}
function Qt(t) {
  var e = t.f;
  if ((e & pe) === 0) {
    U(t, G);
    var n = S, r = Rn;
    S = t, Rn = !0;
    try {
      (e & (Oe | Ri)) !== 0 ? so(t) : Vr(t), ms(t);
      var i = ks(t);
      t.teardown = typeof i == "function" ? i : null, t.wv = ys;
      var s;
      Ii && Ra && (t.f & Z) !== 0 && t.deps;
    } finally {
      Rn = r, S = n;
    }
  }
}
function N(t) {
  var e = t.f, n = (e & ne) !== 0;
  if (A !== null && !De) {
    var r = S !== null && (S.f & pe) !== 0;
    if (!r && (We === null || !We.has(t))) {
      var i = A.deps;
      if ((A.f & Fn) !== 0)
        t.rv < bt && (t.rv = bt, ce === null && i !== null && i[fe] === t ? fe++ : ce === null ? ce = [t] : ce.push(t));
      else {
        A.deps ?? (A.deps = []), Pn.call(A.deps, t) || A.deps.push(t);
        var s = t.reactions;
        s === null ? t.reactions = [A] : Pn.call(s, A) || s.push(A);
      }
    }
  }
  if (ot && St.has(t))
    return St.get(t);
  if (n) {
    var a = (
      /** @type {Derived} */
      t
    );
    if (ot) {
      var o = a.v;
      return ((a.f & G) === 0 && a.reactions !== null || Ts(a)) && (o = Ir(a)), St.set(a, o), o;
    }
    var l = (a.f & Ee) === 0 && !De && A !== null && (Rn || (A.f & Ee) !== 0), u = (a.f & Ot) === 0;
    kn(a) && (l && (a.f |= Ee), Zi(a)), l && !u && (Qi(a), $s(a));
  }
  if (ee != null && ee.has(t))
    return ee.get(t);
  if ((t.f & ht) !== 0)
    throw t.v;
  return t.v;
}
function $s(t) {
  if (t.f |= Ee, t.deps !== null)
    for (const e of t.deps)
      (e.reactions ?? (e.reactions = [])).push(t), (e.f & ne) !== 0 && (e.f & Ee) === 0 && (Qi(
        /** @type {Derived} */
        e
      ), $s(
        /** @type {Derived} */
        e
      ));
}
function Ts(t) {
  if (t.v === X) return !0;
  if (t.deps === null) return !1;
  for (const e of t.deps)
    if (St.has(e) || (e.f & ne) !== 0 && Ts(
      /** @type {Derived} */
      e
    ))
      return !0;
  return !1;
}
function Wr(t) {
  var e = De;
  try {
    return De = !0, t();
  } finally {
    De = e;
  }
}
const xt = Symbol("events"), Cs = /* @__PURE__ */ new Set(), _r = /* @__PURE__ */ new Set();
function uo(t, e, n, r = {}) {
  function i(s) {
    if (r.capture || br.call(e, s), !s.cancelBubble)
      return Lr(() => n == null ? void 0 : n.call(this, s));
  }
  return st(() => {
    e.addEventListener(t, i, r);
  }), i;
}
function Ss(t, e, n, r, i) {
  var s = { capture: r, passive: i }, a = uo(t, e, n, s);
  (e === document.body || // @ts-ignore
  e === window || // @ts-ignore
  e === document || // Firefox has quirky behavior, it can happen that we still get "canplay" events when the element is already removed
  e instanceof HTMLMediaElement) && jr(() => {
    e.removeEventListener(t, a, s);
  });
}
function J(t, e, n) {
  (e[xt] ?? (e[xt] = {}))[t] = n;
}
function pt(t) {
  for (var e = 0; e < t.length; e++)
    Cs.add(t[e]);
  for (var n of _r)
    n(t);
}
let li = null;
function br(t) {
  var g, v;
  var e = this, n = (
    /** @type {Node} */
    e.ownerDocument
  ), r = t.type, i = ((g = t.composedPath) == null ? void 0 : g.call(t)) || [], s = (
    /** @type {null | Element} */
    i[0] || t.target
  );
  li = t;
  var a = 0, o = li === t && t[xt];
  if (o) {
    var l = i.indexOf(o);
    if (l !== -1 && (e === document || e === /** @type {any} */
    window)) {
      t[xt] = e;
      return;
    }
    var u = i.indexOf(e);
    if (u === -1)
      return;
    l <= u && (a = l);
  }
  if (s = /** @type {Element} */
  i[a] || t.target, s !== e) {
    Bn(t, "currentTarget", {
      configurable: !0,
      get() {
        return s || n;
      }
    });
    var c = A, h = S;
    ke(null), Ye(null);
    try {
      for (var f, p = []; s !== null && s !== e; ) {
        try {
          var m = (v = s[xt]) == null ? void 0 : v[r];
          m != null && (!/** @type {any} */
          s.disabled || // DOM could've been updated already by the time this is reached, so we check this as well
          // -> the target could not have been disabled because it emits the event in the first place
          t.target === s) && m.call(s, t);
        } catch (b) {
          f ? p.push(b) : f = b;
        }
        if (t.cancelBubble) break;
        a++, s = a < i.length ? (
          /** @type {Element} */
          i[a]
        ) : null;
      }
      if (f) {
        for (let b of p)
          queueMicrotask(() => {
            throw b;
          });
        throw f;
      }
    } finally {
      t[xt] = e, delete t.currentTarget, ke(c), Ye(h);
    }
  }
}
var Ai;
const ir = (
  // We gotta write it like this because after downleveling the pure comment may end up in the wrong location
  ((Ai = globalThis == null ? void 0 : globalThis.window) == null ? void 0 : Ai.trustedTypes) && /* @__PURE__ */ globalThis.window.trustedTypes.createPolicy("svelte-trusted-html", {
    /** @param {string} html */
    createHTML: (t) => t
  })
);
function co(t) {
  return (
    /** @type {string} */
    (ir == null ? void 0 : ir.createHTML(t)) ?? t
  );
}
function fo(t) {
  var e = Rr("template");
  return e.innerHTML = co(t.replaceAll("<!>", "<!---->")), e.content;
}
function xr(t, e) {
  var n = (
    /** @type {Effect} */
    S
  );
  n.nodes === null && (n.nodes = { start: t, end: e, a: null, t: null });
}
// @__NO_SIDE_EFFECTS__
function K(t, e) {
  var n = (e & aa) !== 0, r, i = !t.startsWith("<!>");
  return () => {
    if (R)
      return xr(j, null), j;
    r === void 0 && (r = fo(i ? t : "<!>" + t), r = /** @type {TemplateNode} */
    /* @__PURE__ */ vn(r));
    var s = (
      /** @type {TemplateNode} */
      n || ls ? document.importNode(r, !0) : r.cloneNode(!0)
    );
    return xr(s, s), s;
  };
}
function Y(t, e) {
  if (R) {
    var n = (
      /** @type {Effect & { nodes: EffectNodes }} */
      S
    );
    ((n.f & Ot) === 0 || n.nodes.end === null) && (n.nodes.end = j), En();
    return;
  }
  t !== null && t.before(
    /** @type {Node} */
    e
  );
}
const ho = ["touchstart", "touchmove"];
function vo(t) {
  return ho.includes(t);
}
function $e(t, e) {
  var n = e == null ? "" : typeof e == "object" ? `${e}` : e;
  n !== /** @type {any} */
  (t[sn] ?? (t[sn] = t.nodeValue)) && (t[sn] = n, t.nodeValue = `${n}`);
}
function As(t, e) {
  return Ms(t, e);
}
function po(t, e) {
  gr(), e.intro = e.intro ?? !1;
  const n = e.target, r = R, i = j;
  try {
    for (var s = /* @__PURE__ */ vn(n); s && (s.nodeType !== wn || /** @type {Comment} */
    s.data !== Ni); )
      s = /* @__PURE__ */ lt(s);
    if (!s)
      throw Xt;
    rt(!0), me(
      /** @type {Comment} */
      s
    );
    const a = Ms(t, { ...e, anchor: s });
    return rt(!1), /**  @type {Exports} */
    a;
  } catch (a) {
    if (a instanceof Error && a.message.split(`
`).some((o) => o.startsWith("https://svelte.dev/e/")))
      throw a;
    return a !== Xt && console.warn("Failed to hydrate: ", a), e.recover === !1 && ka(), gr(), fs(n), rt(!1), As(t, e);
  } finally {
    rt(r), me(i);
  }
}
const An = /* @__PURE__ */ new Map();
function Ms(t, { target: e, anchor: n, props: r = {}, events: i, context: s, intro: a = !0, transformError: o }) {
  gr();
  var l = void 0, u = ro(() => {
    var c = n ?? e.appendChild(ze());
    Fa(
      /** @type {TemplateNode} */
      c,
      {
        pending: () => {
        }
      },
      (p) => {
        Ce({});
        var m = (
          /** @type {ComponentContext} */
          se
        );
        if (s && (m.c = s), i && (r.$$events = i), R && xr(
          /** @type {TemplateNode} */
          p,
          null
        ), l = t(p, r) || {}, R && (S.nodes.end = j, j === null || j.nodeType !== wn || /** @type {Comment} */
        j.data !== Cr))
          throw Zn(), Xt;
        Se();
      },
      o
    );
    var h = /* @__PURE__ */ new Set(), f = (p) => {
      for (var m = 0; m < p.length; m++) {
        var g = p[m];
        if (!h.has(g)) {
          h.add(g);
          var v = vo(g);
          for (const _ of [e, document]) {
            var b = An.get(_);
            b === void 0 && (b = /* @__PURE__ */ new Map(), An.set(_, b));
            var y = b.get(g);
            y === void 0 ? (_.addEventListener(g, br, { passive: v }), b.set(g, 1)) : b.set(g, y + 1);
          }
        }
      }
    };
    return f(Gn(Cs)), _r.add(f), () => {
      var v;
      for (var p of h)
        for (const b of [e, document]) {
          var m = (
            /** @type {Map<string, number>} */
            An.get(b)
          ), g = (
            /** @type {number} */
            m.get(p)
          );
          --g == 0 ? (b.removeEventListener(p, br), m.delete(p), m.size === 0 && An.delete(b)) : m.set(p, g);
        }
      _r.delete(f), c !== n && ((v = c.parentNode) == null || v.removeChild(c));
    };
  });
  return yr.set(l, u), l;
}
let yr = /* @__PURE__ */ new WeakMap();
function mo(t, e) {
  const n = yr.get(t);
  return n ? (yr.delete(t), n(e)) : Promise.resolve();
}
var Ne, Fe, ve, $t, xn, yn, Xn;
class go {
  /**
   * @param {TemplateNode} anchor
   * @param {boolean} transition
   */
  constructor(e, n = !0) {
    /** @type {TemplateNode} */
    F(this, "anchor");
    /** @type {Map<Batch, Key>} */
    C(this, Ne, /* @__PURE__ */ new Map());
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
    C(this, Fe, /* @__PURE__ */ new Map());
    /**
     * Similar to #onscreen with respect to the keys, but contains branches that are not yet
     * in the DOM, because their insertion is deferred.
     * @type {Map<Key, Branch>}
     */
    C(this, ve, /* @__PURE__ */ new Map());
    /**
     * Keys of effects that are currently outroing
     * @type {Set<Key>}
     */
    C(this, $t, /* @__PURE__ */ new Set());
    /**
     * Whether to pause (i.e. outro) on change, or destroy immediately.
     * This is necessary for `<svelte:element>`
     */
    C(this, xn, !0);
    /**
     * @param {Batch} batch
     */
    C(this, yn, (e) => {
      if (d(this, Ne).has(e)) {
        var n = (
          /** @type {Key} */
          d(this, Ne).get(e)
        ), r = d(this, Fe).get(n);
        if (r)
          qn(r), d(this, $t).delete(n);
        else {
          var i = d(this, ve).get(n);
          i && (qn(i.effect), d(this, Fe).set(n, i.effect), d(this, ve).delete(n), i.fragment.lastChild.remove(), this.anchor.before(i.fragment), r = i.effect);
        }
        for (const [s, a] of d(this, Ne)) {
          if (d(this, Ne).delete(s), s === e)
            break;
          const o = d(this, ve).get(a);
          o && (ae(o.effect), d(this, ve).delete(a));
        }
        for (const [s, a] of d(this, Fe)) {
          if (s === n || d(this, $t).has(s)) continue;
          const o = () => {
            if (Array.from(d(this, Ne).values()).includes(s)) {
              var u = document.createDocumentFragment();
              zr(a, u), u.append(ze()), d(this, ve).set(s, { effect: a, fragment: u });
            } else
              ae(a);
            d(this, $t).delete(s), d(this, Fe).delete(s);
          };
          d(this, xn) || !r ? (d(this, $t).add(s), At(a, o, !1)) : o();
        }
      }
    });
    /**
     * @param {Batch} batch
     */
    C(this, Xn, (e) => {
      d(this, Ne).delete(e);
      const n = Array.from(d(this, Ne).values());
      for (const [r, i] of d(this, ve))
        n.includes(r) || (ae(i.effect), d(this, ve).delete(r));
    });
    this.anchor = e, $(this, xn, n);
  }
  /**
   *
   * @param {any} key
   * @param {null | ((target: TemplateNode) => void)} fn
   */
  ensure(e, n) {
    var r = (
      /** @type {Batch} */
      T
    ), i = ds();
    if (n && !d(this, Fe).has(e) && !d(this, ve).has(e))
      if (i) {
        var s = document.createDocumentFragment(), a = ze();
        s.append(a), d(this, ve).set(e, {
          effect: we(() => n(a)),
          fragment: s
        });
      } else
        d(this, Fe).set(
          e,
          we(() => n(this.anchor))
        );
    if (d(this, Ne).set(r, e), i) {
      for (const [o, l] of d(this, Fe))
        o === e ? r.unskip_effect(l) : r.skip_effect(l);
      for (const [o, l] of d(this, ve))
        o === e ? r.unskip_effect(l.effect) : r.skip_effect(l.effect);
      r.oncommit(d(this, yn)), r.ondiscard(d(this, Xn));
    } else
      R && (this.anchor = j), d(this, yn).call(this, r);
  }
}
Ne = new WeakMap(), Fe = new WeakMap(), ve = new WeakMap(), $t = new WeakMap(), xn = new WeakMap(), yn = new WeakMap(), Xn = new WeakMap();
function en(t, e, n = !1) {
  var r;
  R && (r = j, En());
  var i = new go(t), s = n ? Kt : 0;
  function a(o, l) {
    if (R) {
      var u = Bi(
        /** @type {TemplateNode} */
        r
      );
      if (o !== parseInt(u.substring(1))) {
        var c = Vn();
        me(c), i.anchor = c, rt(!1), i.ensure(o, l), rt(!0);
        return;
      }
    }
    i.ensure(o, l);
  }
  Fr(() => {
    var o = !1;
    e((l, u = 0) => {
      o = !0, a(u, l);
    }), o || a(-1, null);
  }, s);
}
function Ns(t, e) {
  return e;
}
function _o(t, e, n) {
  for (var r = [], i = e.length, s, a = e.length, o = 0; o < i; o++) {
    let h = e[o];
    At(
      h,
      () => {
        if (s) {
          if (s.pending.delete(h), s.done.add(h), s.pending.size === 0) {
            var f = (
              /** @type {Set<EachOutroGroup>} */
              t.outrogroups
            );
            wr(t, Gn(s.done)), f.delete(s), f.size === 0 && (t.outrogroups = null);
          }
        } else
          a -= 1;
      },
      !1
    );
  }
  if (a === 0) {
    var l = r.length === 0 && n !== null;
    if (l) {
      var u = (
        /** @type {Element} */
        n
      ), c = (
        /** @type {Element} */
        u.parentNode
      );
      fs(c), c.append(u), t.items.clear();
    }
    wr(t, e, !l);
  } else
    s = {
      pending: new Set(e),
      done: /* @__PURE__ */ new Set()
    }, (t.outrogroups ?? (t.outrogroups = /* @__PURE__ */ new Set())).add(s);
}
function wr(t, e, n = !0) {
  var r;
  if (t.pending.size > 0) {
    r = /* @__PURE__ */ new Set();
    for (const a of t.pending.values())
      for (const o of a)
        r.add(
          /** @type {EachItem} */
          t.items.get(o).e
        );
  }
  for (var i = 0; i < e.length; i++) {
    var s = e[i];
    if (r != null && r.has(s)) {
      s.f |= Ve;
      const a = document.createDocumentFragment();
      zr(s, a);
    } else
      ae(e[i], n);
  }
}
var ui;
function Is(t, e, n, r, i, s = null) {
  var a = t, o = /* @__PURE__ */ new Map(), l = (e & Mi) !== 0;
  if (l) {
    var u = (
      /** @type {Element} */
      t
    );
    a = R ? me(/* @__PURE__ */ vn(u)) : u.appendChild(ze());
  }
  R && En();
  var c = null, h = /* @__PURE__ */ Ji(() => {
    var _ = n();
    return (
      /** @type {V[]} */
      Sr(_) ? _ : _ == null ? [] : Gn(_)
    );
  }), f, p = /* @__PURE__ */ new Map(), m = !0;
  function g(_) {
    (y.effect.f & pe) === 0 && (y.pending.delete(_), y.fallback = c, bo(y, f, a, e, r), c !== null && (f.length === 0 ? (c.f & Ve) === 0 ? qn(c) : (c.f ^= Ve, un(c, null, a)) : At(c, () => {
      c = null;
    })));
  }
  function v(_) {
    y.pending.delete(_);
  }
  var b = Fr(() => {
    f = /** @type {V[]} */
    N(h);
    var _ = f.length;
    let E = !1;
    if (R) {
      var x = Bi(a) === Tr;
      x !== (_ === 0) && (a = Vn(), me(a), rt(!1), E = !0);
    }
    for (var k = /* @__PURE__ */ new Set(), D = (
      /** @type {Batch} */
      T
    ), z = ds(), P = 0; P < _; P += 1) {
      R && j.nodeType === wn && /** @type {Comment} */
      j.data === Cr && (a = /** @type {Comment} */
      j, E = !0, rt(!1));
      var B = f[P], q = r(B, P), Q = m ? null : o.get(q);
      Q ? (Q.v && Zt(Q.v, B), Q.i && Zt(Q.i, P), z && D.unskip_effect(Q.e)) : (Q = xo(
        o,
        m ? a : ui ?? (ui = ze()),
        B,
        q,
        P,
        i,
        e,
        n
      ), m || (Q.e.f |= Ve), o.set(q, Q)), k.add(q);
    }
    if (_ === 0 && s && !c && (m ? c = we(() => s(a)) : (c = we(() => s(ui ?? (ui = ze()))), c.f |= Ve)), _ > k.size && ba(), R && _ > 0 && me(Vn()), !m)
      if (p.set(D, k), z) {
        for (const [mt, Lt] of o)
          k.has(mt) || D.skip_effect(Lt.e);
        D.oncommit(g), D.ondiscard(v);
      } else
        g(D);
    E && rt(!0), N(h);
  }), y = { effect: b, items: o, pending: p, outrogroups: null, fallback: c };
  m = !1, R && (a = j);
}
function tn(t) {
  for (; t !== null && (t.f & Re) === 0; )
    t = t.next;
  return t;
}
function bo(t, e, n, r, i) {
  var B, q, Q, mt, Lt, Ke, w, oe, Kr;
  var s = (r & ea) !== 0, a = e.length, o = t.items, l = tn(t.effect.first), u, c = null, h, f = [], p = [], m, g, v, b;
  if (s)
    for (b = 0; b < a; b += 1)
      m = e[b], g = i(m, b), v = /** @type {EachItem} */
      o.get(g).e, (v.f & Ve) === 0 && ((q = (B = v.nodes) == null ? void 0 : B.a) == null || q.measure(), (h ?? (h = /* @__PURE__ */ new Set())).add(v));
  for (b = 0; b < a; b += 1) {
    if (m = e[b], g = i(m, b), v = /** @type {EachItem} */
    o.get(g).e, t.outrogroups !== null)
      for (const Je of t.outrogroups)
        Je.pending.delete(v), Je.done.delete(v);
    if ((v.f & ie) !== 0 && (qn(v), s && ((mt = (Q = v.nodes) == null ? void 0 : Q.a) == null || mt.unfix(), (h ?? (h = /* @__PURE__ */ new Set())).delete(v))), (v.f & Ve) !== 0)
      if (v.f ^= Ve, v === l)
        un(v, null, n);
      else {
        var y = c ? c.next : l;
        v === t.effect.last && (t.effect.last = v.prev), v.prev && (v.prev.next = v.next), v.next && (v.next.prev = v.prev), ut(t, c, v), ut(t, v, y), un(v, y, n), c = v, f = [], p = [], l = tn(c.next);
        continue;
      }
    if (v !== l) {
      if (u !== void 0 && u.has(v)) {
        if (f.length < p.length) {
          var _ = p[0], E;
          c = _.prev;
          var x = f[0], k = f[f.length - 1];
          for (E = 0; E < f.length; E += 1)
            un(f[E], _, n);
          for (E = 0; E < p.length; E += 1)
            u.delete(p[E]);
          ut(t, x.prev, k.next), ut(t, c, x), ut(t, k, _), l = _, c = k, b -= 1, f = [], p = [];
        } else
          u.delete(v), un(v, l, n), ut(t, v.prev, v.next), ut(t, v, c === null ? t.effect.first : c.next), ut(t, c, v), c = v;
        continue;
      }
      for (f = [], p = []; l !== null && l !== v; )
        (u ?? (u = /* @__PURE__ */ new Set())).add(l), p.push(l), l = tn(l.next);
      if (l === null)
        continue;
    }
    (v.f & Ve) === 0 && f.push(v), c = v, l = tn(v.next);
  }
  if (t.outrogroups !== null) {
    for (const Je of t.outrogroups)
      Je.pending.size === 0 && (wr(t, Gn(Je.done)), (Lt = t.outrogroups) == null || Lt.delete(Je));
    t.outrogroups.size === 0 && (t.outrogroups = null);
  }
  if (l !== null || u !== void 0) {
    var D = [];
    if (u !== void 0)
      for (v of u)
        (v.f & ie) === 0 && D.push(v);
    for (; l !== null; )
      (l.f & ie) === 0 && l !== t.fallback && D.push(l), l = tn(l.next);
    var z = D.length;
    if (z > 0) {
      var P = (r & Mi) !== 0 && a === 0 ? n : null;
      if (s) {
        for (b = 0; b < z; b += 1)
          (w = (Ke = D[b].nodes) == null ? void 0 : Ke.a) == null || w.measure();
        for (b = 0; b < z; b += 1)
          (Kr = (oe = D[b].nodes) == null ? void 0 : oe.a) == null || Kr.fix();
      }
      _o(t, D, P);
    }
  }
  s && st(() => {
    var Je, Jr;
    if (h !== void 0)
      for (v of h)
        (Jr = (Je = v.nodes) == null ? void 0 : Je.a) == null || Jr.apply();
  });
}
function xo(t, e, n, r, i, s, a, o) {
  var l = (a & Zs) !== 0 ? (a & ta) === 0 ? /* @__PURE__ */ as(n, !1, !1) : It(n) : null, u = (a & Qs) !== 0 ? It(i) : null;
  return {
    v: l,
    i: u,
    e: we(() => (s(e, l ?? n, u ?? i, o), () => {
      t.delete(r);
    }))
  };
}
function un(t, e, n) {
  if (t.nodes)
    for (var r = t.nodes.start, i = t.nodes.end, s = e && (e.f & Ve) === 0 ? (
      /** @type {EffectNodes} */
      e.nodes.start
    ) : n; r !== null; ) {
      var a = (
        /** @type {TemplateNode} */
        /* @__PURE__ */ lt(r)
      );
      if (s.before(r), r === i)
        return;
      r = a;
    }
}
function ut(t, e, n) {
  e === null ? t.effect.first = n : e.next = n, n === null ? t.effect.last = e : n.prev = e;
}
function Os(t, e, n, r, i) {
  var o;
  R && En();
  var s = (o = e.$$slots) == null ? void 0 : o[n], a = !1;
  s === !0 && (s = e.children, a = !0), s === void 0 || s(t, a ? () => r : r);
}
function Le(t, e) {
  ps(() => {
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
      const i = Rr("style");
      i.id = e.hash, i.textContent = e.code, r.appendChild(i);
    }
  });
}
const ci = [...` 	
\r\f \v\uFEFF`];
function yo(t, e, n) {
  var r = t == null ? "" : "" + t;
  if (n) {
    for (var i of Object.keys(n))
      if (n[i])
        r = r ? r + " " + i : i;
      else if (r.length)
        for (var s = i.length, a = 0; (a = r.indexOf(i, a)) >= 0; ) {
          var o = a + s;
          (a === 0 || ci.includes(r[a - 1])) && (o === r.length || ci.includes(r[o])) ? r = (a === 0 ? "" : r.substring(0, a)) + r.substring(o + 1) : a = o;
        }
  }
  return r === "" ? null : r;
}
function Ds(t, e, n, r, i, s) {
  var a = (
    /** @type {any} */
    t[cr]
  );
  if (R || a !== n || a === void 0) {
    var o = yo(n, r, s);
    (!R || o !== t.getAttribute("class")) && (o == null ? t.removeAttribute("class") : t.className = o), t[cr] = n;
  } else if (s && i !== s)
    for (var l in s) {
      var u = !!s[l];
      (i == null || u !== !!i[l]) && t.classList.toggle(l, u);
    }
  return s;
}
function Rs(t, e, n = !1) {
  if (t.multiple) {
    if (e == null)
      return;
    if (!Sr(e))
      return Na();
    for (var r of t.options)
      r.selected = e.includes(fi(r));
    return;
  }
  for (r of t.options) {
    var i = fi(r);
    if (Za(i, e)) {
      r.selected = !0;
      return;
    }
  }
  (!n || e !== void 0) && (t.selectedIndex = -1);
}
function wo(t) {
  var e = new MutationObserver(() => {
    Rs(t, t.__value);
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
  }), jr(() => {
    e.disconnect();
  });
}
function fi(t) {
  return "__value" in t ? t.__value : t.value;
}
const Eo = Symbol("is custom element"), ko = Symbol("is html"), $o = ji ? "link" : "LINK", To = ji ? "progress" : "PROGRESS";
function $n(t) {
  if (R) {
    var e = !1, n = () => {
      if (!e) {
        if (e = !0, t.hasAttribute("value")) {
          var r = t.value;
          qe(t, "value", null), t.value = r;
        }
        if (t.hasAttribute("checked")) {
          var i = t.checked;
          qe(t, "checked", null), t.checked = i;
        }
      }
    };
    t[Pi] = n, st(n), Qa();
  }
}
function Qn(t, e) {
  var n = qr(t);
  n.value === (n.value = // treat null and undefined the same for the initial value
  e ?? void 0) || // @ts-expect-error
  // `progress` elements always need their value set when it's `0`
  t.value === e && (e !== 0 || t.nodeName !== To) || (t.value = e ?? "");
}
function Ls(t, e) {
  var n = qr(t);
  n.checked !== (n.checked = // treat null and undefined the same for the initial value
  e ?? void 0) && (t.checked = e);
}
function Co(t, e) {
  e ? t.hasAttribute("selected") || t.setAttribute("selected", "") : t.removeAttribute("selected");
}
function qe(t, e, n, r) {
  var i = qr(t);
  R && (i[e] = t.getAttribute(e), e === "src" || e === "srcset" || e === "href" && t.nodeName === $o) || i[e] !== (i[e] = n) && (e === "loading" && (t[ma] = n), n == null ? t.removeAttribute(e) : typeof n != "string" && So(t).includes(e) ? t[e] = n : t.setAttribute(e, n));
}
function qr(t) {
  return (
    /** @type {Record<string | symbol, unknown>} **/
    /** @type {any} */
    t[Nn] ?? (t[Nn] = {
      [Eo]: t.nodeName.includes("-"),
      [ko]: t.namespaceURI === oa
    })
  );
}
var di = /* @__PURE__ */ new Map();
function So(t) {
  var e = t.getAttribute("is") || t.nodeName, n = di.get(e);
  if (n) return n;
  di.set(e, n = []);
  for (var r, i = t, s = Element.prototype; s !== i; ) {
    r = ua(i);
    for (var a in r)
      r[a].set && // better safe than sorry, we don't want spread attributes to mess with HTML content
      a !== "innerHTML" && a !== "textContent" && a !== "innerText" && n.push(a);
    i = Oi(i);
  }
  return n;
}
function sr(t, e) {
  return t === e || (t == null ? void 0 : t[Ct]) === e;
}
function Ur(t = {}, e, n, r) {
  var i = (
    /** @type {ComponentContext} */
    se.r
  ), s = (
    /** @type {Effect} */
    S
  );
  return ps(() => {
    var a, o;
    return Hr(() => {
      a = o, o = [], Wr(() => {
        sr(n(...o), t) || (e(t, ...o), a && sr(n(...a), t) && e(null, ...a));
      });
    }), () => {
      let l = s;
      for (; l !== i && l.parent !== null && l.parent.f & ur; )
        l = l.parent;
      const u = () => {
        o && sr(n(...o), t) && e(null, ...o);
      }, c = l.teardown;
      l.teardown = () => {
        u(), c == null || c();
      };
    };
  }), t;
}
function I(t, e, n, r) {
  var E;
  var i = !0, s = (n & ia) !== 0, a = (n & sa) !== 0, o = (
    /** @type {V} */
    r
  ), l = !0, u = (
    /** @type {Derived<V> | undefined} */
    void 0
  ), c = () => a && i ? (u ?? (u = /* @__PURE__ */ hn(
    /** @type {() => V} */
    r
  )), N(u)) : (l && (l = !1, o = a ? Wr(
    /** @type {() => V} */
    r
  ) : (
    /** @type {V} */
    r
  )), o);
  let h;
  if (s) {
    var f = Ct in t || Li in t;
    h = ((E = Tt(t, e)) == null ? void 0 : E.set) ?? (f && e in t ? (x) => t[e] = x : void 0);
  }
  var p, m = !1;
  s ? [p, m] = ja(() => (
    /** @type {V} */
    t[e]
  )) : p = /** @type {V} */
  t[e], p === void 0 && r !== void 0 && (p = c(), h && ($a(), h(p)));
  var g;
  if (g = () => {
    var x = (
      /** @type {V} */
      t[e]
    );
    return x === void 0 ? c() : (l = !0, x);
  }, (n & ra) === 0)
    return g;
  if (h) {
    var v = t.$$legacy;
    return (
      /** @type {() => V} */
      (function(x, k) {
        return arguments.length > 0 ? ((!k || v || m) && h(k ? g() : x), x) : g();
      })
    );
  }
  var b = !1, y = ((n & na) !== 0 ? hn : Ji)(() => (b = !1, g()));
  s && N(y);
  var _ = (
    /** @type {Effect} */
    S
  );
  return (
    /** @type {() => V} */
    (function(x, k) {
      if (arguments.length > 0) {
        const D = k ? N(y) : s ? _t(x) : x;
        return Ie(y, D), b = !0, o !== void 0 && (o = D), x;
      }
      return ot && b || (_.f & pe) !== 0 ? y.v : N(y);
    })
  );
}
function Ao(t) {
  return new Mo(t);
}
var nt, ye;
class Mo {
  /**
   * @param {ComponentConstructorOptions & {
   *  component: any;
   * }} options
   */
  constructor(e) {
    /** @type {any} */
    C(this, nt);
    /** @type {Record<string, any>} */
    C(this, ye);
    var s;
    var n = /* @__PURE__ */ new Map(), r = (a, o) => {
      var l = /* @__PURE__ */ as(o, !1, !1);
      return n.set(a, l), l;
    };
    const i = new Proxy(
      { ...e.props || {}, $$events: {} },
      {
        get(a, o) {
          return N(n.get(o) ?? r(o, Reflect.get(a, o)));
        },
        has(a, o) {
          return o === Li ? !0 : (N(n.get(o) ?? r(o, Reflect.get(a, o))), Reflect.has(a, o));
        },
        set(a, o, l) {
          return Ie(n.get(o) ?? r(o, l), l), Reflect.set(a, o, l);
        }
      }
    );
    $(this, ye, (e.hydrate ? po : As)(e.component, {
      target: e.target,
      anchor: e.anchor,
      props: i,
      context: e.context,
      intro: e.intro ?? !1,
      recover: e.recover,
      transformError: e.transformError
    })), (!((s = e == null ? void 0 : e.props) != null && s.$$host) || e.sync === !1) && M(), $(this, nt, i.$$events);
    for (const a of Object.keys(d(this, ye)))
      a === "$set" || a === "$destroy" || a === "$on" || Bn(this, a, {
        get() {
          return d(this, ye)[a];
        },
        /** @param {any} value */
        set(o) {
          d(this, ye)[a] = o;
        },
        enumerable: !0
      });
    d(this, ye).$set = /** @param {Record<string, any>} next */
    (a) => {
      Object.assign(i, a);
    }, d(this, ye).$destroy = () => {
      mo(d(this, ye));
    };
  }
  /** @param {Record<string, any>} props */
  $set(e) {
    d(this, ye).$set(e);
  }
  /**
   * @param {string} event
   * @param {(...args: any[]) => any} callback
   * @returns {any}
   */
  $on(e, n) {
    d(this, nt)[e] = d(this, nt)[e] || [];
    const r = (...i) => n.call(this, ...i);
    return d(this, nt)[e].push(r), () => {
      d(this, nt)[e] = d(this, nt)[e].filter(
        /** @param {any} fn */
        (i) => i !== r
      );
    };
  }
  $destroy() {
    d(this, ye).$destroy();
  }
}
nt = new WeakMap(), ye = new WeakMap();
let Ps;
typeof HTMLElement == "function" && (Ps = class extends HTMLElement {
  /**
   * @param {*} $$componentCtor
   * @param {*} $$slots
   * @param {ShadowRootInit | undefined} shadow_root_init
   */
  constructor(e, n, r) {
    super();
    /** The Svelte component constructor */
    F(this, "$$ctor");
    /** Slots */
    F(this, "$$s");
    /** @type {any} The Svelte component instance */
    F(this, "$$c");
    /** Whether or not the custom element is connected */
    F(this, "$$cn", !1);
    /** @type {Record<string, any>} Component props data */
    F(this, "$$d", {});
    /** `true` if currently in the process of reflecting component props back to attributes */
    F(this, "$$r", !1);
    /** @type {Record<string, CustomElementPropDefinition>} Props definition (name, reflected, type etc) */
    F(this, "$$p_d", {});
    /** @type {Record<string, EventListenerOrEventListenerObject[]>} Event listeners */
    F(this, "$$l", {});
    /** @type {Map<EventListenerOrEventListenerObject, Function>} Event listener unsubscribe functions */
    F(this, "$$l_u", /* @__PURE__ */ new Map());
    /** @type {any} The managed render effect for reflecting attributes */
    F(this, "$$me");
    /** @type {ShadowRoot | null} The ShadowRoot of the custom element */
    F(this, "$$shadowRoot", null);
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
          const a = Rr("slot");
          i !== "default" && (a.name = i), Y(s, a);
        };
      };
      if (await Promise.resolve(), !this.$$cn || this.$$c)
        return;
      const n = {}, r = No(this);
      for (const i of this.$$s)
        i in r && (i === "default" && !this.$$d.children ? (this.$$d.children = e(i), n.default = !0) : n[i] = e(i));
      for (const i of this.attributes) {
        const s = this.$$g_p(i.name);
        s in this.$$d || (this.$$d[s] = Ln(s, i.value, this.$$p_d, "toProp"));
      }
      for (const i in this.$$p_d)
        !(i in this.$$d) && this[i] !== void 0 && (this.$$d[i] = this[i], delete this[i]);
      this.$$c = Ao({
        component: this.$$ctor,
        target: this.$$shadowRoot || this,
        props: {
          ...this.$$d,
          $$slots: n,
          $$host: this
        }
      }), this.$$me = no(() => {
        Hr(() => {
          var i;
          this.$$r = !0;
          for (const s of jn(this.$$c)) {
            if (!((i = this.$$p_d[s]) != null && i.reflect)) continue;
            this.$$d[s] = this.$$c[s];
            const a = Ln(
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
    this.$$r || (e = this.$$g_p(e), this.$$d[e] = Ln(e, r, this.$$p_d, "toProp"), (i = this.$$c) == null || i.$set({ [e]: this.$$d[e] }));
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
    return jn(this.$$p_d).find(
      (n) => this.$$p_d[n].attribute === e || !this.$$p_d[n].attribute && n.toLowerCase() === e
    ) || e;
  }
});
function Ln(t, e, n, r) {
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
function No(t) {
  const e = {};
  return t.childNodes.forEach((n) => {
    e[
      /** @type {Element} node */
      n.slot || "default"
    ] = !0;
  }), e;
}
function Pe(t, e, n, r, i, s) {
  let a = class extends Ps {
    constructor() {
      super(t, n, i), this.$$p_d = e;
    }
    static get observedAttributes() {
      return jn(e).map(
        (o) => (e[o].attribute || o).toLowerCase()
      );
    }
  };
  return jn(e).forEach((o) => {
    Bn(a.prototype, o, {
      get() {
        return this.$$c && o in this.$$c ? this.$$c[o] : this.$$d[o];
      },
      set(l) {
        var h;
        l = Ln(o, l, e), this.$$d[o] = l;
        var u = this.$$c;
        if (u) {
          var c = (h = Tt(u, o)) == null ? void 0 : h.get;
          c ? u[o] = l : u.$set({ [o]: l });
        }
      }
    });
  }), r.forEach((o) => {
    Bn(a.prototype, o, {
      get() {
        var l;
        return (l = this.$$c) == null ? void 0 : l[o];
      }
    });
  }), t.element = /** @type {any} */
  a, a;
}
var Io = /* @__PURE__ */ K('<span class="lbl"> </span>'), Oo = /* @__PURE__ */ K('<label class="xi-slider svelte-8xo3l7"><!> <input type="range" class="svelte-8xo3l7"/> <span class="val svelte-8xo3l7"> </span></label>');
const Do = {
  hash: "svelte-8xo3l7",
  code: '.xi-slider.svelte-8xo3l7 {display:inline-flex;align-items:center;gap:0.5rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}input[type="range"].svelte-8xo3l7 {accent-color:var(--xi-accent, #3b82f6);vertical-align:middle;}.val.svelte-8xo3l7 {min-width:2.5em;text-align:right;font-variant-numeric:tabular-nums;}'
};
function Ro(t, e) {
  Ce(e, !0), Le(t, Do);
  let n = I(e, "value", 15, 0), r = I(e, "min", 7, 0), i = I(e, "max", 7, 100), s = I(e, "step", 7, 1), a = I(e, "label", 7, ""), o = I(e, "disabled", 7, !1);
  const l = e.$$host, u = (_) => l.dispatchEvent(new CustomEvent(_, { detail: { value: n() }, bubbles: !0, composed: !0 }));
  function c(_) {
    n(Number(_.target.value)), u("input");
  }
  function h(_) {
    n(Number(_.target.value)), u("change");
  }
  var f = {
    get value() {
      return n();
    },
    set value(_ = 0) {
      n(_), M();
    },
    get min() {
      return r();
    },
    set min(_ = 0) {
      r(_), M();
    },
    get max() {
      return i();
    },
    set max(_ = 100) {
      i(_), M();
    },
    get step() {
      return s();
    },
    set step(_ = 1) {
      s(_), M();
    },
    get label() {
      return a();
    },
    set label(_ = "") {
      a(_), M();
    },
    get disabled() {
      return o();
    },
    set disabled(_ = !1) {
      o(_), M();
    }
  }, p = Oo(), m = W(p);
  {
    var g = (_) => {
      var E = Io(), x = W(E, !0);
      V(E), te(() => $e(x, a())), Y(_, E);
    };
    en(m, (_) => {
      a() && _(g);
    });
  }
  var v = ge(m, 2);
  $n(v);
  var b = ge(v, 2), y = W(b, !0);
  return V(b), V(p), te(() => {
    qe(v, "min", r()), qe(v, "max", i()), qe(v, "step", s()), Qn(v, n()), v.disabled = o(), $e(y, n());
  }), J("input", v, c), J("change", v, h), Y(t, p), Se(f);
}
pt(["input", "change"]);
customElements.define("xi-slider", Pe(
  Ro,
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
var Lo = /* @__PURE__ */ K('<span class="lbl"> </span>'), Po = /* @__PURE__ */ K('<label class="xi-number svelte-1f6ykwb"><!> <input type="number" class="svelte-1f6ykwb"/></label>');
const jo = {
  hash: "svelte-1f6ykwb",
  code: ".xi-number.svelte-1f6ykwb {display:inline-flex;align-items:center;gap:0.5rem;font:var(--xi-font, 13px system-ui, sans-serif);color:var(--xi-fg, inherit);}input.svelte-1f6ykwb {width:6em;padding:0.15rem 0.3rem;color:var(--xi-fg, inherit);background:var(--xi-bg, #fff);border:1px solid var(--xi-border, #ccc);border-radius:var(--xi-radius, 3px);accent-color:var(--xi-accent, #3b82f6);}input.svelte-1f6ykwb:focus {outline:1px solid var(--xi-accent, #3b82f6);outline-offset:-1px;}"
};
function Bo(t, e) {
  Ce(e, !0), Le(t, jo);
  let n = I(e, "value", 15, 0), r = I(e, "min", 7), i = I(e, "max", 7), s = I(e, "step", 7, 1), a = I(e, "label", 7, ""), o = I(e, "disabled", 7, !1);
  const l = e.$$host, u = (y) => l.dispatchEvent(new CustomEvent(y, { detail: { value: n() }, bubbles: !0, composed: !0 })), c = (y) => y.target.value === "" ? null : Number(y.target.value);
  function h(y) {
    n(c(y)), u("input");
  }
  function f(y) {
    n(c(y)), u("change");
  }
  var p = {
    get value() {
      return n();
    },
    set value(y = 0) {
      n(y), M();
    },
    get min() {
      return r();
    },
    set min(y) {
      r(y), M();
    },
    get max() {
      return i();
    },
    set max(y) {
      i(y), M();
    },
    get step() {
      return s();
    },
    set step(y = 1) {
      s(y), M();
    },
    get label() {
      return a();
    },
    set label(y = "") {
      a(y), M();
    },
    get disabled() {
      return o();
    },
    set disabled(y = !1) {
      o(y), M();
    }
  }, m = Po(), g = W(m);
  {
    var v = (y) => {
      var _ = Lo(), E = W(_, !0);
      V(_), te(() => $e(E, a())), Y(y, _);
    };
    en(g, (y) => {
      a() && y(v);
    });
  }
  var b = ge(g, 2);
  return $n(b), V(m), te(() => {
    qe(b, "min", r()), qe(b, "max", i()), qe(b, "step", s()), Qn(b, n()), b.disabled = o();
  }), J("input", b, h), J("change", b, f), Y(t, m), Se(p);
}
pt(["input", "change"]);
customElements.define("xi-number", Pe(
  Bo,
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
var Ho = /* @__PURE__ */ K('<span class="lbl"> </span>'), Fo = /* @__PURE__ */ K('<label class="xi-toggle svelte-141rfru"><input type="checkbox" class="svelte-141rfru"/> <!></label>');
const Vo = {
  hash: "svelte-141rfru",
  code: ".xi-toggle.svelte-141rfru {display:inline-flex;align-items:center;gap:0.4rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);cursor:pointer;}input.svelte-141rfru {accent-color:var(--xi-accent, #3b82f6);}"
};
function zo(t, e) {
  Ce(e, !0), Le(t, Vo);
  let n = I(e, "value", 15, !1), r = I(e, "label", 7, ""), i = I(e, "disabled", 7, !1);
  const s = e.$$host;
  function a(f) {
    n(f.target.checked), s.dispatchEvent(new CustomEvent("change", { detail: { value: n() }, bubbles: !0, composed: !0 }));
  }
  var o = {
    get value() {
      return n();
    },
    set value(f = !1) {
      n(f), M();
    },
    get label() {
      return r();
    },
    set label(f = "") {
      r(f), M();
    },
    get disabled() {
      return i();
    },
    set disabled(f = !1) {
      i(f), M();
    }
  }, l = Fo(), u = W(l);
  $n(u);
  var c = ge(u, 2);
  {
    var h = (f) => {
      var p = Ho(), m = W(p, !0);
      V(p), te(() => $e(m, r())), Y(f, p);
    };
    en(c, (f) => {
      r() && f(h);
    });
  }
  return V(l), te(() => {
    Ls(u, n()), u.disabled = i();
  }), J("change", u, a), Y(t, l), Se(o);
}
pt(["change"]);
customElements.define("xi-toggle", Pe(
  zo,
  {
    value: { reflect: !0, type: "Boolean" },
    label: {},
    disabled: {}
  },
  [],
  [],
  { mode: "open" }
));
function js(t) {
  let e = t;
  if (typeof t == "string")
    try {
      e = JSON.parse(t);
    } catch {
      e = [];
    }
  return Array.isArray(e) ? e.map((n) => n && typeof n == "object" ? { value: n.value, label: n.label ?? String(n.value) } : { value: n, label: String(n) }) : [];
}
var Wo = /* @__PURE__ */ K('<span class="lbl"> </span>'), qo = /* @__PURE__ */ K('<label class="opt svelte-mtcxej"><input type="radio" class="svelte-mtcxej"/> <span> </span></label>'), Uo = /* @__PURE__ */ K('<div class="xi-radio svelte-mtcxej" role="radiogroup"><!> <!></div>');
const Yo = {
  hash: "svelte-mtcxej",
  code: ".xi-radio.svelte-mtcxej {display:inline-flex;align-items:center;gap:0.75rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}.opt.svelte-mtcxej {display:inline-flex;align-items:center;gap:0.25rem;cursor:pointer;}input.svelte-mtcxej {accent-color:var(--xi-accent, #3b82f6);}"
};
function Xo(t, e) {
  Ce(e, !0), Le(t, Yo);
  let n = I(e, "value", 15, ""), r = I(e, "options", 23, () => []), i = I(e, "label", 7, ""), s = I(e, "disabled", 7, !1), a = I(e, "name", 7, "xi-radio");
  const o = e.$$host, l = /* @__PURE__ */ Nr(() => js(r()));
  function u(g) {
    n(g), o.dispatchEvent(new CustomEvent("change", { detail: { value: n() }, bubbles: !0, composed: !0 }));
  }
  var c = {
    get value() {
      return n();
    },
    set value(g = "") {
      n(g), M();
    },
    get options() {
      return r();
    },
    set options(g = []) {
      r(g), M();
    },
    get label() {
      return i();
    },
    set label(g = "") {
      i(g), M();
    },
    get disabled() {
      return s();
    },
    set disabled(g = !1) {
      s(g), M();
    },
    get name() {
      return a();
    },
    set name(g = "xi-radio") {
      a(g), M();
    }
  }, h = Uo(), f = W(h);
  {
    var p = (g) => {
      var v = Wo(), b = W(v, !0);
      V(v), te(() => $e(b, i())), Y(g, v);
    };
    en(f, (g) => {
      i() && g(p);
    });
  }
  var m = ge(f, 2);
  return Is(m, 17, () => N(l), Ns, (g, v) => {
    var b = qo(), y = W(b);
    $n(y);
    var _ = ge(y, 2), E = W(_, !0);
    V(_), V(b), te(() => {
      qe(y, "name", a()), Qn(y, N(v).value), Ls(y, N(v).value === n()), y.disabled = s(), $e(E, N(v).label);
    }), J("change", y, () => u(N(v).value)), Y(g, b);
  }), V(h), Y(t, h), Se(c);
}
pt(["change"]);
customElements.define("xi-radio", Pe(
  Xo,
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
var Go = /* @__PURE__ */ K('<span class="lbl"> </span>'), Ko = /* @__PURE__ */ K("<option> </option>"), Jo = /* @__PURE__ */ K('<label class="xi-dropdown svelte-1wd9iqr"><!> <select class="svelte-1wd9iqr"></select></label>');
const Zo = {
  hash: "svelte-1wd9iqr",
  code: ".xi-dropdown.svelte-1wd9iqr {display:inline-flex;align-items:center;gap:0.5rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}select.svelte-1wd9iqr {padding:0.15rem 0.3rem;}"
};
function Qo(t, e) {
  Ce(e, !0), Le(t, Zo);
  let n = I(e, "value", 15, ""), r = I(e, "options", 23, () => []), i = I(e, "label", 7, ""), s = I(e, "disabled", 7, !1);
  const a = e.$$host, o = /* @__PURE__ */ Nr(() => js(r()));
  function l(g) {
    n(g.target.value), a.dispatchEvent(new CustomEvent("change", { detail: { value: n() }, bubbles: !0, composed: !0 }));
  }
  var u = {
    get value() {
      return n();
    },
    set value(g = "") {
      n(g), M();
    },
    get options() {
      return r();
    },
    set options(g = []) {
      r(g), M();
    },
    get label() {
      return i();
    },
    set label(g = "") {
      i(g), M();
    },
    get disabled() {
      return s();
    },
    set disabled(g = !1) {
      s(g), M();
    }
  }, c = Jo(), h = W(c);
  {
    var f = (g) => {
      var v = Go(), b = W(v, !0);
      V(v), te(() => $e(b, i())), Y(g, v);
    };
    en(h, (g) => {
      i() && g(f);
    });
  }
  var p = ge(h, 2);
  Is(p, 21, () => N(o), Ns, (g, v) => {
    var b = Ko(), y = W(b, !0);
    V(b);
    var _ = {};
    te(() => {
      Co(b, N(v).value === n()), $e(y, N(v).label), _ !== (_ = N(v).value) && (b.value = (b.__value = N(v).value) ?? "");
    }), Y(g, b);
  }), V(p);
  var m;
  return wo(p), V(c), te(() => {
    p.disabled = s(), m !== (m = n()) && (p.value = (p.__value = n()) ?? "", Rs(p, n()));
  }), J("change", p, l), Y(t, c), Se(u);
}
pt(["change"]);
customElements.define("xi-dropdown", Pe(
  Qo,
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
var el = /* @__PURE__ */ K('<input class="xi-text svelte-krpro1" type="text"/>');
const tl = {
  hash: "svelte-krpro1",
  code: ".xi-text.svelte-krpro1 {box-sizing:border-box;width:100%;font:var(--xi-font, 13px system-ui, sans-serif);padding:0.3em 0.5em;color:var(--xi-fg, inherit);background:var(--xi-bg, #fff);border:1px solid var(--xi-border, #ccc);border-radius:var(--xi-radius, 3px);}.xi-text.svelte-krpro1:focus {outline:1px solid var(--xi-accent, #3b82f6);outline-offset:-1px;}"
};
function nl(t, e) {
  Ce(e, !0), Le(t, tl);
  let n = I(e, "value", 15, ""), r = I(e, "placeholder", 7, ""), i = I(e, "disabled", 7, !1);
  const s = e.$$host, a = (h) => s.dispatchEvent(new CustomEvent(h, { detail: { value: n() }, bubbles: !0, composed: !0 }));
  function o(h) {
    n(h.target.value), a("input");
  }
  function l(h) {
    n(h.target.value), a("change");
  }
  var u = {
    get value() {
      return n();
    },
    set value(h = "") {
      n(h), M();
    },
    get placeholder() {
      return r();
    },
    set placeholder(h = "") {
      r(h), M();
    },
    get disabled() {
      return i();
    },
    set disabled(h = !1) {
      i(h), M();
    }
  }, c = el();
  return $n(c), te(() => {
    Qn(c, n()), qe(c, "placeholder", r()), c.disabled = i();
  }), J("input", c, o), J("change", c, l), Y(t, c), Se(u);
}
pt(["input", "change"]);
customElements.define("xi-text", Pe(nl, { value: { reflect: !0 }, placeholder: {}, disabled: {} }, [], [], { mode: "open" }));
var rl = /* @__PURE__ */ K('<span class="ico svelte-1v6o256" aria-hidden="true"> </span>'), il = /* @__PURE__ */ K("<button><!> <!></button>");
const sl = {
  hash: "svelte-1v6o256",
  code: ".xi-button.svelte-1v6o256 {display:inline-flex;align-items:center;gap:0.4em;font:var(--xi-font, 13px system-ui, sans-serif);padding:0.35em 0.9em;border:1px solid transparent;border-radius:var(--xi-radius, 3px);background:var(--xi-btn-bg, #3b82f6);color:var(--xi-btn-fg, #fff);cursor:pointer;}.xi-button.svelte-1v6o256:hover {background:var(--xi-btn-hover-bg, #2f6fe0);}.xi-button.svelte-1v6o256:focus-visible {outline:1px solid var(--xi-accent, #3b82f6);outline-offset:2px;}.xi-button.secondary.svelte-1v6o256 {background:var(--xi-btn-secondary-bg, #444);color:var(--xi-btn-secondary-fg, #fff);}.xi-button.secondary.svelte-1v6o256:hover {background:var(--xi-btn-secondary-hover-bg, #4f4f4f);}.xi-button.svelte-1v6o256:disabled {opacity:0.5;cursor:default;}.ico.svelte-1v6o256 {font-size:0.9em;line-height:1;}"
};
function al(t, e) {
  Ce(e, !0), Le(t, sl);
  let n = I(e, "secondary", 7, !1), r = I(e, "disabled", 7, !1), i = I(e, "icon", 7, "");
  const s = { add: "＋", play: "▶", "debug-stop": "■", stop: "■" }, a = /* @__PURE__ */ Nr(() => i() ? s[i()] ?? "" : "");
  var o = {
    get secondary() {
      return n();
    },
    set secondary(p = !1) {
      n(p), M();
    },
    get disabled() {
      return r();
    },
    set disabled(p = !1) {
      r(p), M();
    },
    get icon() {
      return i();
    },
    set icon(p = "") {
      i(p), M();
    }
  }, l = il();
  let u;
  var c = W(l);
  {
    var h = (p) => {
      var m = rl(), g = W(m, !0);
      V(m), te(() => $e(g, N(a))), Y(p, m);
    };
    en(c, (p) => {
      N(a) && p(h);
    });
  }
  var f = ge(c, 2);
  return Os(f, e, "default", {}), V(l), te(() => {
    u = Ds(l, 1, "xi-button svelte-1v6o256", null, u, { secondary: n() }), l.disabled = r();
  }), Y(t, l), Se(o);
}
customElements.define("xi-button", Pe(
  al,
  {
    secondary: { reflect: !0, type: "Boolean" },
    disabled: { reflect: !0, type: "Boolean" },
    icon: {}
  },
  ["default"],
  [],
  { mode: "open" }
));
var ol = /* @__PURE__ */ K("<span><!></span>");
const ll = {
  hash: "svelte-e9efnj",
  code: ".xi-badge.svelte-e9efnj {display:inline-flex;align-items:center;font:var(--xi-font, 11px system-ui, sans-serif);font-size:0.85em;line-height:1;padding:0.2em 0.55em;border-radius:var(--xi-radius, 3px);background:var(--xi-badge-bg, #4d4d4d);color:var(--xi-badge-fg, #fff);white-space:nowrap;}.xi-badge.counter.svelte-e9efnj {border-radius:999px;padding:0.2em 0.6em;}"
};
function ul(t, e) {
  Ce(e, !0), Le(t, ll);
  let n = I(e, "variant", 7, "");
  var r = {
    get variant() {
      return n();
    },
    set variant(o = "") {
      n(o), M();
    }
  }, i = ol();
  let s;
  var a = W(i);
  return Os(a, e, "default", {}), V(i), te(() => s = Ds(i, 1, "xi-badge svelte-e9efnj", null, s, { counter: n() === "counter" })), Y(t, i), Se(r);
}
customElements.define("xi-badge", Pe(ul, { variant: { reflect: !0 } }, ["default"], [], { mode: "open" }));
var cl = /* @__PURE__ */ K('<div class="xi-trace svelte-1m1z2sd"><span class="lbl"> </span> <canvas class="svelte-1m1z2sd"></canvas> <span class="val svelte-1m1z2sd"> </span></div>');
const fl = {
  hash: "svelte-1m1z2sd",
  code: ".xi-trace.svelte-1m1z2sd {display:inline-flex;align-items:center;gap:0.5rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}canvas.svelte-1m1z2sd {width:120px;height:28px;display:block;}.val.svelte-1m1z2sd {min-width:3em;text-align:right;font-variant-numeric:tabular-nums;}"
};
function dl(t, e) {
  Ce(e, !0), Le(t, fl);
  let n = I(e, "key", 7, ""), r = I(e, "label", 7, ""), i = I(e, "max", 7, 60);
  const s = e.$$host;
  let a, o = /* @__PURE__ */ Be(null), l = /* @__PURE__ */ Be(_t([]));
  function u() {
    if (!a) return;
    const _ = a.getContext && a.getContext("2d");
    if (!_) return;
    const E = a.width = a.clientWidth || 120, x = a.height = a.clientHeight || 28;
    if (_.clearRect(0, 0, E, x), N(l).length < 2) return;
    const k = Math.min(...N(l)), D = Math.max(...N(l)), z = D - k || 1;
    _.beginPath(), N(l).forEach((P, B) => {
      const q = B / (N(l).length - 1) * (E - 2) + 1, Q = x - 2 - (P - k) / z * (x - 4);
      B ? _.lineTo(q, Q) : _.moveTo(q, Q);
    }), _.strokeStyle = getComputedStyle(s).getPropertyValue("--xi-accent") || "#3b82f6", _.lineWidth = 1.5, _.stroke();
  }
  function c(_) {
    const E = _ && _[n()];
    E && (Ie(o, E.value, !0), typeof E.value == "number" && Number.isFinite(E.value) && (Ie(l, [...N(l), E.value].slice(-i()), !0), u()), s.dispatchEvent(new CustomEvent("sample", { detail: { value: E.value }, bubbles: !0, composed: !0 })));
  }
  Br(() => {
    s.update = c, Object.defineProperty(s, "latest", { get: () => N(o), configurable: !0 }), Object.defineProperty(s, "history", { get: () => N(l).slice(), configurable: !0 }), u();
  });
  const h = (_) => _ == null ? "—" : typeof _ == "number" ? Number.isInteger(_) ? _ : _.toFixed(3) : String(_);
  var f = {
    get key() {
      return n();
    },
    set key(_ = "") {
      n(_), M();
    },
    get label() {
      return r();
    },
    set label(_ = "") {
      r(_), M();
    },
    get max() {
      return i();
    },
    set max(_ = 60) {
      i(_), M();
    }
  }, p = cl(), m = W(p), g = W(m, !0);
  V(m);
  var v = ge(m, 2);
  Ur(v, (_) => a = _, () => a);
  var b = ge(v, 2), y = W(b, !0);
  return V(b), V(p), te(
    (_) => {
      $e(g, r() || n()), $e(y, _);
    },
    [() => h(N(o))]
  ), Y(t, p), Se(f);
}
customElements.define("xi-trace", Pe(dl, { key: {}, label: {}, max: { type: "Number" } }, [], [], { mode: "open" }));
function Bs() {
  return { scale: 1, panX: 0, panY: 0, imgW: 0, imgH: 0, viewW: 0, viewH: 0 };
}
function Yr(t, e, n) {
  return { x: (e - t.panX) / t.scale, y: (n - t.panY) / t.scale };
}
function hl(t, e, n) {
  return { x: t.panX + e * t.scale, y: t.panY + n * t.scale };
}
const vl = 0.05, pl = 64, ml = (t) => Math.max(vl, Math.min(pl, t));
function Er(t) {
  return !t.imgW || !t.imgH || !t.viewW || !t.viewH || (t.scale = Math.min(t.viewW / t.imgW, t.viewH / t.imgH) * 0.95, t.panX = (t.viewW - t.imgW * t.scale) / 2, t.panY = (t.viewH - t.imgH * t.scale) / 2), t;
}
function gl(t) {
  return t.scale = 1, t.panX = (t.viewW - t.imgW) / 2, t.panY = (t.viewH - t.imgH) / 2, t;
}
function Hs(t, e, n, r) {
  const { x: i, y: s } = Yr(t, e, n);
  return t.scale = ml(t.scale * r), t.panX = e - i * t.scale, t.panY = n - s * t.scale, t;
}
function _l(t, e, n) {
  return t.panX += e, t.panY += n, t;
}
var bl = /* @__PURE__ */ K('<canvas class="svelte-1yjweo0"></canvas>');
const xl = {
  hash: "svelte-1yjweo0",
  code: ":host {display:block;width:100%;height:100%;}canvas.svelte-1yjweo0 {width:100%;height:100%;display:block;background:var(--xi-viewer-bg, #111);cursor:grab;touch-action:none;image-rendering:pixelated;}canvas.svelte-1yjweo0:active {cursor:grabbing;}"
};
function yl(t, e) {
  Ce(e, !0), Le(t, xl);
  const n = e.$$host;
  let r;
  const i = Bs();
  let s = null, a = null;
  function o() {
    if (!r) return;
    const x = r.getContext("2d");
    x.imageSmoothingEnabled = !1, x.clearRect(0, 0, r.width, r.height), s && (x.setTransform(i.scale, 0, 0, i.scale, i.panX, i.panY), x.drawImage(s, 0, 0), x.setTransform(1, 0, 0, 1, 0, 0));
  }
  function l() {
    if (!r) return;
    const x = r.getBoundingClientRect();
    r.width = Math.max(1, Math.round(x.width)), r.height = Math.max(1, Math.round(x.height)), i.viewW = r.width, i.viewH = r.height, o();
  }
  function u(x, k) {
    n.dispatchEvent(new CustomEvent(x, { detail: k, bubbles: !0, composed: !0 }));
  }
  function c(x) {
    return !!x && typeof x != "string" && !("dataUrl" in x) && (typeof HTMLImageElement < "u" && x instanceof HTMLImageElement || typeof HTMLCanvasElement < "u" && x instanceof HTMLCanvasElement || typeof OffscreenCanvas < "u" && x instanceof OffscreenCanvas || typeof ImageBitmap < "u" && x instanceof ImageBitmap);
  }
  function h(x) {
    if (c(x)) {
      f(x);
      return;
    }
    const k = new Image();
    k.onload = () => f(k), k.src = typeof x == "string" ? x : x.dataUrl;
  }
  function f(x) {
    const k = !i.imgW;
    s = x, i.imgW = x.naturalWidth || x.width, i.imgH = x.naturalHeight || x.height, a = document.createElement("canvas"), a.width = i.imgW, a.height = i.imgH, a.getContext("2d").drawImage(x, 0, 0), k && Er(i), o();
  }
  function p(x) {
    if (!s) return;
    x.preventDefault();
    const k = r.getBoundingClientRect();
    Hs(i, x.clientX - k.left, x.clientY - k.top, x.deltaY < 0 ? 1.15 : 1 / 1.15), o(), u("viewchange", { scale: i.scale });
  }
  let m = null, g = !1;
  function v(x) {
    var k;
    s && (m = { x: x.clientX, y: x.clientY }, g = !1, (k = r.setPointerCapture) == null || k.call(r, x.pointerId));
  }
  function b(x) {
    if (!m) return;
    const k = x.clientX - m.x, D = x.clientY - m.y;
    (k || D) && (g = !0), _l(i, k, D), m = { x: x.clientX, y: x.clientY }, o();
  }
  function y(x) {
    m && !g && _(x), m = null;
  }
  function _(x) {
    if (!s || !a) return;
    const k = r.getBoundingClientRect(), D = Yr(i, x.clientX - k.left, x.clientY - k.top), z = Math.floor(D.x), P = Math.floor(D.y);
    let B = null;
    if (z >= 0 && P >= 0 && z < i.imgW && P < i.imgH) {
      const q = a.getContext("2d").getImageData(z, P, 1, 1).data;
      B = [q[0], q[1], q[2]];
    }
    u("pixelpick", { x: z, y: P, rgb: B });
  }
  Br(() => {
    n.setFrame = h, n.fit = () => {
      Er(i), o(), u("viewchange", { scale: i.scale });
    }, n.oneToOne = () => {
      gl(i), o(), u("viewchange", { scale: i.scale });
    }, l();
    const x = new ResizeObserver(l);
    return x.observe(r), () => x.disconnect();
  });
  var E = bl();
  Ur(E, (x) => r = x, () => r), Ss("wheel", E, p), J("pointerdown", E, v), J("pointermove", E, b), J("pointerup", E, y), Y(t, E), Se();
}
pt(["pointerdown", "pointermove", "pointerup"]);
customElements.define("xi-image-viewer", Pe(yl, {}, [], [], { mode: "open" }));
function wl() {
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
function El() {
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
      const a = s(t), o = s(e);
      i.strokeStyle = "#f59e0b", i.lineWidth = 1.5, i.strokeRect(Math.min(a.x, o.x), Math.min(a.y, o.y), Math.abs(o.x - a.x), Math.abs(o.y - a.y));
    }
  };
}
function kl() {
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
const kr = { point: wl, rect: El, polygon: kl };
function hu(t, e) {
  kr[t] = e;
}
function hi(t) {
  return kr[t] ? kr[t]() : null;
}
var $l = /* @__PURE__ */ K('<div class="xi-editor svelte-1qrk1oj"><div class="bar svelte-1qrk1oj"><span class="lbl"> </span> <span class="spacer svelte-1qrk1oj"></span> <button class="cancel svelte-1qrk1oj">Cancel</button> <button class="commit svelte-1qrk1oj">Commit</button></div> <canvas class="svelte-1qrk1oj"></canvas></div>');
const Tl = {
  hash: "svelte-1qrk1oj",
  code: ":host {display:block;width:100%;height:100%;}.xi-editor.svelte-1qrk1oj {display:flex;flex-direction:column;height:100%;}.bar.svelte-1qrk1oj {display:flex;align-items:center;gap:0.5rem;padding:0.35rem 0.5rem;font:13px system-ui, sans-serif;background:var(--xi-bar-bg, #1f2937);color:#e5e7eb;}.spacer.svelte-1qrk1oj {flex:1;}button.svelte-1qrk1oj {padding:0.2rem 0.7rem;border:0;border-radius:4px;cursor:pointer;font:inherit;}.commit.svelte-1qrk1oj {background:var(--xi-accent, #6366f1);color:#fff;}.cancel.svelte-1qrk1oj {background:#374151;color:#e5e7eb;}canvas.svelte-1qrk1oj {flex:1;width:100%;display:block;background:var(--xi-viewer-bg, #111);cursor:crosshair;touch-action:none;image-rendering:pixelated;}"
};
function Cl(t, e) {
  Ce(e, !0), Le(t, Tl);
  let n = I(e, "tool", 7, "rect"), r = I(e, "label", 7, "");
  const i = e.$$host;
  let s;
  const a = Bs();
  let o = null, l = hi(n());
  const u = (w) => hl(a, w.x, w.y);
  function c() {
    if (!s) return;
    const w = s.getContext("2d");
    w && (w.imageSmoothingEnabled = !1, w.setTransform(1, 0, 0, 1, 0, 0), w.clearRect(0, 0, s.width, s.height), o && (w.setTransform(a.scale, 0, 0, a.scale, a.panX, a.panY), w.drawImage(o, 0, 0), w.setTransform(1, 0, 0, 1, 0, 0)), l && l.draw(w, u));
  }
  function h() {
    if (!s) return;
    const w = s.getBoundingClientRect();
    s.width = Math.max(1, Math.round(w.width)), s.height = Math.max(1, Math.round(w.height)), a.viewW = s.width, a.viewH = s.height, c();
  }
  function f(w) {
    return !!w && typeof w != "string" && !("dataUrl" in w) && (typeof HTMLImageElement < "u" && w instanceof HTMLImageElement || typeof HTMLCanvasElement < "u" && w instanceof HTMLCanvasElement || typeof OffscreenCanvas < "u" && w instanceof OffscreenCanvas || typeof ImageBitmap < "u" && w instanceof ImageBitmap);
  }
  function p(w) {
    if (f(w)) {
      m(w);
      return;
    }
    const oe = new Image();
    oe.onload = () => m(oe), oe.src = typeof w == "string" ? w : w.dataUrl;
  }
  function m(w) {
    const oe = !a.imgW;
    o = w, a.imgW = w.naturalWidth || w.width, a.imgH = w.naturalHeight || w.height, oe && Er(a), c();
  }
  function g(w) {
    l = hi(w) || l, c();
  }
  const v = (w) => {
    const oe = s.getBoundingClientRect();
    return Yr(a, w.clientX - oe.left, w.clientY - oe.top);
  };
  function b(w) {
    l && (l.onDown(v(w)), c());
  }
  function y(w) {
    l && w.buttons && (l.onMove(v(w)), c());
  }
  function _(w) {
    l && (l.onUp(v(w)), c());
  }
  function E(w) {
    l && (l.onDbl(v(w)), c());
  }
  function x(w) {
    if (!o) return;
    w.preventDefault();
    const oe = s.getBoundingClientRect();
    Hs(a, w.clientX - oe.left, w.clientY - oe.top, w.deltaY < 0 ? 1.15 : 1 / 1.15), c();
  }
  function k() {
    !l || !l.done() || i.dispatchEvent(new CustomEvent("commit", {
      detail: { tool: l.type, result: l.result() },
      bubbles: !0,
      composed: !0
    }));
  }
  function D() {
    i.dispatchEvent(new CustomEvent("cancel", { detail: {}, bubbles: !0, composed: !0 }));
  }
  Br(() => {
    i.setFrame = p, i.setTool = g, i.getResult = () => l && l.done() ? l.result() : null, h();
    const w = new ResizeObserver(h);
    return w.observe(s), () => w.disconnect();
  });
  var z = {
    get tool() {
      return n();
    },
    set tool(w = "rect") {
      n(w), M();
    },
    get label() {
      return r();
    },
    set label(w = "") {
      r(w), M();
    }
  }, P = $l(), B = W(P), q = W(B), Q = W(q, !0);
  V(q);
  var mt = ge(q, 4), Lt = ge(mt, 2);
  V(B);
  var Ke = ge(B, 2);
  return Ur(Ke, (w) => s = w, () => s), V(P), te(() => $e(Q, r() || n())), J("click", mt, D), J("click", Lt, k), J("pointerdown", Ke, b), J("pointermove", Ke, y), J("pointerup", Ke, _), J("dblclick", Ke, E), Ss("wheel", Ke, x), Y(t, P), Se(z);
}
pt([
  "click",
  "pointerdown",
  "pointermove",
  "pointerup",
  "dblclick"
]);
customElements.define("xi-image-editor", Pe(Cl, { tool: {}, label: {} }, [], [], { mode: "open" }));
const Sl = 4003;
function vi(t, e) {
  const n = t && typeof t.code == "number" ? t.code : null, r = t && t.reason || "";
  return e && e.busy ? { busy: !0, code: n, reason: "single-client-busy" } : n === Sl || /single-client-busy/i.test(r) ? { busy: !0, code: n, reason: r || "single-client-busy" } : { busy: !1, code: n, reason: r };
}
class vu {
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
      } catch (u) {
        r(u);
        return;
      }
      i.binaryType = "arraybuffer", this.ws = i;
      let a = null;
      typeof i.on == "function" && i.on("unexpected-response", (u, c) => {
        const h = c && c.headers && c.headers["x-xi-reason"];
        a = {
          statusCode: c && c.statusCode,
          reason: h,
          busy: c && c.statusCode === 503 && h === "single-client-busy"
        };
      }), i.onmessage = (u) => this._onMessage(u);
      let o = !1;
      const l = (u) => {
        if (o) return;
        o = !0;
        const c = vi(u, a);
        this._emit("close", c);
        const h = new Error(c.busy ? "single-client-busy: another client owns the backend" : "connection failed before open");
        h.busy = c.busy, h.reason = c.reason, h.code = c.code, r(h);
      };
      i.onerror = () => {
        for (const { reject: u } of this._pending.values()) u(new Error("socket error"));
        this._pending.clear(), s || l(null);
      }, i.onclose = (u) => {
        for (const { reject: c } of this._pending.values()) c(new Error("socket closed"));
        if (this._pending.clear(), !s) {
          l(u);
          return;
        }
        this._emit("close", vi(u, a));
      }, i.onopen = async () => {
        s = !0, this._emit("open", { url: this.url });
        try {
          if (e.checkVersion) {
            const u = await this.cmd("version"), c = u && u.version;
            if (!(typeof e.checkVersion == "function" ? e.checkVersion(u) : e.checkVersion instanceof RegExp ? e.checkVersion.test(c) : c === e.checkVersion)) {
              r(new Error(`backend version mismatch: got ${c}`)), i.close();
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
      } catch (o) {
        this._pending.delete(r), s(o);
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
const pi = "__xi_vscode_state__";
function Al(t) {
  return t && typeof t == "object" && !Array.isArray(t) ? { ...t, type: "status" } : { type: "status", value: t };
}
function Ml(t = {}) {
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
        Promise.resolve().then(() => e.exchange(n, s.cmd)).then((a) => i.postMessage(Al(a), "*")).catch((a) => i.postMessage({ type: "status", error: String(a && a.message || a) }, "*"));
      }
    },
    getState() {
      try {
        const s = i.sessionStorage && i.sessionStorage.getItem(pi);
        return s ? JSON.parse(s) : null;
      } catch {
        return null;
      }
    },
    setState(s) {
      try {
        i.sessionStorage && i.sessionStorage.setItem(pi, JSON.stringify(s));
      } catch {
      }
      return s;
    }
  };
}
function pu(t = {}) {
  const e = t.win || globalThis, n = Ml(t), r = e.acquireVsCodeApi;
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
const Nl = {
  slider: "xi-slider",
  number: "xi-number",
  toggle: "xi-toggle",
  radio: "xi-radio",
  dropdown: "xi-dropdown",
  text: "xi-text"
};
function Fs(t, e, n) {
  const r = Nl[e.type] || "xi-number", i = t.createElement(r);
  e.label && i.setAttribute("label", e.label);
  for (const s of ["min", "max", "step"]) e[s] != null && i.setAttribute(s, String(e[s]));
  return i.addEventListener("change", (s) => n(s.detail.value, i)), i;
}
function Il(t, { section: e = "Config", tag: n = "control" } = {}) {
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
async function mu(t, e) {
  const { client: n, instance: r, sectionFilter: i } = e, s = t.ownerDocument || globalThis.document, a = await n.getInstanceDef(r) || {}, o = { ...a }, l = e.descriptor && e.descriptor.length ? e.descriptor : Il(a), u = [];
  t.innerHTML = "";
  for (const c of l) {
    if (i && !i(c)) continue;
    const h = s.createElement("section");
    if (h.className = "xi-section", h.dataset.tag = c.tag || "control", c.section) {
      const f = s.createElement("h3");
      f.className = "xi-section-title", f.textContent = c.section, h.appendChild(f);
    }
    for (const f of c.controls || []) {
      const p = Fs(s, f, async (g) => {
        o[f.key] = g;
        try {
          await n.setInstanceDef(r, { ...o });
        } catch {
        }
        t.dispatchEvent(new CustomEvent("xi-change", { detail: { key: f.key, value: g }, bubbles: !0 }));
      }), m = s.createElement("div");
      m.className = "xi-control", m.appendChild(p), h.appendChild(m), f.options != null && (p.options = f.options), f.key in o && (p.value = o[f.key]), u.push({ el: p, key: f.key });
    }
    t.appendChild(h);
  }
  return {
    // Re-read the def from the backend and push values back into the controls.
    async refresh() {
      const c = await n.getInstanceDef(r) || {};
      Object.assign(o, c);
      for (const { el: h, key: f } of u) f in o && (h.value = o[f]);
    },
    destroy() {
      t.innerHTML = "";
    }
  };
}
function Ol(t = []) {
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
function gu(t, e) {
  const { client: n, params: r, section: i = "Parameters", values: s = {} } = e, a = t.ownerDocument || globalThis.document, o = Ol(r), l = { ...s };
  for (const h of o) !(h.key in l) && h.default !== void 0 && (l[h.key] = h.default);
  const u = [];
  t.innerHTML = "";
  const c = a.createElement("section");
  if (c.className = "xi-section", c.dataset.tag = "param", i) {
    const h = a.createElement("h3");
    h.className = "xi-section-title", h.textContent = i, c.appendChild(h);
  }
  for (const h of o) {
    const f = Fs(a, h, async (m) => {
      l[h.key] = m;
      try {
        await n.setParam(h.key, m);
      } catch {
      }
      t.dispatchEvent(new CustomEvent("xi-param", { detail: { name: h.key, value: m }, bubbles: !0 }));
    }), p = a.createElement("div");
    p.className = "xi-control", p.appendChild(f), c.appendChild(p), h.options != null && (f.options = h.options), h.key in l && (f.value = l[h.key]), u.push({ el: f, key: h.key });
  }
  return t.appendChild(c), {
    setValues(h) {
      Object.assign(l, h);
      for (const { el: f, key: p } of u) p in l && (f.value = l[p]);
    },
    values() {
      return { ...l };
    },
    destroy() {
      t.innerHTML = "";
    }
  };
}
const mi = -(2n ** 63n), gi = 2n ** 63n - 1n, _i = 2n ** 64n - 1n, Dl = 64, Rl = 2146959360, Ll = 0;
class Pl {
  constructor(e, n) {
    this.code = e, this.data = n;
  }
}
class Vs extends Error {
  constructor(e) {
    super(e), this.name = new.target.name;
  }
}
class zs extends Vs {
}
class jl extends zs {
}
class it extends Vs {
}
class nn extends it {
}
class Bl extends it {
}
class bi extends it {
}
class xi extends it {
}
class Hl extends it {
}
new TextEncoder();
function yi(t, e) {
  const n = Number(t >> 32n & 0xffffffffn), r = Number(t & 0xffffffffn);
  e.u32(n), e.u32(r);
}
function wi(t, e) {
  if (e.u8(203), Number.isNaN(t)) {
    e.u32(Rl), e.u32(Ll);
    return;
  }
  const n = new DataView(new ArrayBuffer(8));
  n.setFloat64(0, t, !1), e.bytes(new Uint8Array(n.buffer));
}
function Fl(t, { maxDepth: e = Dl, allowExt: n = [] } = {}) {
  const r = new zl(t, e, new Set(n)), i = r.readValue(0);
  if (r.off !== r.b.length)
    throw new Bl(`${r.b.length - r.off} trailing byte(s) after the top-level value`);
  return i;
}
const Vl = new TextDecoder("utf-8", { fatal: !1 });
class zl {
  constructor(e, n, r) {
    this.b = e, this.dv = new DataView(e.buffer, e.byteOffset, e.byteLength), this.off = 0, this.maxDepth = n, this.allowExt = r;
  }
  _remaining() {
    return this.b.length - this.off;
  }
  _need(e) {
    if (e < 0 || this.off + e > this.b.length)
      throw new nn(`need ${e} byte(s) at offset ${this.off} but only ${this._remaining()} remain`);
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
    if (e > this.maxDepth) throw new bi(`nesting exceeded maxDepth=${this.maxDepth}`);
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
        throw new it("0xc1 is not a valid msgpack byte");
      default:
        throw new it(`unsupported msgpack byte 0x${n.toString(16)}`);
    }
  }
  _readStr(e) {
    const n = this._need(e);
    return Vl.decode(this.b.subarray(n, n + e));
  }
  _readBin(e) {
    const n = this._need(e);
    return this.b.slice(n, n + e);
  }
  _readExt(e) {
    const n = Number(this._int(1)), r = this._need(e);
    if (!this.allowExt.has(n)) throw new Hl(`ext type ${n} rejected (not in allowExt)`);
    return new Pl(n, this.b.slice(r, r + e));
  }
  _readArray(e, n) {
    if (e > this._remaining())
      throw new nn(`array claims ${e} element(s) but only ${this._remaining()} byte(s) remain`);
    const r = new Array(e);
    for (let i = 0; i < e; i++) r[i] = this.readValue(n + 1);
    return r;
  }
  _readMap(e, n) {
    if (e > Math.floor(this._remaining() / 2))
      throw new nn(`map claims ${e} pair(s) but only ${this._remaining()} byte(s) remain`);
    const r = {};
    for (let i = 0; i < e; i++) {
      const s = this.readValue(n + 1);
      if (typeof s != "string") throw new xi(`canonical map keys must be string, got ${typeof s}`);
      r[s] = this.readValue(n + 1);
    }
    return r;
  }
  // ---- transcode: read one value, re-emit canonically, preserving type ----
  transcodeValue(e, n) {
    if (e > this.maxDepth) throw new bi(`nesting exceeded maxDepth=${this.maxDepth}`);
    const r = this._u8();
    if (r <= 127) {
      je(BigInt(r), n);
      return;
    }
    if (r >= 224) {
      je(BigInt(r - 256), n);
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
      rn(this._strBytes(r & 31), n);
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
        ar(this._readBin(Number(this._uint(1))), n);
        return;
      case 197:
        ar(this._readBin(Number(this._uint(2))), n);
        return;
      case 198:
        ar(this._readBin(Number(this._uint(4))), n);
        return;
      case 202: {
        const i = this._need(4);
        wi(this.dv.getFloat32(i, !1), n);
        return;
      }
      case 203: {
        const i = this._need(8);
        wi(this.dv.getFloat64(i, !1), n);
        return;
      }
      case 204:
        je(this._uint(1), n);
        return;
      case 205:
        je(this._uint(2), n);
        return;
      case 206:
        je(this._uint(4), n);
        return;
      case 207:
        je(this._uint(8), n);
        return;
      case 208:
        je(this._int(1), n);
        return;
      case 209:
        je(this._int(2), n);
        return;
      case 210:
        je(this._int(4), n);
        return;
      case 211:
        je(this._int(8), n);
        return;
      case 217:
        rn(this._strBytes(Number(this._uint(1))), n);
        return;
      case 218:
        rn(this._strBytes(Number(this._uint(2))), n);
        return;
      case 219:
        rn(this._strBytes(Number(this._uint(4))), n);
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
        throw new it("0xc1 is not a valid msgpack byte");
      default:
        throw new it(`unsupported msgpack byte 0x${r.toString(16)}`);
    }
    throw new zs("the default canonical writer cannot emit ext types");
  }
  _strBytes(e) {
    const n = this._need(e);
    return this.b.slice(n, n + e);
  }
  _transArray(e, n, r) {
    if (e > this._remaining())
      throw new nn(`array claims ${e} element(s) but only ${this._remaining()} byte(s) remain`);
    r.u8(221), r.u32(e);
    for (let i = 0; i < e; i++) this.transcodeValue(n + 1, r);
  }
  _transMap(e, n, r) {
    if (e > Math.floor(this._remaining() / 2))
      throw new nn(`map claims ${e} pair(s) but only ${this._remaining()} byte(s) remain`);
    r.u8(223), r.u32(e);
    for (let i = 0; i < e; i++) {
      const s = this._u8();
      let a;
      if (s >= 160 && s <= 191) a = this._strBytes(s & 31);
      else if (s === 217) a = this._strBytes(Number(this._uint(1)));
      else if (s === 218) a = this._strBytes(Number(this._uint(2)));
      else if (s === 219) a = this._strBytes(Number(this._uint(4)));
      else throw new xi(`canonical map keys must be string (marker 0x${s.toString(16)})`);
      rn(a, r), this.transcodeValue(n + 1, r);
    }
  }
}
function je(t, e) {
  if (t >= mi && t <= gi) {
    e.u8(211);
    const n = t < 0n ? (1n << 64n) + t : t;
    yi(n, e);
  } else if (t > gi && t <= _i)
    e.u8(207), yi(t, e);
  else
    throw new jl(`integer ${t} is outside the canonical range [${mi}, ${_i}]`);
}
function rn(t, e) {
  e.u8(219), e.u32(t.length), e.bytes(t);
}
function ar(t, e) {
  e.u8(198), e.u32(t.length), e.bytes(t);
}
const Wl = {
  u8: { size: 1, read: (t, e) => t.getUint8(e) },
  u16: { size: 2, read: (t, e) => t.getUint16(e, !0) },
  i32: { size: 4, read: (t, e) => t.getInt32(e, !0) },
  f32: { size: 4, read: (t, e) => t.getFloat32(e, !0) },
  f64: { size: 8, read: (t, e) => t.getFloat64(e, !0) }
};
function Tn(t) {
  if (t instanceof DataView) return t;
  if (t instanceof ArrayBuffer) return new DataView(t);
  if (ArrayBuffer.isView(t)) return new DataView(t.buffer, t.byteOffset, t.byteLength);
  throw new Error("payload must be an ArrayBuffer / TypedArray / DataView");
}
function Ws(t, e, n) {
  const r = Wl[e];
  if (!r) throw new Error(`readScalars: unsupported dt "${e}"`);
  const i = Tn(t);
  if (i.byteLength < n * r.size) throw new Error("readScalars: payload shorter than count*elem_size");
  const s = new Float64Array(n);
  for (let a = 0; a < n; a++) s[a] = r.read(i, a * r.size);
  return s;
}
function qs(t, e) {
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
function or(t, e, n) {
  return t + (e - t) * n;
}
function Ei(t, e) {
  const n = Math.min(1, Math.max(0, e)) * (t.length - 1), r = Math.floor(n), i = n - r, s = t[r], a = t[Math.min(t.length - 1, r + 1)];
  return [Math.round(or(s[0], a[0], i)), Math.round(or(s[1], a[1], i)), Math.round(or(s[2], a[2], i))];
}
const ql = [
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
], Ul = [[0, 0, 131], [0, 60, 170], [5, 255, 255], [255, 255, 0], [250, 0, 0], [128, 0, 0]], ki = {
  gray: (t) => {
    const e = Math.round(Math.min(1, Math.max(0, t)) * 255);
    return [e, e, e];
  },
  viridis: (t) => Ei(ql, t),
  jet: (t) => Ei(Ul, t)
};
function Yl(t, e, n, r) {
  const i = Tn(t);
  if (i.byteLength < e * n * r) throw new Error("imageRGBA: payload shorter than w*h*c");
  const s = new Uint8ClampedArray(e * n * 4);
  for (let a = 0; a < e * n; a++) {
    const o = a * r, l = a * 4;
    if (r === 1) {
      const u = i.getUint8(o);
      s[l] = s[l + 1] = s[l + 2] = u, s[l + 3] = 255;
    } else if (r === 2) {
      const u = i.getUint8(o);
      s[l] = s[l + 1] = s[l + 2] = u, s[l + 3] = i.getUint8(o + 1);
    } else
      s[l] = i.getUint8(o), s[l + 1] = i.getUint8(o + 1), s[l + 2] = i.getUint8(o + 2), s[l + 3] = r >= 4 ? i.getUint8(o + 3) : 255;
  }
  return s;
}
function Xl(t, e, n, r, { range: i, colormap: s = "viridis" } = {}) {
  const a = Ws(t, r, e * n), { norm: o, min: l, max: u } = qs(a, i), c = ki[s] || ki.viridis, h = new Uint8ClampedArray(e * n * 4);
  for (let f = 0; f < e * n; f++) {
    const [p, m, g] = c(o[f]), v = f * 4;
    h[v] = p, h[v + 1] = m, h[v + 2] = g, h[v + 3] = 255;
  }
  return { rgba: h, min: l, max: u };
}
function Gl(t, { width: e, height: n, range: r, pad: i = 2 } = {}) {
  const s = t.length, { norm: a } = qs(t, r), o = Math.max(1, e - 2 * i), l = Math.max(1, n - 2 * i), u = new Array(s);
  for (let c = 0; c < s; c++) {
    const h = i + (s === 1 ? o / 2 : c / (s - 1) * o), f = i + (1 - a[c]) * l;
    u[c] = [h, f];
  }
  return u;
}
function Kl(t) {
  const e = t.w | 0, n = t.h | 0;
  return t.n ? t.n | 0 : n === 1 ? e : e === 1 ? n : Math.max(e, n);
}
function Jl(t, { w: e = 1, h: n = 1, vw: r = e, vh: i = n } = {}) {
  const s = r / e, a = i / n, o = (u, c) => [u * s, c * a], l = [];
  for (const u of t || []) {
    const c = u.color || "#39f";
    if (u.type === "point") {
      const [h, f] = o(u.x, u.y);
      l.push({ type: "point", x: h, y: f, r: u.r || 3, color: c });
    } else if (u.type === "box") {
      const [h, f] = o(u.x, u.y);
      l.push({ type: "box", x: h, y: f, w: u.w * s, h: u.h * a, color: c });
    } else u.type === "polyline" && l.push({ type: "polyline", points: (u.points || []).map(([h, f]) => o(h, f)), closed: !!u.closed, color: c });
  }
  return l;
}
function Zl(t) {
  const e = t && typeof t == "object" && !Array.isArray(t) ? t : t instanceof Map ? Object.fromEntries(t) : { value: t }, n = (r) => r === null || typeof r != "object" ? String(r) : JSON.stringify(r);
  return Object.entries(e).map(([r, i]) => [r, n(i)]);
}
function Ql(t, e = 16) {
  const n = Tn(t), r = Math.min(e, n.byteLength), i = [];
  for (let s = 0; s < r; s++) i.push(n.getUint8(s).toString(16).padStart(2, "0"));
  return i.join(" ") + (n.byteLength > r ? " …" : "");
}
function eu(t, e) {
  const n = e ? Tn(e).byteLength : 0;
  return { type: t && t.t || "unknown", size: n, preview: e ? Ql(e) : "" };
}
function tu(t = {}) {
  if (t.render && Us[t.render]) return t.render;
  if (t.render === "table") return "table";
  const e = t.t, n = t.dt;
  if (e === "xi/image") {
    if (n === "u8") return "image";
    if (n === "f32" || n === "u16" || n === "f64")
      return t.h === 1 || t.w === 1 ? "profile" : "heatmap";
  }
  return "hex";
}
function Mn(t, e, n) {
  const i = (t.ownerDocument || globalThis.document).createElement("canvas");
  return i.width = e, i.height = n, t.innerHTML = "", t.appendChild(i), i;
}
function $i(t, e, n, r) {
  const i = t.getContext && t.getContext("2d");
  if (!i || !i.putImageData) return !1;
  const s = t.ownerDocument || globalThis.document, a = i.createImageData ? i.createImageData(n, r) : new s.defaultView.ImageData(n, r);
  return a.data.set(e), i.putImageData(a, 0, 0), !0;
}
const Us = {
  image(t, { desc: e, payload: n }) {
    const { w: r, h: i, c: s = 1 } = e, a = Yl(n, r, i, s);
    return $i(Mn(t, r, i), a, r, i), { kind: "image", w: r, h: i, c: s };
  },
  heatmap(t, { desc: e, payload: n }) {
    const { w: r, h: i, dt: s = "f32" } = e, { rgba: a, min: o, max: l } = Xl(n, r, i, s, { range: e.range, colormap: e.colormap });
    return $i(Mn(t, r, i), a, r, i), { kind: "heatmap", w: r, h: i, min: o, max: l, colormap: e.colormap || "viridis" };
  },
  profile(t, { desc: e, payload: n }) {
    const r = Kl(e), i = Ws(n, e.dt || "f32", r), s = e.width || 240, a = e.height || 80, o = Gl(i, { width: s, height: a, range: e.range }), l = Mn(t, s, a), u = l.getContext && l.getContext("2d");
    return u && u.beginPath && (u.strokeStyle = e.color || "#39f", u.beginPath(), o.forEach(([c, h], f) => f ? u.lineTo(c, h) : u.moveTo(c, h)), u.stroke()), { kind: "profile", n: r, points: o };
  },
  overlay(t, { desc: e, refs: n = {} }) {
    const { w: r = 1, h: i = 1 } = e, s = e.width || r, a = e.height || i, o = Jl(e.shapes, { w: r, h: i, vw: s, vh: a }), l = Mn(t, s, a), u = l.getContext && l.getContext("2d");
    if (u && u.beginPath) {
      const c = e.image && n[e.image];
      c && u.drawImage && u.drawImage(c, 0, 0, s, a);
      for (const h of o)
        u.strokeStyle = h.color, u.fillStyle = h.color, u.beginPath(), h.type === "point" ? (u.arc(h.x, h.y, h.r, 0, Math.PI * 2), u.fill()) : h.type === "box" ? u.strokeRect(h.x, h.y, h.w, h.h) : h.type === "polyline" && (h.points.forEach(([f, p], m) => m ? u.lineTo(f, p) : u.moveTo(f, p)), h.closed && u.closePath(), u.stroke());
    }
    return { kind: "overlay", ops: o };
  },
  table(t, { desc: e, payload: n }) {
    let r = e.value;
    if (n)
      try {
        r = Fl(n instanceof Uint8Array ? n : new Uint8Array(Tn(n).buffer));
      } catch {
      }
    const i = Zl(r ?? e), s = t.ownerDocument || globalThis.document;
    t.innerHTML = "";
    const a = s.createElement("table");
    a.className = "xi-render-table";
    for (const [o, l] of i) {
      const u = s.createElement("tr"), c = s.createElement("th");
      c.textContent = o;
      const h = s.createElement("td");
      h.textContent = l, u.appendChild(c), u.appendChild(h), a.appendChild(u);
    }
    return t.appendChild(a), { kind: "table", rows: i };
  },
  hex(t, { desc: e, payload: n }) {
    const r = eu(e, n), i = t.ownerDocument || globalThis.document;
    t.innerHTML = "";
    const s = i.createElement("div");
    s.className = "xi-render-hex";
    const a = i.createElement("div");
    a.className = "xi-hex-type", a.textContent = r.type;
    const o = i.createElement("div");
    o.className = "xi-hex-size", o.textContent = `${r.size} bytes`;
    const l = i.createElement("code");
    return l.className = "xi-hex-preview", l.textContent = r.preview, s.appendChild(a), s.appendChild(o), s.appendChild(l), t.appendChild(s), { kind: "hex", ...r };
  }
};
function _u(t, e) {
  const n = tu(e.desc || {});
  return Us[n](t, e);
}
const nu = `
  :host { display:block; height:100%; box-sizing:border-box; background:#1e1e1e;
          border:1px solid #333; border-radius:6px; color:#ddd; overflow:hidden;
          font:13px/1.4 system-ui,sans-serif; }
  .hd { padding:4px 8px; font-size:11px; letter-spacing:.04em; text-transform:uppercase;
        color:#888; border-bottom:1px solid #2a2a2a; }
  .body { padding:8px; height:calc(100% - 24px); box-sizing:border-box; }
`;
function er(t, e) {
  return t.attachShadow({ mode: "open" }), t.shadowRoot.innerHTML = `<style>${nu}</style>
    <div class="hd">${e || ""}</div><div class="body"></div>`, t.shadowRoot.querySelector(".body");
}
const ru = (t, e) => t.config && t.config.title || e;
function Ys(t) {
  return t == null ? { kind: "none", label: "—", color: "#bbb" } : t <= -99e4 ? { kind: "sys", label: "SYS", color: "#c084fc" } : t > 0 ? { kind: "ok", label: t > 1 ? `OK${t}` : "OK", color: "#3ad17a" } : t < 0 ? { kind: "ng", label: t < -1 ? `NG${-t}` : "NG", color: "#ff5b5b" } : { kind: "na", label: "NA", color: "#ffb454" };
}
class iu extends HTMLElement {
  connectedCallback() {
    this.body = er(this, ru(this, "Verdict")), this.body.style.cssText = "display:flex;flex-direction:column;align-items:center;justify-content:center;gap:6px;text-align:center", this.big = document.createElement("div"), this.big.style.cssText = "font-weight:800;font-size:clamp(20px,7vw,72px);line-height:1", this.sub = document.createElement("div"), this.sub.style.cssText = "font-size:12px;color:#888;max-width:96%;overflow:hidden;text-overflow:ellipsis;white-space:nowrap", this.body.append(this.big, this.sub);
  }
  feed(e) {
    const n = e.result, r = Ys(n ? n.code : null);
    this.big.textContent = r.label, this.big.style.color = r.color, this.sub.textContent = n && n.msg ? n.msg : "";
  }
}
class su extends HTMLElement {
  connectedCallback() {
    var e, n;
    this.body = er(this, ((e = this.config) == null ? void 0 : e.title) || "Throughput"), this.windowSec = ((n = this.config) == null ? void 0 : n.windowSec) || 60, this.stamps = [], this.lastResult = -1, this.lastCompute = null, this.body.style.cssText = "display:flex;flex-direction:column;align-items:center;justify-content:center;gap:4px", this.big = document.createElement("div"), this.big.style.cssText = "font-size:clamp(18px,6vw,44px);font-weight:700;color:#9ad", this.sub = document.createElement("div"), this.sub.style.cssText = "font-size:12px;color:#888", this.body.append(this.big, this.sub), this.timer = setInterval(() => this.render(), 1e3);
  }
  disconnectedCallback() {
    this.timer && (clearInterval(this.timer), this.timer = 0);
  }
  feed(e) {
    const n = e.result;
    n && n.run_id != null && n.run_id !== this.lastResult && (this.lastResult = n.run_id, this.stamps.push(Date.now())), e.compute_ms != null && (this.lastCompute = e.compute_ms), this.render();
  }
  render() {
    var a, o;
    const e = Date.now(), n = e - this.windowSec * 1e3;
    for (; this.stamps.length && this.stamps[0] < n; ) this.stamps.shift();
    const r = this.stamps.length, i = r ? Math.max((e - this.stamps[0]) / 1e3, 1) : this.windowSec, s = r > 1 ? r / i * 60 : 0;
    this.big.textContent = `${s.toFixed(0)} /min`, this.sub.textContent = `${r} in ${this.windowSec}s` + (this.lastCompute != null ? ` · compute ${((o = (a = this.lastCompute).toFixed) == null ? void 0 : o.call(a, 1)) ?? this.lastCompute} ms` : "");
  }
}
class au extends HTMLElement {
  connectedCallback() {
    var e;
    this.body = er(this, ((e = this.config) == null ? void 0 : e.title) || "Yield"), this.ok = 0, this.ng = 0, this.last = -1, this.body.style.cssText = "display:flex;flex-direction:column;align-items:center;justify-content:center;gap:4px", this.big = document.createElement("div"), this.big.style.cssText = "font-size:clamp(18px,6vw,44px);font-weight:700", this.sub = document.createElement("div"), this.sub.style.cssText = "font-size:12px;color:#888", this.body.append(this.big, this.sub);
  }
  feed(e) {
    var s;
    const n = e.result;
    if (n && n.run_id != null && n.run_id !== this.last) {
      this.last = n.run_id;
      const a = Ys(n.code);
      a.kind === "ok" ? this.ok++ : a.kind === "ng" ? this.ng++ : a.kind === "na" && (this.na = (this.na || 0) + 1);
    }
    const r = this.ok + this.ng, i = r ? 100 * this.ok / r : 0;
    this.big.textContent = `${i.toFixed(1)}%`, this.big.style.color = i >= (((s = this.config) == null ? void 0 : s.warn) ?? 95) ? "#3ad17a" : "#ffb454", this.sub.textContent = `OK ${this.ok} / NG ${this.ng}` + (this.na ? ` · NA ${this.na}` : "");
  }
}
class ou extends HTMLElement {
  connectedCallback() {
    var e;
    this.body = er(this, ((e = this.config) == null ? void 0 : e.title) || "Dispatch groups"), this.body.style.cssText = "display:flex;flex-direction:column;gap:10px;padding:10px;overflow:auto", this.peak = {}, this.rows = {};
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
        const o = document.createElement("div");
        o.style.cssText = "display:flex;justify-content:space-between;font-size:12px";
        const l = document.createElement("span");
        l.style.fontWeight = "600";
        const u = document.createElement("span");
        u.style.color = "#888", o.append(l, u);
        const c = document.createElement("div");
        c.style.cssText = "display:flex;gap:3px;height:18px", a.append(o, c), this.body.appendChild(a), this.rows[r.name] = a = { row: a, name: l, meta: u, bar: c, cells: [] };
      }
      if (a.name.textContent = `${r.name}  ${s}/${i}`, a.name.style.color = s >= i ? "#3ad17a" : s > 0 ? "#9ad" : "#bbb", a.meta.textContent = `q ${r.queue_now ?? 0} · drop ${r.dropped ?? 0} · peak ${this.peak[r.name]}`, a.cells.length !== i) {
        a.bar.replaceChildren(), a.cells = [];
        for (let o = 0; o < i; o++) {
          const l = document.createElement("div");
          l.style.cssText = "flex:1 1 0;border-radius:3px;border:1px solid #333;min-width:6px", a.bar.appendChild(l), a.cells.push(l);
        }
      }
      a.cells.forEach((o, l) => {
        o.style.background = l < s ? "#3ad17a" : "#1a1a1a";
      });
    }
  }
}
const Xs = {
  verdict: iu,
  throughput: su,
  yield: au,
  groups: ou
};
for (const [t, e] of Object.entries(Xs)) customElements.define(`xi-card-${t}`, e);
const Xr = (t) => !!(t && t.card), Rt = (t) => !!(t && (t.dir === "row" || t.dir === "col") && Array.isArray(t.children) && t.children.length >= 1), Ge = (t) => !!(t && Array.isArray(t.tabs) && t.tabs.length >= 1 && t.tabs.every((e) => e && e.child)), Cn = () => ({ type: "verdict", bind: {}, config: { title: "(empty)" } });
function Gr(t) {
  const e = t.children.length;
  return (Array.isArray(t.weights) && t.weights.length === e ? t.weights.slice() : Array(e).fill(1)).map((r) => typeof r == "number" && r > 0 ? r : 1);
}
function lu(t) {
  const e = Gr(t), n = e.reduce((r, i) => r + i, 0) || 1;
  return e.map((r) => r / n);
}
function Gs(t, e) {
  return Ge(t) ? t.tabs[e].child : t.children[e];
}
function uu(t, e, n) {
  if (Ge(t)) {
    const i = t.tabs.slice();
    return i[e] = { ...i[e], child: n }, { ...t, tabs: i };
  }
  const r = t.children.slice();
  return r[e] = n, { ...t, children: r };
}
function $r(t, e, n = []) {
  if (Xr(t)) {
    e(t.card, n);
    return;
  }
  Rt(t) ? t.children.forEach((r, i) => $r(r, e, [...n, i])) : Ge(t) && t.tabs.forEach((r, i) => $r(r.child, e, [...n, i]));
}
function bu(t) {
  let e = 0;
  return $r(t, () => e++), e;
}
function cu(t, e) {
  let n = t;
  for (const r of e)
    if (Rt(n) || Ge(n)) n = Gs(n, r);
    else return;
  return n;
}
function Te(t, e, n) {
  if (e.length === 0) return n(t);
  const [r, ...i] = e;
  return uu(t, r, Te(Gs(t, r), i, n));
}
function xu(t, e, n, r = Cn()) {
  return Te(t, e, (i) => ({ dir: n === "col" ? "col" : "row", children: [i, { card: r }], weights: [1, 1] }));
}
function yu(t, e, n, r = Cn()) {
  if (n = n === "col" ? "col" : "row", e.length === 0) return { dir: n, children: [t, { card: r }], weights: [1, 1] };
  const i = e.slice(0, -1), s = e[e.length - 1], a = cu(t, i);
  return Rt(a) && a.dir === n ? Te(t, i, (o) => {
    const l = o.children.slice();
    l.splice(s + 1, 0, { card: r });
    const u = Gr(o);
    return u.splice(s + 1, 0, u[s]), { ...o, children: l, weights: u };
  }) : Te(t, e, (o) => ({ dir: n, children: [o, { card: r }], weights: [1, 1] }));
}
function wu(t, e) {
  if (e.length === 0) return { card: Cn() };
  const n = e.slice(0, -1), r = e[e.length - 1];
  return Te(t, n, (i) => {
    if (!Rt(i)) return i;
    const s = i.children.filter((o, l) => l !== r), a = Gr(i).filter((o, l) => l !== r);
    return s.length === 1 ? s[0] : { ...i, children: s, weights: a };
  });
}
function Eu(t, e, n) {
  return Te(t, e, () => ({ card: n }));
}
function ku(t, e, n) {
  return Te(t, e, (r) => Rt(r) ? { ...r, weights: n.slice() } : r);
}
function $u(t, e) {
  return Te(t, e, (n) => ({ tabs: [{ name: "Page 1", child: n }, { name: "Page 2", child: { card: Cn() } }], active: 0 }));
}
function Tu(t, e, n, r = { card: Cn() }) {
  return Te(t, e, (i) => Ge(i) ? { ...i, tabs: [...i.tabs, { name: n || `Page ${i.tabs.length + 1}`, child: r }], active: i.tabs.length } : i);
}
function Cu(t, e, n) {
  return Te(t, e, (r) => {
    if (!Ge(r)) return r;
    const i = r.tabs.filter((s, a) => a !== n);
    return i.length === 1 ? i[0].child : { ...r, tabs: i, active: Math.max(0, Math.min(r.active || 0, i.length - 1)) };
  });
}
function Su(t, e, n, r) {
  return Te(t, e, (i) => Ge(i) ? { ...i, tabs: i.tabs.map((s, a) => a === n ? { ...s, name: r } : s) } : i);
}
function Au(t, e, n) {
  return Te(t, e, (r) => Ge(r) ? { ...r, active: n } : r);
}
function Ti(t, e = "root") {
  return Xr(t) ? t.card.type ? [] : [`${e}: leaf has no card.type`] : Rt(t) ? t.children.flatMap((n, r) => Ti(n, `${e}.${r}`)) : Ge(t) ? t.tabs.flatMap((n, r) => Ti(n.child, `${e}.${n.name || r}`)) : [`${e}: node is not a leaf {card}, split {dir,children}, or tabs {tabs}`];
}
function Mu(t, { client: e, dashboard: n, pollStatsMs: r = 200 }) {
  const i = t.ownerDocument || globalThis.document, s = globalThis.requestAnimationFrame || ((v) => setTimeout(v, 16)), a = { run_id: -1, compute_ms: null, status: null, result: null, groups: [] };
  let o = [], l = 0;
  function u() {
    l || (l = s(() => {
      l = 0;
      for (const v of o)
        try {
          v.feed(a);
        } catch {
        }
    }));
  }
  function c(v) {
    const b = Xs[v.type], y = i.createElement(b ? `xi-card-${v.type}` : "div");
    return b || (y.textContent = `unknown card: ${v.type}`, y.style.cssText = "color:#f88;padding:8px"), y.binding = v.bind || {}, y.config = v.config || {}, y.style.minWidth = "0", y.style.minHeight = "0", y.style.overflow = "hidden", b && o.push(y), y;
  }
  function h(v) {
    let b = Math.min(v.active || 0, v.tabs.length - 1);
    const y = i.createElement("div");
    y.style.cssText = "display:flex;flex-direction:column;min-width:0;min-height:0;width:100%;height:100%";
    const _ = i.createElement("div");
    _.style.cssText = "display:flex;gap:2px;flex:0 0 auto;align-items:center;padding:2px 2px 0;overflow:auto";
    const E = i.createElement("div");
    E.style.cssText = "flex:1 1 0;min-width:0;min-height:0;position:relative";
    const x = [], k = [], D = () => {
      x.forEach((z, P) => {
        const B = P === b;
        z.style.background = B ? "#1e1e1e" : "#121212", z.style.color = B ? "#ddd" : "#888";
      }), k.forEach((z, P) => {
        z.style.display = P === b ? "" : "none";
      });
    };
    return v.tabs.forEach((z, P) => {
      const B = i.createElement("div");
      B.style.cssText = "padding:4px 11px;cursor:pointer;border-radius:6px 6px 0 0;border:1px solid #333;border-bottom:none;font:12px system-ui,sans-serif", B.textContent = z.name || `Page ${P + 1}`, B.onclick = () => {
        b = P, D();
      }, x.push(B), _.appendChild(B);
      const q = f(z.child);
      q.style.cssText += ";position:absolute;inset:0;min-width:0;min-height:0;border:1px solid #333;border-radius:0 6px 6px 6px;overflow:hidden", k.push(q), E.appendChild(q);
    }), D(), y.append(_, E), y;
  }
  function f(v) {
    if (Xr(v)) return c(v.card);
    if (Ge(v)) return h(v);
    if (!Rt(v)) {
      const E = i.createElement("div");
      return E.textContent = "bad layout node", E.style.color = "#f88", E;
    }
    const b = v.dir === "col", y = i.createElement("div");
    y.style.cssText = `display:flex;flex-direction:${b ? "column" : "row"};min-width:0;min-height:0;width:100%;height:100%`;
    const _ = lu(v);
    return v.children.forEach((E, x) => {
      const k = f(E);
      k.style.flex = `${_[x]} 1 0`, k.style.minWidth = "0", k.style.minHeight = "0", y.appendChild(k);
    }), y;
  }
  function p() {
    o = [], t.replaceChildren(), t.style.cssText += ";display:flex;min-width:0;min-height:0";
    const v = n && n.layout;
    if (!v) return;
    const b = f(v);
    b.style.flex = "1 1 0", b.style.minWidth = "0", b.style.minHeight = "0", t.appendChild(b), u();
  }
  const m = [
    e.onEvent((v) => {
      v.name === "run_finished" && v.data ? (typeof v.data.run_id == "number" && (a.run_id = v.data.run_id), typeof v.data.inspect_compute_us == "number" ? a.compute_ms = v.data.inspect_compute_us / 1e3 : typeof v.data.ms == "number" && (a.compute_ms = v.data.ms), u()) : v.name === "run_result" && v.data ? (a.result = v.data, u()) : v.name === "status" && (a.status = v.data, u());
    })
  ], g = setInterval(() => {
    e.cmd("dispatch_stats").then((v) => {
      v && Array.isArray(v.groups) && (a.groups = v.groups, u());
    }).catch(() => {
    });
  }, r);
  return p(), {
    setDashboard(v) {
      n = v, p();
    },
    state: a,
    destroy() {
      m.forEach((v) => v()), clearInterval(g), t.replaceChildren();
    }
  };
}
const Nu = [
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
  Sl as BUSY_CLOSE_CODE,
  Xs as CARDS,
  ki as COLORMAPS,
  Nl as CONTROL_TAGS,
  Us as RENDERERS,
  kr as TOOLS,
  Nu as XI_COMPONENTS,
  vu as XiClient,
  yu as addSibling,
  Tu as addTab,
  bu as countLeaves,
  Ml as createVsCodeApi,
  $r as eachLeaf,
  Cn as emptyCard,
  cu as getNode,
  Xl as heatmapRGBA,
  Yl as imageRGBA,
  Il as inferDescriptor,
  pu as installVsCodeShim,
  Xr as isLeaf,
  Rt as isSplit,
  Ge as isTabs,
  hi as makeTool,
  Mu as mountDashboard,
  mu as mountPanel,
  gu as mountParamPanel,
  qs as normalize,
  Jl as overlayOps,
  Ol as paramsToControls,
  tu as pickRenderer,
  Gl as profilePoints,
  Ws as readScalars,
  hu as registerTool,
  wu as removePane,
  Cu as removeTab,
  Su as renameTab,
  _u as renderDescriptor,
  Au as setActive,
  Eu as setCard,
  ku as setWeights,
  xu as splitLeaf,
  Zl as tableRows,
  Ti as validate,
  lu as weightsOf,
  $u as wrapInTabs
};
