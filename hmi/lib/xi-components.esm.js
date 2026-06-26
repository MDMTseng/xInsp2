var Ts = Object.defineProperty;
var Yr = (t) => {
  throw TypeError(t);
};
var Ss = (t, e, n) => e in t ? Ts(t, e, { enumerable: !0, configurable: !0, writable: !0, value: n }) : t[e] = n;
var B = (t, e, n) => Ss(t, typeof e != "symbol" ? e + "" : e, n), Jn = (t, e, n) => e.has(t) || Yr("Cannot " + n);
var f = (t, e, n) => (Jn(t, e, "read from private field"), n ? n.call(t) : e.get(t)), S = (t, e, n) => e.has(t) ? Yr("Cannot add the same private member more than once") : e instanceof WeakSet ? e.add(t) : e.set(t, n), $ = (t, e, n, r) => (Jn(t, e, "write to private field"), r ? r.call(t, n) : e.set(t, n), n), I = (t, e, n) => (Jn(t, e, "access private method"), n);
var li;
typeof window < "u" && ((li = window.__svelte ?? (window.__svelte = {})).v ?? (li.v = /* @__PURE__ */ new Set())).add("5");
const Cs = 1, Ms = 2, fi = 4, As = 8, Is = 16, Ns = 1, Os = 4, Rs = 8, Ls = 16, Ps = 2, ui = "[", br = "[!", Vr = "[?", yr = "]", Ut = {}, X = Symbol("uninitialized"), Ds = "http://www.w3.org/1999/xhtml", ci = !1;
var wr = Array.isArray, Hs = Array.prototype.indexOf, An = Array.prototype.includes, Bn = Array.from, In = Object.keys, Nn = Object.defineProperty, $t = Object.getOwnPropertyDescriptor, js = Object.getOwnPropertyDescriptors, Fs = Object.prototype, Ws = Array.prototype, di = Object.getPrototypeOf, qr = Object.isExtensible;
const zs = () => {
};
function Bs(t) {
  for (var e = 0; e < t.length; e++)
    t[e]();
}
function hi() {
  var t, e, n = new Promise((r, i) => {
    t = r, e = i;
  });
  return { promise: n, resolve: t, reject: e };
}
const Z = 2, Xt = 4, Yn = 8, vi = 1 << 24, Ie = 16, Oe = 32, nt = 64, nr = 128, Ee = 512, G = 1024, K = 2048, Be = 4096, re = 8192, pe = 16384, Nt = 32768, rr = 1 << 25, Gt = 65536, On = 1 << 17, Ys = 1 << 18, Ot = 1 << 19, Vs = 1 << 20, Fe = 1 << 25, At = 65536, Rn = 1 << 21, jt = 1 << 22, ct = 1 << 23, Tt = Symbol("$state"), pi = Symbol("legacy props"), qs = Symbol(""), kn = Symbol("attributes"), Us = Symbol("class"), Xs = Symbol("style"), en = Symbol("text"), gi = Symbol("form reset"), Vn = new class extends Error {
  constructor() {
    super(...arguments);
    B(this, "name", "StaleReactionError");
    B(this, "message", "The reaction that called `getAbortSignal()` was re-run or destroyed");
  }
}();
var ai;
const mi = (
  // We gotta write it like this because after downleveling the pure comment may end up in the wrong location
  !!((ai = globalThis.document) != null && ai.contentType) && /* @__PURE__ */ globalThis.document.contentType.includes("xml")
), xr = 3, _n = 8;
function Gs() {
  throw new Error("https://svelte.dev/e/async_derived_orphan");
}
function Ks(t, e, n) {
  throw new Error("https://svelte.dev/e/each_key_duplicate");
}
function Js(t) {
  throw new Error("https://svelte.dev/e/effect_in_teardown");
}
function Zs() {
  throw new Error("https://svelte.dev/e/effect_in_unowned_derived");
}
function Qs(t) {
  throw new Error("https://svelte.dev/e/effect_orphan");
}
function el() {
  throw new Error("https://svelte.dev/e/effect_update_depth_exceeded");
}
function tl() {
  throw new Error("https://svelte.dev/e/hydration_failed");
}
function nl(t) {
  throw new Error("https://svelte.dev/e/props_invalid_value");
}
function rl() {
  throw new Error("https://svelte.dev/e/state_descriptors_fixed");
}
function il() {
  throw new Error("https://svelte.dev/e/state_prototype_fixed");
}
function sl() {
  throw new Error("https://svelte.dev/e/state_unsafe_mutation");
}
function ll() {
  throw new Error("https://svelte.dev/e/svelte_boundary_reset_onerror");
}
function al() {
  console.warn("https://svelte.dev/e/derived_inert");
}
function qn(t) {
  console.warn("https://svelte.dev/e/hydration_mismatch");
}
function ol() {
  console.warn("https://svelte.dev/e/select_multiple_invalid_value");
}
function fl() {
  console.warn("https://svelte.dev/e/svelte_boundary_reset_noop");
}
let j = !1;
function Qe(t) {
  j = t;
}
let F;
function ge(t) {
  if (t === null)
    throw qn(), Ut;
  return F = t;
}
function Un() {
  return ge(/* @__PURE__ */ lt(F));
}
function V(t) {
  if (j) {
    if (/* @__PURE__ */ lt(F) !== null)
      throw qn(), Ut;
    F = t;
  }
}
function ul(t = 1) {
  if (j) {
    for (var e = t, n = F; e--; )
      n = /** @type {TemplateNode} */
      /* @__PURE__ */ lt(n);
    F = n;
  }
}
function Ln(t = !0) {
  for (var e = 0, n = F; ; ) {
    if (n.nodeType === _n) {
      var r = (
        /** @type {Comment} */
        n.data
      );
      if (r === yr) {
        if (e === 0) return n;
        e -= 1;
      } else (r === ui || r === br || // "[1", "[2", etc. for if blocks
      r[0] === "[" && !isNaN(Number(r.slice(1)))) && (e += 1);
    }
    var i = (
      /** @type {TemplateNode} */
      /* @__PURE__ */ lt(n)
    );
    t && n.remove(), n = i;
  }
}
function _i(t) {
  if (!t || t.nodeType !== _n)
    throw qn(), Ut;
  return (
    /** @type {Comment} */
    t.data
  );
}
function bi(t) {
  return t === this.v;
}
function cl(t, e) {
  return t != t ? e == e : t !== e || t !== null && typeof t == "object" || typeof t == "function";
}
function yi(t) {
  return !cl(t, this.v);
}
let dl = !1, ie = null;
function Kt(t) {
  ie = t;
}
function it(t, e = !1, n) {
  ie = {
    p: ie,
    i: !1,
    c: null,
    e: null,
    s: t,
    x: null,
    r: (
      /** @type {Effect} */
      M
    ),
    l: null
  };
}
function st(t) {
  var e = (
    /** @type {ComponentContext} */
    ie
  ), n = e.e;
  if (n !== null) {
    e.e = null;
    for (var r of n)
      Xi(r);
  }
  return t !== void 0 && (e.x = t), e.i = !0, ie = e.p, t ?? /** @type {T} */
  {};
}
function wi() {
  return !0;
}
let gt = [];
function xi() {
  var t = gt;
  gt = [], Bs(t);
}
function et(t) {
  if (gt.length === 0 && !an) {
    var e = gt;
    queueMicrotask(() => {
      e === gt && xi();
    });
  }
  gt.push(t);
}
function hl() {
  for (; gt.length > 0; )
    xi();
}
function Ei(t) {
  var e = M;
  if (e === null)
    return A.f |= ct, t;
  if ((e.f & Nt) === 0 && (e.f & Xt) === 0)
    throw t;
  ut(t, e);
}
function ut(t, e) {
  if (!(e !== null && (e.f & pe) !== 0)) {
    for (; e !== null; ) {
      if ((e.f & nr) !== 0) {
        if ((e.f & Nt) === 0)
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
const vl = -7169;
function Y(t, e) {
  t.f = t.f & vl | e;
}
function Er(t) {
  (t.f & Ee) !== 0 || t.deps === null ? Y(t, G) : Y(t, Be);
}
function ki(t) {
  if (t !== null)
    for (const e of t)
      (e.f & Z) === 0 || (e.f & At) === 0 || (e.f ^= At, ki(
        /** @type {Derived} */
        e.deps
      ));
}
function $i(t, e, n) {
  (t.f & K) !== 0 ? e.add(t) : (t.f & Be) !== 0 && n.add(t), ki(t.deps), Y(t, G);
}
let xn = !1;
function pl(t) {
  var e = xn;
  try {
    return xn = !1, [t(), xn];
  } finally {
    xn = e;
  }
}
function gl(t) {
  let e = 0, n = It(0), r;
  return () => {
    Mr() && (N(n), Nr(() => (e === 0 && (r = Pr(() => t(() => on(n)))), e += 1, () => {
      et(() => {
        e -= 1, e === 0 && (r == null || r(), r = void 0, on(n));
      });
    })));
  };
}
var ml = Gt | Ot;
function _l(t, e, n, r) {
  new bl(t, e, n, r);
}
var de, dn, be, yt, oe, ye, ne, he, Xe, wt, ot, Ft, hn, vn, Ge, Fn, W, Ti, Si, Ci, ir, $n, Tn, sr, lr;
class bl {
  /**
   * @param {TemplateNode} node
   * @param {BoundaryProps} props
   * @param {((anchor: Node) => void)} children
   * @param {((error: unknown) => unknown) | undefined} [transform_error]
   */
  constructor(e, n, r, i) {
    S(this, W);
    /** @type {Boundary | null} */
    B(this, "parent");
    B(this, "is_pending", !1);
    /**
     * API-level transformError transform function. Transforms errors before they reach the `failed` snippet.
     * Inherited from parent boundary, or defaults to identity.
     * @type {(error: unknown) => unknown}
     */
    B(this, "transform_error");
    /** @type {TemplateNode} */
    S(this, de);
    /** @type {TemplateNode | null} */
    S(this, dn, j ? F : null);
    /** @type {BoundaryProps} */
    S(this, be);
    /** @type {((anchor: Node) => void)} */
    S(this, yt);
    /** @type {Effect} */
    S(this, oe);
    /** @type {Effect | null} */
    S(this, ye, null);
    /** @type {Effect | null} */
    S(this, ne, null);
    /** @type {Effect | null} */
    S(this, he, null);
    /** @type {DocumentFragment | null} */
    S(this, Xe, null);
    S(this, wt, 0);
    S(this, ot, 0);
    S(this, Ft, !1);
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
    S(this, Ge, null);
    S(this, Fn, gl(() => ($(this, Ge, It(f(this, wt))), () => {
      $(this, Ge, null);
    })));
    var s;
    $(this, de, e), $(this, be, n), $(this, yt, (l) => {
      var a = (
        /** @type {Effect} */
        M
      );
      a.b = this, a.f |= nr, r(l);
    }), this.parent = /** @type {Effect} */
    M.b, this.transform_error = i ?? ((s = this.parent) == null ? void 0 : s.transform_error) ?? ((l) => l), $(this, oe, Or(() => {
      if (j) {
        const l = (
          /** @type {Comment} */
          f(this, dn)
        );
        Un();
        const a = l.data === br;
        if (l.data.startsWith(Vr)) {
          const u = JSON.parse(l.data.slice(Vr.length));
          I(this, W, Si).call(this, u);
        } else a ? I(this, W, Ci).call(this) : I(this, W, Ti).call(this);
      } else
        I(this, W, ir).call(this);
    }, ml)), j && $(this, de, F);
  }
  /**
   * Defer an effect inside a pending boundary until the boundary resolves
   * @param {Effect} effect
   */
  defer_effect(e) {
    $i(e, f(this, hn), f(this, vn));
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
  update_pending_count(e, n) {
    I(this, W, sr).call(this, e, n), $(this, wt, f(this, wt) + e), !(!f(this, Ge) || f(this, Ft)) && ($(this, Ft, !0), et(() => {
      $(this, Ft, !1), f(this, Ge) && Jt(f(this, Ge), f(this, wt));
    }));
  }
  get_effect_pending() {
    return f(this, Fn).call(this), N(
      /** @type {Source<number>} */
      f(this, Ge)
    );
  }
  /** @param {unknown} error */
  error(e) {
    if (!f(this, be).onerror && !f(this, be).failed)
      throw e;
    T != null && T.is_fork ? (f(this, ye) && T.skip_effect(f(this, ye)), f(this, ne) && T.skip_effect(f(this, ne)), f(this, he) && T.skip_effect(f(this, he)), T.oncommit(() => {
      I(this, W, lr).call(this, e);
    })) : I(this, W, lr).call(this, e);
  }
}
de = new WeakMap(), dn = new WeakMap(), be = new WeakMap(), yt = new WeakMap(), oe = new WeakMap(), ye = new WeakMap(), ne = new WeakMap(), he = new WeakMap(), Xe = new WeakMap(), wt = new WeakMap(), ot = new WeakMap(), Ft = new WeakMap(), hn = new WeakMap(), vn = new WeakMap(), Ge = new WeakMap(), Fn = new WeakMap(), W = new WeakSet(), Ti = function() {
  try {
    $(this, ye, xe(() => f(this, yt).call(this, f(this, de))));
  } catch (e) {
    this.error(e);
  }
}, /**
 * @param {unknown} error The deserialized error from the server's hydration comment
 */
Si = function(e) {
  const n = f(this, be).failed;
  n && $(this, he, xe(() => {
    n(
      f(this, de),
      () => e,
      () => () => {
      }
    );
  }));
}, Ci = function() {
  const e = f(this, be).pending;
  e && (this.is_pending = !0, $(this, ne, xe(() => e(f(this, de)))), et(() => {
    var n = $(this, Xe, document.createDocumentFragment()), r = We();
    n.append(r), $(this, ye, I(this, W, Tn).call(this, () => xe(() => f(this, yt).call(this, r)))), f(this, ot) === 0 && (f(this, de).before(n), $(this, Xe, null), Ct(
      /** @type {Effect} */
      f(this, ne),
      () => {
        $(this, ne, null);
      }
    ), I(this, W, $n).call(
      this,
      /** @type {Batch} */
      T
    ));
  }));
}, ir = function() {
  try {
    if (this.is_pending = this.has_pending_snippet(), $(this, ot, 0), $(this, wt, 0), $(this, ye, xe(() => {
      f(this, yt).call(this, f(this, de));
    })), f(this, ot) > 0) {
      var e = $(this, Xe, document.createDocumentFragment());
      Lr(f(this, ye), e);
      const n = (
        /** @type {(anchor: Node) => void} */
        f(this, be).pending
      );
      $(this, ne, xe(() => n(f(this, de))));
    } else
      I(this, W, $n).call(
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
$n = function(e) {
  this.is_pending = !1, e.transfer_effects(f(this, hn), f(this, vn));
}, /**
 * @template T
 * @param {() => T} fn
 */
Tn = function(e) {
  var n = M, r = A, i = ie;
  Ye(f(this, oe)), $e(f(this, oe)), Kt(f(this, oe).ctx);
  try {
    return dt.ensure(), e();
  } catch (s) {
    return Ei(s), null;
  } finally {
    Ye(n), $e(r), Kt(i);
  }
}, /**
 * Updates the pending count associated with the currently visible pending snippet,
 * if any, such that we can replace the snippet with content once work is done
 * @param {1 | -1} d
 * @param {Batch} batch
 */
sr = function(e, n) {
  var r;
  if (!this.has_pending_snippet()) {
    this.parent && I(r = this.parent, W, sr).call(r, e, n);
    return;
  }
  $(this, ot, f(this, ot) + e), f(this, ot) === 0 && (I(this, W, $n).call(this, n), f(this, ne) && Ct(f(this, ne), () => {
    $(this, ne, null);
  }), f(this, Xe) && (f(this, de).before(f(this, Xe)), $(this, Xe, null)));
}, /**
 * @param {unknown} error
 */
lr = function(e) {
  f(this, ye) && (se(f(this, ye)), $(this, ye, null)), f(this, ne) && (se(f(this, ne)), $(this, ne, null)), f(this, he) && (se(f(this, he)), $(this, he, null)), j && (ge(
    /** @type {TemplateNode} */
    f(this, dn)
  ), ul(), ge(Ln()));
  var n = f(this, be).onerror;
  let r = f(this, be).failed;
  var i = !1, s = !1;
  const l = () => {
    if (i) {
      fl();
      return;
    }
    i = !0, s && ll(), f(this, he) !== null && Ct(f(this, he), () => {
      $(this, he, null);
    }), I(this, W, Tn).call(this, () => {
      I(this, W, ir).call(this);
    });
  }, a = (o) => {
    try {
      s = !0, n == null || n(o, l), s = !1;
    } catch (u) {
      ut(u, f(this, oe) && f(this, oe).parent);
    }
    r && $(this, he, I(this, W, Tn).call(this, () => {
      try {
        return xe(() => {
          var u = (
            /** @type {Effect} */
            M
          );
          u.b = this, u.f |= nr, r(
            f(this, de),
            () => o,
            () => l
          );
        });
      } catch (u) {
        return ut(
          u,
          /** @type {Effect} */
          f(this, oe).parent
        ), null;
      }
    }));
  };
  et(() => {
    var o;
    try {
      o = this.transform_error(e);
    } catch (u) {
      ut(u, f(this, oe) && f(this, oe).parent);
      return;
    }
    o !== null && typeof o == "object" && typeof /** @type {any} */
    o.then == "function" ? o.then(
      a,
      /** @param {unknown} e */
      (u) => ut(u, f(this, oe) && f(this, oe).parent)
    ) : a(o);
  });
};
function yl(t, e, n, r) {
  const i = fn;
  var s = t.filter((g) => !g.settled), l = e.map(i);
  if (n.length === 0 && s.length === 0) {
    r(l);
    return;
  }
  var a = (
    /** @type {Effect} */
    M
  ), o = wl(), u = s.length === 1 ? s[0].promise : s.length > 1 ? Promise.all(s.map((g) => g.promise)) : null;
  function v(g) {
    if ((a.f & pe) === 0) {
      o();
      try {
        r([...l, ...g]);
      } catch (_) {
        ut(_, a);
      }
      Pn();
    }
  }
  var b = Mi();
  if (n.length === 0) {
    u.then(() => v([])).finally(b);
    return;
  }
  function d() {
    Promise.all(n.map((g) => /* @__PURE__ */ xl(g))).then(v).catch((g) => ut(g, a)).finally(b);
  }
  u ? u.then(() => {
    o(), d(), Pn();
  }) : d();
}
function wl() {
  var t = (
    /** @type {Effect} */
    M
  ), e = A, n = ie, r = (
    /** @type {Batch} */
    T
  );
  return function(s = !0) {
    Ye(t), $e(e), Kt(n), s && (t.f & pe) === 0 && (r == null || r.activate(), r == null || r.apply());
  };
}
function Pn(t = !0) {
  Ye(null), $e(null), Kt(null), t && (T == null || T.deactivate());
}
function Mi() {
  var t = (
    /** @type {Effect} */
    M
  ), e = t.b, n = (
    /** @type {Batch} */
    T
  ), r = !!(e != null && e.is_rendered());
  return e == null || e.update_pending_count(1, n), n.increment(r, t), () => {
    e == null || e.update_pending_count(-1, n), n.decrement(r, t);
  };
}
// @__NO_SIDE_EFFECTS__
function fn(t) {
  var e = Z | K;
  return M !== null && (M.f |= Ot), {
    ctx: ie,
    deps: null,
    effects: null,
    equals: bi,
    f: e,
    fn: t,
    reactions: null,
    rv: 0,
    v: (
      /** @type {V} */
      X
    ),
    wv: 0,
    parent: M,
    ac: null
  };
}
const tn = Symbol("obsolete");
// @__NO_SIDE_EFFECTS__
function xl(t, e, n) {
  let r = (
    /** @type {Effect | null} */
    M
  );
  r === null && Gs();
  var i = (
    /** @type {Promise<V>} */
    /** @type {unknown} */
    void 0
  ), s = It(
    /** @type {V} */
    X
  ), l = !A, a = /* @__PURE__ */ new Set();
  return Ll(() => {
    var g, _;
    var o = (
      /** @type {Effect} */
      M
    ), u = hi();
    i = u.promise;
    try {
      Promise.resolve(t()).then(u.resolve, (h) => {
        h !== Vn && u.reject(h);
      }).finally(Pn);
    } catch (h) {
      u.reject(h), Pn();
    }
    var v = (
      /** @type {Batch} */
      T
    );
    if (l) {
      if ((o.f & Nt) !== 0)
        var b = Mi();
      if (
        // boundary can be null if the async derived is inside an $effect.root not connected to the component render tree
        (g = r.b) != null && g.is_rendered()
      )
        (_ = v.async_deriveds.get(o)) == null || _.reject(tn);
      else
        for (const h of a.values())
          h.reject(tn);
      a.add(u), v.async_deriveds.set(o, u);
    }
    const d = (h, p = void 0) => {
      b == null || b(), a.delete(u), p !== tn && (v.activate(), p ? (s.f |= ct, Jt(s, p)) : ((s.f & ct) !== 0 && (s.f ^= ct), Jt(s, h)), v.deactivate());
    };
    u.promise.then(d, (h) => d(null, h || "unknown"));
  }), Ar(() => {
    for (const o of a)
      o.reject(tn);
  }), new Promise((o) => {
    function u(v) {
      function b() {
        v === i ? o(s) : u(i);
      }
      v.then(b, b);
    }
    u(i);
  });
}
// @__NO_SIDE_EFFECTS__
function Ai(t) {
  const e = /* @__PURE__ */ fn(t);
  return es(e), e;
}
// @__NO_SIDE_EFFECTS__
function Ii(t) {
  const e = /* @__PURE__ */ fn(t);
  return e.equals = yi, e;
}
function El(t) {
  var e = t.effects;
  if (e !== null) {
    t.effects = null;
    for (var n = 0; n < e.length; n += 1)
      se(
        /** @type {Effect} */
        e[n]
      );
  }
}
function kr(t) {
  var e, n = M, r = t.parent;
  if (!rt && r !== null && t.v !== X && // if it was never evaluated before, it's guaranteed to fail downstream, so we try to execute instead
  (r.f & (pe | re)) !== 0)
    return al(), t.v;
  Ye(r);
  try {
    t.f &= ~At, El(t), e = is(t);
  } finally {
    Ye(n);
  }
  return e;
}
function Ni(t) {
  var e = kr(t);
  if (!t.equals(e) && (t.wv = ns(), (!(T != null && T.is_fork) || t.deps === null) && (T !== null ? (T.capture(t, e, !0), ln == null || ln.capture(t, e, !0)) : t.v = e, t.deps === null))) {
    Y(t, G);
    return;
  }
  rt || (J !== null ? (Mr() || T != null && T.is_fork) && J.set(t, e) : Er(t));
}
function kl(t) {
  var e, n;
  if (t.effects !== null)
    for (const r of t.effects)
      (r.teardown || r.ac) && ((e = r.teardown) == null || e.call(r), (n = r.ac) == null || n.abort(Vn), r.fn !== null && (r.teardown = zs), r.ac = null, cn(r, 0), Rr(r));
}
function Oi(t) {
  if (t.effects !== null)
    for (const e of t.effects)
      e.teardown && e.fn !== null && Zt(e);
}
let Zn = null, Pt = null, T = null, ln = null, J = null, ar = null, an = !1, Qn = !1, Ht = null, Sn = null;
var Ur = 0;
let $l = 1;
var Wt, ft, xt, zt, Bt, Yt, Ke, Vt, fe, pn, Je, Ce, He, qt, Et, L, or, nn, fr, Ri, Li, Dt, Tl, rn;
const Wn = class Wn {
  constructor() {
    S(this, L);
    B(this, "id", $l++);
    /** True as soon as `#process` was called */
    S(this, Wt, !1);
    B(this, "linked", !0);
    /** @type {Batch | null} */
    S(this, ft, null);
    /** @type {Batch | null} */
    S(this, xt, null);
    /** @type {Map<Effect, ReturnType<typeof deferred<any>>>} */
    B(this, "async_deriveds", /* @__PURE__ */ new Map());
    /**
     * The current values of any signals that are updated in this batch.
     * Tuple format: [value, is_derived] (note: is_derived is false for deriveds, too, if they were overridden via assignment)
     * They keys of this map are identical to `this.#previous`
     * @type {Map<Value, [any, boolean]>}
     */
    B(this, "current", /* @__PURE__ */ new Map());
    /**
     * The values of any signals (sources and deriveds) that are updated in this batch _before_ those updates took place.
     * They keys of this map are identical to `this.#current`
     * @type {Map<Value, any>}
     */
    B(this, "previous", /* @__PURE__ */ new Map());
    /**
     * When the batch is committed (and the DOM is updated), we need to remove old branches
     * and append new ones by calling the functions added inside (if/each/key/etc) blocks
     * @type {Set<(batch: Batch) => void>}
     */
    S(this, zt, /* @__PURE__ */ new Set());
    /**
     * If a fork is discarded, we need to destroy any effects that are no longer needed
     * @type {Set<(batch: Batch) => void>}
     */
    S(this, Bt, /* @__PURE__ */ new Set());
    /**
     * The number of async effects that are currently in flight
     */
    S(this, Yt, 0);
    /**
     * Async effects that are currently in flight, _not_ inside a pending boundary
     * @type {Map<Effect, number>}
     */
    S(this, Ke, /* @__PURE__ */ new Map());
    /**
     * A deferred that resolves when the batch is committed, used with `settled()`
     * TODO replace with Promise.withResolvers once supported widely enough
     * @type {{ promise: Promise<void>, resolve: (value?: any) => void, reject: (reason: unknown) => void } | null}
     */
    S(this, Vt, null);
    /**
     * The root effects that need to be flushed
     * @type {Effect[]}
     */
    S(this, fe, []);
    /**
     * Effects created while this batch was active.
     * @type {Effect[]}
     */
    S(this, pn, []);
    /**
     * Deferred effects (which run after async work has completed) that are DIRTY
     * @type {Set<Effect>}
     */
    S(this, Je, /* @__PURE__ */ new Set());
    /**
     * Deferred effects that are MAYBE_DIRTY
     * @type {Set<Effect>}
     */
    S(this, Ce, /* @__PURE__ */ new Set());
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
    B(this, "is_fork", !1);
    S(this, Et, !1);
    Pt === null ? Zn = Pt = this : ($(Pt, xt, this), $(this, ft, Pt)), Pt = this;
  }
  /**
   * Add an effect to the #skipped_branches map and reset its children
   * @param {Effect} effect
   */
  skip_effect(e) {
    f(this, He).has(e) || f(this, He).set(e, { d: [], m: [] }), f(this, qt).delete(e);
  }
  /**
   * Remove an effect from the #skipped_branches map and reschedule
   * any tracked dirty/maybe_dirty child effects
   * @param {Effect} effect
   * @param {(e: Effect) => void} callback
   */
  unskip_effect(e, n = (r) => this.schedule(r)) {
    var r = f(this, He).get(e);
    if (r) {
      f(this, He).delete(e);
      for (var i of r.d)
        Y(i, K), n(i);
      for (i of r.m)
        Y(i, Be), n(i);
    }
    f(this, qt).add(e);
  }
  /**
   * Associate a change to a given source with the current
   * batch, noting its previous and current values
   * @param {Value} source
   * @param {any} value
   * @param {boolean} [is_derived]
   */
  capture(e, n, r = !1) {
    e.v !== X && !this.previous.has(e) && this.previous.set(e, e.v), (e.f & ct) === 0 && (this.current.set(e, [n, r]), J == null || J.set(e, n)), this.is_fork || (e.v = n);
  }
  activate() {
    T = this;
  }
  deactivate() {
    T = null, J = null;
  }
  flush() {
    try {
      Qn = !0, T = this, I(this, L, nn).call(this);
    } finally {
      Ur = 0, ar = null, Ht = null, Sn = null, Qn = !1, T = null, J = null, St.clear();
    }
  }
  discard() {
    var e;
    for (const n of f(this, Bt)) n(this);
    f(this, Bt).clear();
    for (const n of this.async_deriveds.values())
      n.reject(tn);
    I(this, L, rn).call(this), (e = f(this, Vt)) == null || e.resolve();
  }
  /**
   * @param {Effect} effect
   */
  register_created_effect(e) {
    f(this, pn).push(e);
  }
  /**
   * @param {boolean} blocking
   * @param {Effect} effect
   */
  increment(e, n) {
    if ($(this, Yt, f(this, Yt) + 1), e) {
      let r = f(this, Ke).get(n) ?? 0;
      f(this, Ke).set(n, r + 1);
    }
  }
  /**
   * @param {boolean} blocking
   * @param {Effect} effect
   */
  decrement(e, n) {
    if ($(this, Yt, f(this, Yt) - 1), e) {
      let r = f(this, Ke).get(n) ?? 0;
      r === 1 ? f(this, Ke).delete(n) : f(this, Ke).set(n, r - 1);
    }
    f(this, Et) || ($(this, Et, !0), et(() => {
      $(this, Et, !1), this.linked && this.flush();
    }));
  }
  /**
   * @param {Set<Effect>} dirty_effects
   * @param {Set<Effect>} maybe_dirty_effects
   */
  transfer_effects(e, n) {
    for (const r of e)
      f(this, Je).add(r);
    for (const r of n)
      f(this, Ce).add(r);
    e.clear(), n.clear();
  }
  /** @param {(batch: Batch) => void} fn */
  oncommit(e) {
    f(this, zt).add(e);
  }
  /** @param {(batch: Batch) => void} fn */
  ondiscard(e) {
    f(this, Bt).add(e);
  }
  settled() {
    return (f(this, Vt) ?? $(this, Vt, hi())).promise;
  }
  static ensure() {
    if (T === null) {
      const e = T = new Wn();
      !Qn && !an && et(() => {
        f(e, Wt) || e.flush();
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
  schedule(e) {
    var i;
    if (ar = e, (i = e.b) != null && i.is_pending && (e.f & (Xt | Yn | vi)) !== 0 && (e.f & Nt) === 0) {
      e.b.defer_effect(e);
      return;
    }
    for (var n = e; n.parent !== null; ) {
      n = n.parent;
      var r = n.f;
      if (Ht !== null && n === M && (A === null || (A.f & Z) === 0))
        return;
      if ((r & (nt | Oe)) !== 0) {
        if ((r & G) === 0)
          return;
        n.f ^= G;
      }
    }
    f(this, fe).push(n);
  }
};
Wt = new WeakMap(), ft = new WeakMap(), xt = new WeakMap(), zt = new WeakMap(), Bt = new WeakMap(), Yt = new WeakMap(), Ke = new WeakMap(), Vt = new WeakMap(), fe = new WeakMap(), pn = new WeakMap(), Je = new WeakMap(), Ce = new WeakMap(), He = new WeakMap(), qt = new WeakMap(), Et = new WeakMap(), L = new WeakSet(), or = function() {
  if (this.is_fork) return !0;
  for (const r of f(this, Ke).keys()) {
    for (var e = r, n = !1; e.parent !== null; ) {
      if (f(this, He).has(e)) {
        n = !0;
        break;
      }
      e = e.parent;
    }
    if (!n)
      return !0;
  }
  return !1;
}, nn = function() {
  var o, u, v, b;
  $(this, Wt, !0), Ur++ > 1e3 && (I(this, L, rn).call(this), Sl());
  for (const d of f(this, Je))
    f(this, Ce).delete(d), Y(d, K), this.schedule(d);
  for (const d of f(this, Ce))
    Y(d, Be), this.schedule(d);
  const e = f(this, fe);
  $(this, fe, []), this.apply();
  var n = Ht = [], r = [], i = Sn = [];
  for (const d of e)
    try {
      I(this, L, fr).call(this, d, n, r);
    } catch (g) {
      throw Hi(d), I(this, L, or).call(this) || this.discard(), g;
    }
  if (T = null, i.length > 0) {
    var s = Wn.ensure();
    for (const d of i)
      s.schedule(d);
  }
  if (Ht = null, Sn = null, I(this, L, or).call(this)) {
    I(this, L, Dt).call(this, r), I(this, L, Dt).call(this, n);
    for (const [d, g] of f(this, He))
      Di(d, g);
    i.length > 0 && /** @type {unknown} */
    I(o = T, L, nn).call(o);
    return;
  }
  const l = I(this, L, Ri).call(this);
  if (l) {
    I(this, L, Dt).call(this, r), I(this, L, Dt).call(this, n), I(u = l, L, Li).call(u, this);
    return;
  }
  f(this, Je).clear(), f(this, Ce).clear();
  for (const d of f(this, zt)) d(this);
  f(this, zt).clear(), ln = this, Xr(r), Xr(n), ln = null, (v = f(this, Vt)) == null || v.resolve();
  var a = (
    /** @type {Batch | null} */
    /** @type {unknown} */
    T
  );
  if (f(this, Yt) === 0 && (f(this, fe).length === 0 || a !== null) && I(this, L, rn).call(this), f(this, fe).length > 0)
    if (a !== null) {
      const d = a;
      f(d, fe).push(...f(this, fe).filter((g) => !f(d, fe).includes(g)));
    } else
      a = this;
  a !== null && I(b = a, L, nn).call(b);
}, /**
 * Traverse the effect tree, executing effects or stashing
 * them for later execution as appropriate
 * @param {Effect} root
 * @param {Effect[]} effects
 * @param {Effect[]} render_effects
 */
fr = function(e, n, r) {
  e.f ^= G;
  for (var i = e.first; i !== null; ) {
    var s = i.f, l = (s & (Oe | nt)) !== 0, a = l && (s & G) !== 0, o = a || (s & re) !== 0 || f(this, He).has(i);
    if (!o && i.fn !== null) {
      l ? i.f ^= G : (s & Xt) !== 0 ? n.push(i) : bn(i) && ((s & Ie) !== 0 && f(this, Ce).add(i), Zt(i));
      var u = i.first;
      if (u !== null) {
        i = u;
        continue;
      }
    }
    for (; i !== null; ) {
      var v = i.next;
      if (v !== null) {
        i = v;
        break;
      }
      i = i.parent;
    }
  }
}, Ri = function() {
  for (var e = f(this, ft); e !== null; ) {
    if (!e.is_fork) {
      for (const [n, [, r]] of this.current)
        if (e.current.has(n) && !r)
          return e;
    }
    e = f(e, ft);
  }
  return null;
}, /**
 * @param {Batch} batch
 */
Li = function(e) {
  var r;
  for (const [i, s] of e.current)
    !this.previous.has(i) && e.previous.has(i) && this.previous.set(i, e.previous.get(i)), this.current.set(i, s);
  for (const [i, s] of e.async_deriveds) {
    const l = this.async_deriveds.get(i);
    l && s.promise.then(l.resolve).catch(l.reject);
  }
  e.async_deriveds.clear(), this.transfer_effects(f(e, Je), f(e, Ce));
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
          l & (jt | Ie) && !this.async_deriveds.has(a) && (f(this, Ce).delete(a), Y(a, K), this.schedule(a));
        }
      }
  };
  for (const i of this.current.keys())
    n(i);
  this.oncommit(() => e.discard()), I(r = e, L, rn).call(r), T = this, I(this, L, nn).call(this);
}, /**
 * @param {Effect[]} effects
 */
Dt = function(e) {
  for (var n = 0; n < e.length; n += 1)
    $i(e[n], f(this, Je), f(this, Ce));
}, Tl = function() {
  var b;
  for (let d = Zn; d !== null; d = f(d, xt)) {
    var e = d.id < this.id, n = [];
    for (const [g, [_, h]] of this.current) {
      if (d.current.has(g)) {
        var r = (
          /** @type {[any, boolean]} */
          d.current.get(g)[0]
        );
        if (e && _ !== r)
          d.current.set(g, [_, h]);
        else
          continue;
      }
      n.push(g);
    }
    if (e)
      for (const [g, _] of this.async_deriveds) {
        const h = d.async_deriveds.get(g);
        h && _.promise.then(h.resolve).catch(h.reject);
      }
    var i = [...d.current.keys()].filter(
      (g) => !/** @type {[any, boolean]} */
      d.current.get(g)[1]
    );
    if (!(!f(d, Wt) || i.length === 0)) {
      var s = i.filter((g) => !this.current.has(g));
      if (s.length === 0)
        e && d.discard();
      else if (n.length > 0) {
        if (e)
          for (const g of f(this, qt))
            d.unskip_effect(g, (_) => {
              var h;
              (_.f & (Ie | jt)) !== 0 ? d.schedule(_) : I(h = d, L, Dt).call(h, [_]);
            });
        d.activate();
        var l = /* @__PURE__ */ new Set(), a = /* @__PURE__ */ new Map();
        for (var o of n)
          Pi(o, s, l, a);
        a = /* @__PURE__ */ new Map();
        var u = [...d.current].filter(([g, _]) => {
          const h = this.current.get(g);
          return h ? h[0] !== _[0] || h[1] !== _[1] : !0;
        }).map(([g]) => g);
        if (u.length > 0)
          for (const g of f(this, pn))
            (g.f & (pe | re | On)) === 0 && $r(g, u, a) && ((g.f & (jt | Ie)) !== 0 ? (Y(g, K), d.schedule(g)) : f(d, Je).add(g));
        if (f(d, fe).length > 0 && !f(d, Et)) {
          d.apply();
          for (var v of f(d, fe))
            I(b = d, L, fr).call(b, v, [], []);
          $(d, fe, []);
        }
        d.deactivate();
      }
    }
  }
}, rn = function() {
  if (this.linked) {
    var e = f(this, ft), n = f(this, xt);
    e === null ? Zn = n : $(e, xt, n), n === null ? Pt = e : $(n, ft, e), this.linked = !1;
  }
};
let dt = Wn;
function O(t) {
  var e = an;
  an = !0;
  try {
    for (var n; ; ) {
      if (hl(), T === null)
        return (
          /** @type {T} */
          n
        );
      T.flush();
    }
  } finally {
    an = e;
  }
}
function Sl() {
  try {
    el();
  } catch (t) {
    ut(t, ar);
  }
}
let Se = null;
function Xr(t) {
  var e = t.length;
  if (e !== 0) {
    for (var n = 0; n < e; ) {
      var r = t[n++];
      if ((r.f & (pe | re)) === 0 && bn(r) && (Se = /* @__PURE__ */ new Set(), Zt(r), r.deps === null && r.first === null && r.nodes === null && r.teardown === null && r.ac === null && Ji(r), (Se == null ? void 0 : Se.size) > 0)) {
        St.clear();
        for (const i of Se) {
          if ((i.f & (pe | re)) !== 0) continue;
          const s = [i];
          let l = i.parent;
          for (; l !== null; )
            Se.has(l) && (Se.delete(l), s.push(l)), l = l.parent;
          for (let a = s.length - 1; a >= 0; a--) {
            const o = s[a];
            (o.f & (pe | re)) === 0 && Zt(o);
          }
        }
        Se.clear();
      }
    }
    Se = null;
  }
}
function Pi(t, e, n, r) {
  if (!n.has(t) && (n.add(t), t.reactions !== null))
    for (const i of t.reactions) {
      const s = i.f;
      (s & Z) !== 0 ? Pi(
        /** @type {Derived} */
        i,
        e,
        n,
        r
      ) : (s & (jt | Ie)) !== 0 && (s & K) === 0 && $r(i, e, r) && (Y(i, K), Tr(
        /** @type {Effect} */
        i
      ));
    }
}
function $r(t, e, n) {
  const r = n.get(t);
  if (r !== void 0) return r;
  if (t.deps !== null)
    for (const i of t.deps) {
      if (An.call(e, i))
        return !0;
      if ((i.f & Z) !== 0 && $r(
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
function Tr(t) {
  T.schedule(t);
}
function Di(t, e) {
  if (!((t.f & Oe) !== 0 && (t.f & G) !== 0)) {
    (t.f & K) !== 0 ? e.d.push(t) : (t.f & Be) !== 0 && e.m.push(t), Y(t, G);
    for (var n = t.first; n !== null; )
      Di(n, e), n = n.next;
  }
}
function Hi(t) {
  Y(t, G);
  for (var e = t.first; e !== null; )
    Hi(e), e = e.next;
}
let Dn = /* @__PURE__ */ new Set();
const St = /* @__PURE__ */ new Map();
let ji = !1;
function It(t, e) {
  var n = {
    f: 0,
    // TODO ideally we could skip this altogether, but it causes type errors
    v: t,
    reactions: null,
    equals: bi,
    rv: 0,
    wv: 0
  };
  return n;
}
// @__NO_SIDE_EFFECTS__
function De(t, e) {
  const n = It(t);
  return es(n), n;
}
// @__NO_SIDE_EFFECTS__
function Fi(t, e = !1, n = !0) {
  const r = It(t);
  return e || (r.equals = yi), r;
}
function Ae(t, e, n = !1) {
  A !== null && // since we are untracking the function inside `$inspect.with` we need to add this check
  // to ensure we error if state is set inside an inspect effect
  (!Ne || (A.f & On) !== 0) && wi() && (A.f & (Z | Ie | jt | On)) !== 0 && (ze === null || !ze.has(t)) && sl();
  let r = n ? mt(e) : e;
  return Jt(t, r, Sn);
}
function Jt(t, e, n = null) {
  if (!t.equals(e)) {
    St.set(t, rt ? e : t.v);
    var r = dt.ensure();
    if (r.capture(t, e), (t.f & Z) !== 0) {
      const i = (
        /** @type {Derived} */
        t
      );
      (t.f & K) !== 0 && kr(i), J === null && Er(i);
    }
    t.wv = ns(), Wi(t, K, n), M !== null && (M.f & G) !== 0 && (M.f & (Oe | nt)) === 0 && (_e === null ? Hl([t]) : _e.push(t)), !r.is_fork && Dn.size > 0 && !ji && Cl();
  }
  return e;
}
function Cl() {
  ji = !1;
  for (const t of Dn) {
    (t.f & G) !== 0 && Y(t, Be);
    let e;
    try {
      e = bn(t);
    } catch {
      e = !0;
    }
    e && Zt(t);
  }
  Dn.clear();
}
function on(t) {
  Ae(t, t.v + 1);
}
function Wi(t, e, n) {
  var r = t.reactions;
  if (r !== null)
    for (var i = r.length, s = 0; s < i; s++) {
      var l = r[s], a = l.f, o = (a & K) === 0;
      if (o && Y(l, e), (a & On) !== 0)
        Dn.add(
          /** @type {Effect} */
          l
        );
      else if ((a & Z) !== 0) {
        var u = (
          /** @type {Derived} */
          l
        );
        J == null || J.delete(u), (a & At) === 0 && (a & Ee && (M === null || (M.f & Rn) === 0) && (l.f |= At), Wi(u, Be, n));
      } else if (o) {
        var v = (
          /** @type {Effect} */
          l
        );
        (a & Ie) !== 0 && Se !== null && Se.add(v), n !== null ? n.push(v) : Tr(v);
      }
    }
}
function mt(t) {
  if (typeof t != "object" || t === null || Tt in t)
    return t;
  const e = di(t);
  if (e !== Fs && e !== Ws)
    return t;
  var n = /* @__PURE__ */ new Map(), r = wr(t), i = /* @__PURE__ */ De(0), s = Mt, l = (a) => {
    if (Mt === s)
      return a();
    var o = A, u = Mt;
    $e(null), Qr(s);
    var v = a();
    return $e(o), Qr(u), v;
  };
  return r && n.set("length", /* @__PURE__ */ De(
    /** @type {any[]} */
    t.length
  )), new Proxy(
    /** @type {any} */
    t,
    {
      defineProperty(a, o, u) {
        (!("value" in u) || u.configurable === !1 || u.enumerable === !1 || u.writable === !1) && rl();
        var v = n.get(o);
        return v === void 0 ? l(() => {
          var b = /* @__PURE__ */ De(u.value);
          return n.set(o, b), b;
        }) : Ae(v, u.value, !0), !0;
      },
      deleteProperty(a, o) {
        var u = n.get(o);
        if (u === void 0) {
          if (o in a) {
            const v = l(() => /* @__PURE__ */ De(X));
            n.set(o, v), on(i);
          }
        } else
          Ae(u, X), on(i);
        return !0;
      },
      get(a, o, u) {
        var g;
        if (o === Tt)
          return t;
        var v = n.get(o), b = o in a;
        if (v === void 0 && (!b || (g = $t(a, o)) != null && g.writable) && (v = l(() => {
          var _ = mt(b ? a[o] : X), h = /* @__PURE__ */ De(_);
          return h;
        }), n.set(o, v)), v !== void 0) {
          var d = N(v);
          return d === X ? void 0 : d;
        }
        return Reflect.get(a, o, u);
      },
      getOwnPropertyDescriptor(a, o) {
        var u = Reflect.getOwnPropertyDescriptor(a, o);
        if (u && "value" in u) {
          var v = n.get(o);
          v && (u.value = N(v));
        } else if (u === void 0) {
          var b = n.get(o), d = b == null ? void 0 : b.v;
          if (b !== void 0 && d !== X)
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
        if (o === Tt)
          return !0;
        var u = n.get(o), v = u !== void 0 && u.v !== X || Reflect.has(a, o);
        if (u !== void 0 || M !== null && (!v || (d = $t(a, o)) != null && d.writable)) {
          u === void 0 && (u = l(() => {
            var g = v ? mt(a[o]) : X, _ = /* @__PURE__ */ De(g);
            return _;
          }), n.set(o, u));
          var b = N(u);
          if (b === X)
            return !1;
        }
        return v;
      },
      set(a, o, u, v) {
        var c;
        var b = n.get(o), d = o in a;
        if (r && o === "length")
          for (var g = u; g < /** @type {Source<number>} */
          b.v; g += 1) {
            var _ = n.get(g + "");
            _ !== void 0 ? Ae(_, X) : g in a && (_ = l(() => /* @__PURE__ */ De(X)), n.set(g + "", _));
          }
        if (b === void 0)
          (!d || (c = $t(a, o)) != null && c.writable) && (b = l(() => /* @__PURE__ */ De(void 0)), Ae(b, mt(u)), n.set(o, b));
        else {
          d = b.v !== X;
          var h = l(() => mt(u));
          Ae(b, h);
        }
        var p = Reflect.getOwnPropertyDescriptor(a, o);
        if (p != null && p.set && p.set.call(v, u), !d) {
          if (r && typeof o == "string") {
            var y = (
              /** @type {Source<number>} */
              n.get("length")
            ), w = Number(o);
            Number.isInteger(w) && w >= y.v && Ae(y, w + 1);
          }
          on(i);
        }
        return !0;
      },
      ownKeys(a) {
        N(i);
        var o = Reflect.ownKeys(a).filter((b) => {
          var d = n.get(b);
          return d === void 0 || d.v !== X;
        });
        for (var [u, v] of n)
          v.v !== X && !(u in a) && o.push(u);
        return o;
      },
      setPrototypeOf() {
        il();
      }
    }
  );
}
function Gr(t) {
  try {
    if (t !== null && typeof t == "object" && Tt in t)
      return t[Tt];
  } catch {
  }
  return t;
}
function Ml(t, e) {
  return Object.is(Gr(t), Gr(e));
}
var Kr, zi, Bi, Yi;
function ur() {
  if (Kr === void 0) {
    Kr = window, zi = /Firefox/.test(navigator.userAgent);
    var t = Element.prototype, e = Node.prototype, n = Text.prototype;
    Bi = $t(e, "firstChild").get, Yi = $t(e, "nextSibling").get, qr(t) && (t[Us] = void 0, t[kn] = null, t[Xs] = void 0, t.__e = void 0), qr(n) && (n[en] = void 0);
  }
}
function We(t = "") {
  return document.createTextNode(t);
}
// @__NO_SIDE_EFFECTS__
function un(t) {
  return (
    /** @type {TemplateNode | null} */
    Bi.call(t)
  );
}
// @__NO_SIDE_EFFECTS__
function lt(t) {
  return (
    /** @type {TemplateNode | null} */
    Yi.call(t)
  );
}
function q(t, e) {
  if (!j)
    return /* @__PURE__ */ un(t);
  var n = /* @__PURE__ */ un(F);
  if (n === null)
    n = F.appendChild(We());
  else if (e && n.nodeType !== xr) {
    var r = We();
    return n == null || n.before(r), ge(r), r;
  }
  return e && Ui(
    /** @type {Text} */
    n
  ), ge(n), n;
}
function ke(t, e = 1, n = !1) {
  let r = j ? F : t;
  for (var i; e--; )
    i = r, r = /** @type {TemplateNode} */
    /* @__PURE__ */ lt(r);
  if (!j)
    return r;
  if (n) {
    if ((r == null ? void 0 : r.nodeType) !== xr) {
      var s = We();
      return r === null ? i == null || i.after(s) : r.before(s), ge(s), s;
    }
    Ui(
      /** @type {Text} */
      r
    );
  }
  return ge(r), r;
}
function Vi(t) {
  t.textContent = "";
}
function qi() {
  return !1;
}
function Sr(t, e, n) {
  return (
    /** @type {T extends keyof HTMLElementTagNameMap ? HTMLElementTagNameMap[T] : Element} */
    n ? document.createElement(t, { is: n }) : document.createElement(t)
  );
}
function Ui(t) {
  if (
    /** @type {string} */
    t.nodeValue.length < 65536
  )
    return;
  let e = t.nextSibling;
  for (; e !== null && e.nodeType === xr; )
    e.remove(), t.nodeValue += /** @type {string} */
    e.nodeValue, e = t.nextSibling;
}
let Jr = !1;
function Al() {
  Jr || (Jr = !0, document.addEventListener(
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
            (e = n[gi]) == null || e.call(n);
      });
    },
    // In the capture phase to guarantee we get noticed of it (no possibility of stopPropagation)
    { capture: !0 }
  ));
}
function Cr(t) {
  var e = A, n = M;
  $e(null), Ye(null);
  try {
    return t();
  } finally {
    $e(e), Ye(n);
  }
}
function Il(t) {
  M === null && (A === null && Qs(), Zs()), rt && Js();
}
function Nl(t, e) {
  var n = e.last;
  n === null ? e.last = e.first = t : (n.next = t, t.prev = n, e.last = t);
}
function Ve(t, e) {
  var n = M;
  n !== null && (n.f & re) !== 0 && (t |= re);
  var r = {
    ctx: ie,
    deps: null,
    nodes: null,
    f: t | K | Ee,
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
  if ((t & Xt) !== 0)
    Ht !== null ? Ht.push(r) : dt.ensure().schedule(r);
  else if (e !== null) {
    try {
      Zt(r);
    } catch (l) {
      throw se(r), l;
    }
    i.deps === null && i.teardown === null && i.nodes === null && i.first === i.last && // either `null`, or a singular child
    (i.f & Ot) === 0 && (i = i.first, (t & Ie) !== 0 && (t & Gt) !== 0 && i !== null && (i.f |= Gt));
  }
  if (i !== null && (i.parent = n, n !== null && Nl(i, n), A !== null && (A.f & Z) !== 0 && (t & nt) === 0)) {
    var s = (
      /** @type {Derived} */
      A
    );
    (s.effects ?? (s.effects = [])).push(i);
  }
  return r;
}
function Mr() {
  return A !== null && !Ne;
}
function Ar(t) {
  const e = Ve(Yn, null);
  return Y(e, G), e.teardown = t, e;
}
function Ir(t) {
  Il();
  var e = (
    /** @type {Effect} */
    M.f
  ), n = !A && (e & Oe) !== 0 && ie !== null && !ie.i;
  if (n) {
    var r = (
      /** @type {ComponentContext} */
      ie
    );
    (r.e ?? (r.e = [])).push(t);
  } else
    return Xi(t);
}
function Xi(t) {
  return Ve(Xt | Vs, t);
}
function Ol(t) {
  dt.ensure();
  const e = Ve(nt | Ot, t);
  return () => {
    se(e);
  };
}
function Rl(t) {
  dt.ensure();
  const e = Ve(nt | Ot, t);
  return (n = {}) => new Promise((r) => {
    n.outro ? Ct(e, () => {
      se(e), r(void 0);
    }) : (se(e), r(void 0));
  });
}
function Gi(t) {
  return Ve(Xt, t);
}
function Ll(t) {
  return Ve(jt | Ot, t);
}
function Nr(t, e = 0) {
  return Ve(Yn | e, t);
}
function me(t, e = [], n = [], r = []) {
  yl(r, e, n, (i) => {
    Ve(Yn, () => {
      t(...i.map(N));
    });
  });
}
function Or(t, e = 0) {
  var n = Ve(Ie | e, t);
  return n;
}
function xe(t) {
  return Ve(Oe | Ot, t);
}
function Ki(t) {
  var e = t.teardown;
  if (e !== null) {
    const n = rt, r = A;
    Zr(!0), $e(null);
    try {
      e.call(null);
    } finally {
      Zr(n), $e(r);
    }
  }
}
function Rr(t, e = !1) {
  var n = t.first;
  for (t.first = t.last = null; n !== null; ) {
    const i = n.ac;
    i !== null && Cr(() => {
      i.abort(Vn);
    });
    var r = n.next;
    (n.f & nt) !== 0 ? n.parent = null : se(n, e), n = r;
  }
}
function Pl(t) {
  for (var e = t.first; e !== null; ) {
    var n = e.next;
    (e.f & Oe) === 0 && se(e), e = n;
  }
}
function se(t, e = !0) {
  var n = !1;
  (e || (t.f & Ys) !== 0) && t.nodes !== null && t.nodes.end !== null && (Dl(
    t.nodes.start,
    /** @type {TemplateNode} */
    t.nodes.end
  ), n = !0), t.f |= rr, Rr(t, e && !n), cn(t, 0);
  var r = t.nodes && t.nodes.t;
  if (r !== null)
    for (const s of r)
      s.stop();
  Ki(t), t.f ^= rr, t.f |= pe;
  var i = t.parent;
  i !== null && i.first !== null && Ji(t), t.next = t.prev = t.teardown = t.ctx = t.deps = t.fn = t.nodes = t.ac = t.b = null;
}
function Dl(t, e) {
  for (; t !== null; ) {
    var n = t === e ? null : /* @__PURE__ */ lt(t);
    t.remove(), t = n;
  }
}
function Ji(t) {
  var e = t.parent, n = t.prev, r = t.next;
  n !== null && (n.next = r), r !== null && (r.prev = n), e !== null && (e.first === t && (e.first = r), e.last === t && (e.last = n));
}
function Ct(t, e, n = !0) {
  var r = [];
  Zi(t, r, !0);
  var i = () => {
    n && se(t), e && e();
  }, s = r.length;
  if (s > 0) {
    var l = () => --s || i();
    for (var a of r)
      a.out(l);
  } else
    i();
}
function Zi(t, e, n) {
  if ((t.f & re) === 0) {
    t.f ^= re;
    var r = t.nodes && t.nodes.t;
    if (r !== null)
      for (const a of r)
        (a.is_global || n) && e.push(a);
    for (var i = t.first; i !== null; ) {
      var s = i.next;
      if ((i.f & nt) === 0) {
        var l = (i.f & Gt) !== 0 || // If this is a branch effect without a block effect parent,
        // it means the parent block effect was pruned. In that case,
        // transparency information was transferred to the branch effect.
        (i.f & Oe) !== 0 && (t.f & Ie) !== 0;
        Zi(i, e, l ? n : !1);
      }
      i = s;
    }
  }
}
function Hn(t) {
  Qi(t, !0);
}
function Qi(t, e) {
  if ((t.f & re) !== 0) {
    t.f ^= re, (t.f & G) === 0 && (Y(t, K), dt.ensure().schedule(t));
    for (var n = t.first; n !== null; ) {
      var r = n.next, i = (n.f & Gt) !== 0 || (n.f & Oe) !== 0;
      Qi(n, i ? e : !1), n = r;
    }
    var s = t.nodes && t.nodes.t;
    if (s !== null)
      for (const l of s)
        (l.is_global || e) && l.in();
  }
}
function Lr(t, e) {
  if (t.nodes)
    for (var n = t.nodes.start, r = t.nodes.end; n !== null; ) {
      var i = n === r ? null : /* @__PURE__ */ lt(n);
      e.append(n), n = i;
    }
}
let Cn = !1, rt = !1;
function Zr(t) {
  rt = t;
}
let A = null, Ne = !1;
function $e(t) {
  A = t;
}
let M = null;
function Ye(t) {
  M = t;
}
let ze = null;
function es(t) {
  A !== null && (ze ?? (ze = /* @__PURE__ */ new Set())).add(t);
}
let ue = null, ce = 0, _e = null;
function Hl(t) {
  _e = t;
}
let ts = 1, _t = 0, Mt = _t;
function Qr(t) {
  Mt = t;
}
function ns() {
  return ++ts;
}
function bn(t) {
  var e = t.f;
  if ((e & K) !== 0)
    return !0;
  if (e & Z && (t.f &= ~At), (e & Be) !== 0) {
    for (var n = (
      /** @type {Value[]} */
      t.deps
    ), r = n.length, i = 0; i < r; i++) {
      var s = n[i];
      if (bn(
        /** @type {Derived} */
        s
      ) && Ni(
        /** @type {Derived} */
        s
      ), s.wv > t.wv)
        return !0;
    }
    (e & Ee) !== 0 && // During time traveling we don't want to reset the status so that
    // traversal of the graph in the other batches still happens
    J === null && Y(t, G);
  }
  return !1;
}
function rs(t, e, n = !0) {
  var r = t.reactions;
  if (r !== null && !(ze !== null && ze.has(t)))
    for (var i = 0; i < r.length; i++) {
      var s = r[i];
      (s.f & Z) !== 0 ? rs(
        /** @type {Derived} */
        s,
        e,
        !1
      ) : e === s && (n ? Y(s, K) : (s.f & G) !== 0 && Y(s, Be), Tr(
        /** @type {Effect} */
        s
      ));
    }
}
function is(t) {
  var h;
  var e = ue, n = ce, r = _e, i = A, s = ze, l = ie, a = Ne, o = Mt, u = t.f;
  ue = /** @type {null | Value[]} */
  null, ce = 0, _e = null, A = (u & (Oe | nt)) === 0 ? t : null, ze = null, Kt(t.ctx), Ne = !1, Mt = ++_t, t.ac !== null && (Cr(() => {
    t.ac.abort(Vn);
  }), t.ac = null);
  try {
    t.f |= Rn;
    var v = (
      /** @type {Function} */
      t.fn
    ), b = v();
    t.f |= Nt;
    var d = t.deps, g = T == null ? void 0 : T.is_fork;
    if (ue !== null) {
      var _;
      if (g || cn(t, ce), d !== null && ce > 0)
        for (d.length = ce + ue.length, _ = 0; _ < ue.length; _++)
          d[ce + _] = ue[_];
      else
        t.deps = d = ue;
      if (Mr() && (t.f & Ee) !== 0)
        for (_ = ce; _ < d.length; _++)
          ((h = d[_]).reactions ?? (h.reactions = [])).push(t);
    } else !g && d !== null && ce < d.length && (cn(t, ce), d.length = ce);
    if (wi() && _e !== null && !Ne && d !== null && (t.f & (Z | Be | K)) === 0)
      for (_ = 0; _ < /** @type {Source[]} */
      _e.length; _++)
        rs(
          _e[_],
          /** @type {Effect} */
          t
        );
    if (i !== null && i !== t) {
      if (_t++, i.deps !== null)
        for (let p = 0; p < n; p += 1)
          i.deps[p].rv = _t;
      if (e !== null)
        for (const p of e)
          p.rv = _t;
      _e !== null && (r === null ? r = _e : r.push(.../** @type {Source[]} */
      _e));
    }
    return (t.f & ct) !== 0 && (t.f ^= ct), b;
  } catch (p) {
    return Ei(p);
  } finally {
    t.f ^= Rn, ue = e, ce = n, _e = r, A = i, ze = s, Kt(l), Ne = a, Mt = o;
  }
}
function jl(t, e) {
  let n = e.reactions;
  if (n !== null) {
    var r = Hs.call(n, t);
    if (r !== -1) {
      var i = n.length - 1;
      i === 0 ? n = e.reactions = null : (n[r] = n[i], n.pop());
    }
  }
  if (n === null && (e.f & Z) !== 0 && // Destroying a child effect while updating a parent effect can cause a dependency to appear
  // to be unused, when in fact it is used by the currently-updating parent. Checking `new_deps`
  // allows us to skip the expensive work of disconnecting and immediately reconnecting it
  (ue === null || !An.call(ue, e))) {
    var s = (
      /** @type {Derived} */
      e
    );
    (s.f & Ee) !== 0 && (s.f ^= Ee, s.f &= ~At), s.v !== X && Er(s), kl(s), cn(s, 0);
  }
}
function cn(t, e) {
  var n = t.deps;
  if (n !== null)
    for (var r = e; r < n.length; r++)
      jl(t, n[r]);
}
function Zt(t) {
  var e = t.f;
  if ((e & pe) === 0) {
    Y(t, G);
    var n = M, r = Cn;
    M = t, Cn = !0;
    try {
      (e & (Ie | vi)) !== 0 ? Pl(t) : Rr(t), Ki(t);
      var i = is(t);
      t.teardown = typeof i == "function" ? i : null, t.wv = ts;
      var s;
      ci && dl && (t.f & K) !== 0 && t.deps;
    } finally {
      Cn = r, M = n;
    }
  }
}
function N(t) {
  var e = t.f, n = (e & Z) !== 0;
  if (A !== null && !Ne) {
    var r = M !== null && (M.f & pe) !== 0;
    if (!r && (ze === null || !ze.has(t))) {
      var i = A.deps;
      if ((A.f & Rn) !== 0)
        t.rv < _t && (t.rv = _t, ue === null && i !== null && i[ce] === t ? ce++ : ue === null ? ue = [t] : ue.push(t));
      else {
        A.deps ?? (A.deps = []), An.call(A.deps, t) || A.deps.push(t);
        var s = t.reactions;
        s === null ? t.reactions = [A] : An.call(s, A) || s.push(A);
      }
    }
  }
  if (rt && St.has(t))
    return St.get(t);
  if (n) {
    var l = (
      /** @type {Derived} */
      t
    );
    if (rt) {
      var a = l.v;
      return ((l.f & G) === 0 && l.reactions !== null || ls(l)) && (a = kr(l)), St.set(l, a), a;
    }
    var o = (l.f & Ee) === 0 && !Ne && A !== null && (Cn || (A.f & Ee) !== 0), u = (l.f & Nt) === 0;
    bn(l) && (o && (l.f |= Ee), Ni(l)), o && !u && (Oi(l), ss(l));
  }
  if (J != null && J.has(t))
    return J.get(t);
  if ((t.f & ct) !== 0)
    throw t.v;
  return t.v;
}
function ss(t) {
  if (t.f |= Ee, t.deps !== null)
    for (const e of t.deps)
      (e.reactions ?? (e.reactions = [])).push(t), (e.f & Z) !== 0 && (e.f & Ee) === 0 && (Oi(
        /** @type {Derived} */
        e
      ), ss(
        /** @type {Derived} */
        e
      ));
}
function ls(t) {
  if (t.v === X) return !0;
  if (t.deps === null) return !1;
  for (const e of t.deps)
    if (St.has(e) || (e.f & Z) !== 0 && ls(
      /** @type {Derived} */
      e
    ))
      return !0;
  return !1;
}
function Pr(t) {
  var e = Ne;
  try {
    return Ne = !0, t();
  } finally {
    Ne = e;
  }
}
const bt = Symbol("events"), as = /* @__PURE__ */ new Set(), cr = /* @__PURE__ */ new Set();
function Fl(t, e, n, r = {}) {
  function i(s) {
    if (r.capture || dr.call(e, s), !s.cancelBubble)
      return Cr(() => n == null ? void 0 : n.call(this, s));
  }
  return et(() => {
    e.addEventListener(t, i, r);
  }), i;
}
function os(t, e, n, r, i) {
  var s = { capture: r, passive: i }, l = Fl(t, e, n, s);
  (e === document.body || // @ts-ignore
  e === window || // @ts-ignore
  e === document || // Firefox has quirky behavior, it can happen that we still get "canplay" events when the element is already removed
  e instanceof HTMLMediaElement) && Ar(() => {
    e.removeEventListener(t, l, s);
  });
}
function ee(t, e, n) {
  (e[bt] ?? (e[bt] = {}))[t] = n;
}
function Rt(t) {
  for (var e = 0; e < t.length; e++)
    as.add(t[e]);
  for (var n of cr)
    n(t);
}
let ei = null;
function dr(t) {
  var h, p;
  var e = this, n = (
    /** @type {Node} */
    e.ownerDocument
  ), r = t.type, i = ((h = t.composedPath) == null ? void 0 : h.call(t)) || [], s = (
    /** @type {null | Element} */
    i[0] || t.target
  );
  ei = t;
  var l = 0, a = ei === t && t[bt];
  if (a) {
    var o = i.indexOf(a);
    if (o !== -1 && (e === document || e === /** @type {any} */
    window)) {
      t[bt] = e;
      return;
    }
    var u = i.indexOf(e);
    if (u === -1)
      return;
    o <= u && (l = o);
  }
  if (s = /** @type {Element} */
  i[l] || t.target, s !== e) {
    Nn(t, "currentTarget", {
      configurable: !0,
      get() {
        return s || n;
      }
    });
    var v = A, b = M;
    $e(null), Ye(null);
    try {
      for (var d, g = []; s !== null && s !== e; ) {
        try {
          var _ = (p = s[bt]) == null ? void 0 : p[r];
          _ != null && (!/** @type {any} */
          s.disabled || // DOM could've been updated already by the time this is reached, so we check this as well
          // -> the target could not have been disabled because it emits the event in the first place
          t.target === s) && _.call(s, t);
        } catch (y) {
          d ? g.push(y) : d = y;
        }
        if (t.cancelBubble) break;
        l++, s = l < i.length ? (
          /** @type {Element} */
          i[l]
        ) : null;
      }
      if (d) {
        for (let y of g)
          queueMicrotask(() => {
            throw y;
          });
        throw d;
      }
    } finally {
      t[bt] = e, delete t.currentTarget, $e(v), Ye(b);
    }
  }
}
var oi;
const er = (
  // We gotta write it like this because after downleveling the pure comment may end up in the wrong location
  ((oi = globalThis == null ? void 0 : globalThis.window) == null ? void 0 : oi.trustedTypes) && /* @__PURE__ */ globalThis.window.trustedTypes.createPolicy("svelte-trusted-html", {
    /** @param {string} html */
    createHTML: (t) => t
  })
);
function Wl(t) {
  return (
    /** @type {string} */
    (er == null ? void 0 : er.createHTML(t)) ?? t
  );
}
function zl(t) {
  var e = Sr("template");
  return e.innerHTML = Wl(t.replaceAll("<!>", "<!---->")), e.content;
}
function hr(t, e) {
  var n = (
    /** @type {Effect} */
    M
  );
  n.nodes === null && (n.nodes = { start: t, end: e, a: null, t: null });
}
// @__NO_SIDE_EFFECTS__
function le(t, e) {
  var n = (e & Ps) !== 0, r, i = !t.startsWith("<!>");
  return () => {
    if (j)
      return hr(F, null), F;
    r === void 0 && (r = zl(i ? t : "<!>" + t), r = /** @type {TemplateNode} */
    /* @__PURE__ */ un(r));
    var s = (
      /** @type {TemplateNode} */
      n || zi ? document.importNode(r, !0) : r.cloneNode(!0)
    );
    return hr(s, s), s;
  };
}
function te(t, e) {
  if (j) {
    var n = (
      /** @type {Effect & { nodes: EffectNodes }} */
      M
    );
    ((n.f & Nt) === 0 || n.nodes.end === null) && (n.nodes.end = F), Un();
    return;
  }
  t !== null && t.before(
    /** @type {Node} */
    e
  );
}
const Bl = ["touchstart", "touchmove"];
function Yl(t) {
  return Bl.includes(t);
}
function Re(t, e) {
  var n = e == null ? "" : typeof e == "object" ? `${e}` : e;
  n !== /** @type {any} */
  (t[en] ?? (t[en] = t.nodeValue)) && (t[en] = n, t.nodeValue = `${n}`);
}
function fs(t, e) {
  return us(t, e);
}
function Vl(t, e) {
  ur(), e.intro = e.intro ?? !1;
  const n = e.target, r = j, i = F;
  try {
    for (var s = /* @__PURE__ */ un(n); s && (s.nodeType !== _n || /** @type {Comment} */
    s.data !== ui); )
      s = /* @__PURE__ */ lt(s);
    if (!s)
      throw Ut;
    Qe(!0), ge(
      /** @type {Comment} */
      s
    );
    const l = us(t, { ...e, anchor: s });
    return Qe(!1), /**  @type {Exports} */
    l;
  } catch (l) {
    if (l instanceof Error && l.message.split(`
`).some((a) => a.startsWith("https://svelte.dev/e/")))
      throw l;
    return l !== Ut && console.warn("Failed to hydrate: ", l), e.recover === !1 && tl(), ur(), Vi(n), Qe(!1), fs(t, e);
  } finally {
    Qe(r), ge(i);
  }
}
const En = /* @__PURE__ */ new Map();
function us(t, { target: e, anchor: n, props: r = {}, events: i, context: s, intro: l = !0, transformError: a }) {
  ur();
  var o = void 0, u = Rl(() => {
    var v = n ?? e.appendChild(We());
    _l(
      /** @type {TemplateNode} */
      v,
      {
        pending: () => {
        }
      },
      (g) => {
        it({});
        var _ = (
          /** @type {ComponentContext} */
          ie
        );
        if (s && (_.c = s), i && (r.$$events = i), j && hr(
          /** @type {TemplateNode} */
          g,
          null
        ), o = t(g, r) || {}, j && (M.nodes.end = F, F === null || F.nodeType !== _n || /** @type {Comment} */
        F.data !== yr))
          throw qn(), Ut;
        st();
      },
      a
    );
    var b = /* @__PURE__ */ new Set(), d = (g) => {
      for (var _ = 0; _ < g.length; _++) {
        var h = g[_];
        if (!b.has(h)) {
          b.add(h);
          var p = Yl(h);
          for (const c of [e, document]) {
            var y = En.get(c);
            y === void 0 && (y = /* @__PURE__ */ new Map(), En.set(c, y));
            var w = y.get(h);
            w === void 0 ? (c.addEventListener(h, dr, { passive: p }), y.set(h, 1)) : y.set(h, w + 1);
          }
        }
      }
    };
    return d(Bn(as)), cr.add(d), () => {
      var p;
      for (var g of b)
        for (const y of [e, document]) {
          var _ = (
            /** @type {Map<string, number>} */
            En.get(y)
          ), h = (
            /** @type {number} */
            _.get(g)
          );
          --h == 0 ? (y.removeEventListener(g, dr), _.delete(g), _.size === 0 && En.delete(y)) : _.set(g, h);
        }
      cr.delete(d), v !== n && ((p = v.parentNode) == null || p.removeChild(v));
    };
  });
  return vr.set(o, u), o;
}
let vr = /* @__PURE__ */ new WeakMap();
function ql(t, e) {
  const n = vr.get(t);
  return n ? (vr.delete(t), n(e)) : Promise.resolve();
}
var Me, je, ve, kt, gn, mn, zn;
class Ul {
  /**
   * @param {TemplateNode} anchor
   * @param {boolean} transition
   */
  constructor(e, n = !0) {
    /** @type {TemplateNode} */
    B(this, "anchor");
    /** @type {Map<Batch, Key>} */
    S(this, Me, /* @__PURE__ */ new Map());
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
    S(this, je, /* @__PURE__ */ new Map());
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
    S(this, kt, /* @__PURE__ */ new Set());
    /**
     * Whether to pause (i.e. outro) on change, or destroy immediately.
     * This is necessary for `<svelte:element>`
     */
    S(this, gn, !0);
    /**
     * @param {Batch} batch
     */
    S(this, mn, (e) => {
      if (f(this, Me).has(e)) {
        var n = (
          /** @type {Key} */
          f(this, Me).get(e)
        ), r = f(this, je).get(n);
        if (r)
          Hn(r), f(this, kt).delete(n);
        else {
          var i = f(this, ve).get(n);
          i && (Hn(i.effect), f(this, je).set(n, i.effect), f(this, ve).delete(n), i.fragment.lastChild.remove(), this.anchor.before(i.fragment), r = i.effect);
        }
        for (const [s, l] of f(this, Me)) {
          if (f(this, Me).delete(s), s === e)
            break;
          const a = f(this, ve).get(l);
          a && (se(a.effect), f(this, ve).delete(l));
        }
        for (const [s, l] of f(this, je)) {
          if (s === n || f(this, kt).has(s)) continue;
          const a = () => {
            if (Array.from(f(this, Me).values()).includes(s)) {
              var u = document.createDocumentFragment();
              Lr(l, u), u.append(We()), f(this, ve).set(s, { effect: l, fragment: u });
            } else
              se(l);
            f(this, kt).delete(s), f(this, je).delete(s);
          };
          f(this, gn) || !r ? (f(this, kt).add(s), Ct(l, a, !1)) : a();
        }
      }
    });
    /**
     * @param {Batch} batch
     */
    S(this, zn, (e) => {
      f(this, Me).delete(e);
      const n = Array.from(f(this, Me).values());
      for (const [r, i] of f(this, ve))
        n.includes(r) || (se(i.effect), f(this, ve).delete(r));
    });
    this.anchor = e, $(this, gn, n);
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
    ), i = qi();
    if (n && !f(this, je).has(e) && !f(this, ve).has(e))
      if (i) {
        var s = document.createDocumentFragment(), l = We();
        s.append(l), f(this, ve).set(e, {
          effect: xe(() => n(l)),
          fragment: s
        });
      } else
        f(this, je).set(
          e,
          xe(() => n(this.anchor))
        );
    if (f(this, Me).set(r, e), i) {
      for (const [a, o] of f(this, je))
        a === e ? r.unskip_effect(o) : r.skip_effect(o);
      for (const [a, o] of f(this, ve))
        a === e ? r.unskip_effect(o.effect) : r.skip_effect(o.effect);
      r.oncommit(f(this, mn)), r.ondiscard(f(this, zn));
    } else
      j && (this.anchor = F), f(this, mn).call(this, r);
  }
}
Me = new WeakMap(), je = new WeakMap(), ve = new WeakMap(), kt = new WeakMap(), gn = new WeakMap(), mn = new WeakMap(), zn = new WeakMap();
function yn(t, e, n = !1) {
  var r;
  j && (r = F, Un());
  var i = new Ul(t), s = n ? Gt : 0;
  function l(a, o) {
    if (j) {
      var u = _i(
        /** @type {TemplateNode} */
        r
      );
      if (a !== parseInt(u.substring(1))) {
        var v = Ln();
        ge(v), i.anchor = v, Qe(!1), i.ensure(a, o), Qe(!0);
        return;
      }
    }
    i.ensure(a, o);
  }
  Or(() => {
    var a = !1;
    e((o, u = 0) => {
      a = !0, l(u, o);
    }), a || l(-1, null);
  }, s);
}
function cs(t, e) {
  return e;
}
function Xl(t, e, n) {
  for (var r = [], i = e.length, s, l = e.length, a = 0; a < i; a++) {
    let b = e[a];
    Ct(
      b,
      () => {
        if (s) {
          if (s.pending.delete(b), s.done.add(b), s.pending.size === 0) {
            var d = (
              /** @type {Set<EachOutroGroup>} */
              t.outrogroups
            );
            pr(t, Bn(s.done)), d.delete(s), d.size === 0 && (t.outrogroups = null);
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
      ), v = (
        /** @type {Element} */
        u.parentNode
      );
      Vi(v), v.append(u), t.items.clear();
    }
    pr(t, e, !o);
  } else
    s = {
      pending: new Set(e),
      done: /* @__PURE__ */ new Set()
    }, (t.outrogroups ?? (t.outrogroups = /* @__PURE__ */ new Set())).add(s);
}
function pr(t, e, n = !0) {
  var r;
  if (t.pending.size > 0) {
    r = /* @__PURE__ */ new Set();
    for (const l of t.pending.values())
      for (const a of l)
        r.add(
          /** @type {EachItem} */
          t.items.get(a).e
        );
  }
  for (var i = 0; i < e.length; i++) {
    var s = e[i];
    if (r != null && r.has(s)) {
      s.f |= Fe;
      const l = document.createDocumentFragment();
      Lr(s, l);
    } else
      se(e[i], n);
  }
}
var ti;
function ds(t, e, n, r, i, s = null) {
  var l = t, a = /* @__PURE__ */ new Map(), o = (e & fi) !== 0;
  if (o) {
    var u = (
      /** @type {Element} */
      t
    );
    l = j ? ge(/* @__PURE__ */ un(u)) : u.appendChild(We());
  }
  j && Un();
  var v = null, b = /* @__PURE__ */ Ii(() => {
    var c = n();
    return (
      /** @type {V[]} */
      wr(c) ? c : c == null ? [] : Bn(c)
    );
  }), d, g = /* @__PURE__ */ new Map(), _ = !0;
  function h(c) {
    (w.effect.f & pe) === 0 && (w.pending.delete(c), w.fallback = v, Gl(w, d, l, e, r), v !== null && (d.length === 0 ? (v.f & Fe) === 0 ? Hn(v) : (v.f ^= Fe, sn(v, null, l)) : Ct(v, () => {
      v = null;
    })));
  }
  function p(c) {
    w.pending.delete(c);
  }
  var y = Or(() => {
    d = /** @type {V[]} */
    N(b);
    var c = d.length;
    let x = !1;
    if (j) {
      var m = _i(l) === br;
      m !== (c === 0) && (l = Ln(), ge(l), Qe(!1), x = !0);
    }
    for (var k = /* @__PURE__ */ new Set(), C = (
      /** @type {Batch} */
      T
    ), D = qi(), P = 0; P < c; P += 1) {
      j && F.nodeType === _n && /** @type {Comment} */
      F.data === yr && (l = /** @type {Comment} */
      F, x = !0, Qe(!1));
      var U = d[P], H = r(U, P), z = _ ? null : a.get(H);
      z ? (z.v && Jt(z.v, U), z.i && Jt(z.i, P), D && C.unskip_effect(z.e)) : (z = Kl(
        a,
        _ ? l : ti ?? (ti = We()),
        U,
        H,
        P,
        i,
        e,
        n
      ), _ || (z.e.f |= Fe), a.set(H, z)), k.add(H);
    }
    if (c === 0 && s && !v && (_ ? v = xe(() => s(l)) : (v = xe(() => s(ti ?? (ti = We()))), v.f |= Fe)), c > k.size && Ks(), j && c > 0 && ge(Ln()), !_)
      if (g.set(C, k), D) {
        for (const [Q, Pe] of a)
          k.has(Q) || C.skip_effect(Pe.e);
        C.oncommit(h), C.ondiscard(p);
      } else
        h(C);
    x && Qe(!0), N(b);
  }), w = { effect: y, items: a, pending: g, outrogroups: null, fallback: v };
  _ = !1, j && (l = F);
}
function Qt(t) {
  for (; t !== null && (t.f & Oe) === 0; )
    t = t.next;
  return t;
}
function Gl(t, e, n, r, i) {
  var U, H, z, Q, Pe, qe, E, ae, zr;
  var s = (r & As) !== 0, l = e.length, a = t.items, o = Qt(t.effect.first), u, v = null, b, d = [], g = [], _, h, p, y;
  if (s)
    for (y = 0; y < l; y += 1)
      _ = e[y], h = i(_, y), p = /** @type {EachItem} */
      a.get(h).e, (p.f & Fe) === 0 && ((H = (U = p.nodes) == null ? void 0 : U.a) == null || H.measure(), (b ?? (b = /* @__PURE__ */ new Set())).add(p));
  for (y = 0; y < l; y += 1) {
    if (_ = e[y], h = i(_, y), p = /** @type {EachItem} */
    a.get(h).e, t.outrogroups !== null)
      for (const Ue of t.outrogroups)
        Ue.pending.delete(p), Ue.done.delete(p);
    if ((p.f & re) !== 0 && (Hn(p), s && ((Q = (z = p.nodes) == null ? void 0 : z.a) == null || Q.unfix(), (b ?? (b = /* @__PURE__ */ new Set())).delete(p))), (p.f & Fe) !== 0)
      if (p.f ^= Fe, p === o)
        sn(p, null, n);
      else {
        var w = v ? v.next : o;
        p === t.effect.last && (t.effect.last = p.prev), p.prev && (p.prev.next = p.next), p.next && (p.next.prev = p.prev), at(t, v, p), at(t, p, w), sn(p, w, n), v = p, d = [], g = [], o = Qt(v.next);
        continue;
      }
    if (p !== o) {
      if (u !== void 0 && u.has(p)) {
        if (d.length < g.length) {
          var c = g[0], x;
          v = c.prev;
          var m = d[0], k = d[d.length - 1];
          for (x = 0; x < d.length; x += 1)
            sn(d[x], c, n);
          for (x = 0; x < g.length; x += 1)
            u.delete(g[x]);
          at(t, m.prev, k.next), at(t, v, m), at(t, k, c), o = c, v = k, y -= 1, d = [], g = [];
        } else
          u.delete(p), sn(p, o, n), at(t, p.prev, p.next), at(t, p, v === null ? t.effect.first : v.next), at(t, v, p), v = p;
        continue;
      }
      for (d = [], g = []; o !== null && o !== p; )
        (u ?? (u = /* @__PURE__ */ new Set())).add(o), g.push(o), o = Qt(o.next);
      if (o === null)
        continue;
    }
    (p.f & Fe) === 0 && d.push(p), v = p, o = Qt(p.next);
  }
  if (t.outrogroups !== null) {
    for (const Ue of t.outrogroups)
      Ue.pending.size === 0 && (pr(t, Bn(Ue.done)), (Pe = t.outrogroups) == null || Pe.delete(Ue));
    t.outrogroups.size === 0 && (t.outrogroups = null);
  }
  if (o !== null || u !== void 0) {
    var C = [];
    if (u !== void 0)
      for (p of u)
        (p.f & re) === 0 && C.push(p);
    for (; o !== null; )
      (o.f & re) === 0 && o !== t.fallback && C.push(o), o = Qt(o.next);
    var D = C.length;
    if (D > 0) {
      var P = (r & fi) !== 0 && l === 0 ? n : null;
      if (s) {
        for (y = 0; y < D; y += 1)
          (E = (qe = C[y].nodes) == null ? void 0 : qe.a) == null || E.measure();
        for (y = 0; y < D; y += 1)
          (zr = (ae = C[y].nodes) == null ? void 0 : ae.a) == null || zr.fix();
      }
      Xl(t, C, P);
    }
  }
  s && et(() => {
    var Ue, Br;
    if (b !== void 0)
      for (p of b)
        (Br = (Ue = p.nodes) == null ? void 0 : Ue.a) == null || Br.apply();
  });
}
function Kl(t, e, n, r, i, s, l, a) {
  var o = (l & Cs) !== 0 ? (l & Is) === 0 ? /* @__PURE__ */ Fi(n, !1, !1) : It(n) : null, u = (l & Ms) !== 0 ? It(i) : null;
  return {
    v: o,
    i: u,
    e: xe(() => (s(e, o ?? n, u ?? i, a), () => {
      t.delete(r);
    }))
  };
}
function sn(t, e, n) {
  if (t.nodes)
    for (var r = t.nodes.start, i = t.nodes.end, s = e && (e.f & Fe) === 0 ? (
      /** @type {EffectNodes} */
      e.nodes.start
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
function at(t, e, n) {
  e === null ? t.effect.first = n : e.next = n, n === null ? t.effect.last = e : n.prev = e;
}
function vt(t, e) {
  Gi(() => {
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
      const i = Sr("style");
      i.id = e.hash, i.textContent = e.code, r.appendChild(i);
    }
  });
}
function hs(t, e, n = !1) {
  if (t.multiple) {
    if (e == null)
      return;
    if (!wr(e))
      return ol();
    for (var r of t.options)
      r.selected = e.includes(ni(r));
    return;
  }
  for (r of t.options) {
    var i = ni(r);
    if (Ml(i, e)) {
      r.selected = !0;
      return;
    }
  }
  (!n || e !== void 0) && (t.selectedIndex = -1);
}
function Jl(t) {
  var e = new MutationObserver(() => {
    hs(t, t.__value);
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
  }), Ar(() => {
    e.disconnect();
  });
}
function ni(t) {
  return "__value" in t ? t.__value : t.value;
}
const Zl = Symbol("is custom element"), Ql = Symbol("is html"), ea = mi ? "link" : "LINK", ta = mi ? "progress" : "PROGRESS";
function Xn(t) {
  if (j) {
    var e = !1, n = () => {
      if (!e) {
        if (e = !0, t.hasAttribute("value")) {
          var r = t.value;
          tt(t, "value", null), t.value = r;
        }
        if (t.hasAttribute("checked")) {
          var i = t.checked;
          tt(t, "checked", null), t.checked = i;
        }
      }
    };
    t[gi] = n, et(n), Al();
  }
}
function Dr(t, e) {
  var n = Hr(t);
  n.value === (n.value = // treat null and undefined the same for the initial value
  e ?? void 0) || // @ts-expect-error
  // `progress` elements always need their value set when it's `0`
  t.value === e && (e !== 0 || t.nodeName !== ta) || (t.value = e ?? "");
}
function vs(t, e) {
  var n = Hr(t);
  n.checked !== (n.checked = // treat null and undefined the same for the initial value
  e ?? void 0) && (t.checked = e);
}
function na(t, e) {
  e ? t.hasAttribute("selected") || t.setAttribute("selected", "") : t.removeAttribute("selected");
}
function tt(t, e, n, r) {
  var i = Hr(t);
  j && (i[e] = t.getAttribute(e), e === "src" || e === "srcset" || e === "href" && t.nodeName === ea) || i[e] !== (i[e] = n) && (e === "loading" && (t[qs] = n), n == null ? t.removeAttribute(e) : typeof n != "string" && ra(t).includes(e) ? t[e] = n : t.setAttribute(e, n));
}
function Hr(t) {
  return (
    /** @type {Record<string | symbol, unknown>} **/
    /** @type {any} */
    t[kn] ?? (t[kn] = {
      [Zl]: t.nodeName.includes("-"),
      [Ql]: t.namespaceURI === Ds
    })
  );
}
var ri = /* @__PURE__ */ new Map();
function ra(t) {
  var e = t.getAttribute("is") || t.nodeName, n = ri.get(e);
  if (n) return n;
  ri.set(e, n = []);
  for (var r, i = t, s = Element.prototype; s !== i; ) {
    r = js(i);
    for (var l in r)
      r[l].set && // better safe than sorry, we don't want spread attributes to mess with HTML content
      l !== "innerHTML" && l !== "textContent" && l !== "innerText" && n.push(l);
    i = di(i);
  }
  return n;
}
function tr(t, e) {
  return t === e || (t == null ? void 0 : t[Tt]) === e;
}
function jr(t = {}, e, n, r) {
  var i = (
    /** @type {ComponentContext} */
    ie.r
  ), s = (
    /** @type {Effect} */
    M
  );
  return Gi(() => {
    var l, a;
    return Nr(() => {
      l = a, a = [], Pr(() => {
        tr(n(...a), t) || (e(t, ...a), l && tr(n(...l), t) && e(null, ...l));
      });
    }), () => {
      let o = s;
      for (; o !== i && o.parent !== null && o.parent.f & rr; )
        o = o.parent;
      const u = () => {
        a && tr(n(...a), t) && e(null, ...a);
      }, v = o.teardown;
      o.teardown = () => {
        u(), v == null || v();
      };
    };
  }), t;
}
function R(t, e, n, r) {
  var x;
  var i = !0, s = (n & Rs) !== 0, l = (n & Ls) !== 0, a = (
    /** @type {V} */
    r
  ), o = !0, u = (
    /** @type {Derived<V> | undefined} */
    void 0
  ), v = () => l && i ? (u ?? (u = /* @__PURE__ */ fn(
    /** @type {() => V} */
    r
  )), N(u)) : (o && (o = !1, a = l ? Pr(
    /** @type {() => V} */
    r
  ) : (
    /** @type {V} */
    r
  )), a);
  let b;
  if (s) {
    var d = Tt in t || pi in t;
    b = ((x = $t(t, e)) == null ? void 0 : x.set) ?? (d && e in t ? (m) => t[e] = m : void 0);
  }
  var g, _ = !1;
  s ? [g, _] = pl(() => (
    /** @type {V} */
    t[e]
  )) : g = /** @type {V} */
  t[e], g === void 0 && r !== void 0 && (g = v(), b && (nl(), b(g)));
  var h;
  if (h = () => {
    var m = (
      /** @type {V} */
      t[e]
    );
    return m === void 0 ? v() : (o = !0, m);
  }, (n & Os) === 0)
    return h;
  if (b) {
    var p = t.$$legacy;
    return (
      /** @type {() => V} */
      (function(m, k) {
        return arguments.length > 0 ? ((!k || p || _) && b(k ? h() : m), m) : h();
      })
    );
  }
  var y = !1, w = ((n & Ns) !== 0 ? fn : Ii)(() => (y = !1, h()));
  s && N(w);
  var c = (
    /** @type {Effect} */
    M
  );
  return (
    /** @type {() => V} */
    (function(m, k) {
      if (arguments.length > 0) {
        const C = k ? N(w) : s ? mt(m) : m;
        return Ae(w, C), y = !0, a !== void 0 && (a = C), m;
      }
      return rt && y || (c.f & pe) !== 0 ? w.v : N(w);
    })
  );
}
function ia(t) {
  return new sa(t);
}
var Ze, we;
class sa {
  /**
   * @param {ComponentConstructorOptions & {
   *  component: any;
   * }} options
   */
  constructor(e) {
    /** @type {any} */
    S(this, Ze);
    /** @type {Record<string, any>} */
    S(this, we);
    var s;
    var n = /* @__PURE__ */ new Map(), r = (l, a) => {
      var o = /* @__PURE__ */ Fi(a, !1, !1);
      return n.set(l, o), o;
    };
    const i = new Proxy(
      { ...e.props || {}, $$events: {} },
      {
        get(l, a) {
          return N(n.get(a) ?? r(a, Reflect.get(l, a)));
        },
        has(l, a) {
          return a === pi ? !0 : (N(n.get(a) ?? r(a, Reflect.get(l, a))), Reflect.has(l, a));
        },
        set(l, a, o) {
          return Ae(n.get(a) ?? r(a, o), o), Reflect.set(l, a, o);
        }
      }
    );
    $(this, we, (e.hydrate ? Vl : fs)(e.component, {
      target: e.target,
      anchor: e.anchor,
      props: i,
      context: e.context,
      intro: e.intro ?? !1,
      recover: e.recover,
      transformError: e.transformError
    })), (!((s = e == null ? void 0 : e.props) != null && s.$$host) || e.sync === !1) && O(), $(this, Ze, i.$$events);
    for (const l of Object.keys(f(this, we)))
      l === "$set" || l === "$destroy" || l === "$on" || Nn(this, l, {
        get() {
          return f(this, we)[l];
        },
        /** @param {any} value */
        set(a) {
          f(this, we)[l] = a;
        },
        enumerable: !0
      });
    f(this, we).$set = /** @param {Record<string, any>} next */
    (l) => {
      Object.assign(i, l);
    }, f(this, we).$destroy = () => {
      ql(f(this, we));
    };
  }
  /** @param {Record<string, any>} props */
  $set(e) {
    f(this, we).$set(e);
  }
  /**
   * @param {string} event
   * @param {(...args: any[]) => any} callback
   * @returns {any}
   */
  $on(e, n) {
    f(this, Ze)[e] = f(this, Ze)[e] || [];
    const r = (...i) => n.call(this, ...i);
    return f(this, Ze)[e].push(r), () => {
      f(this, Ze)[e] = f(this, Ze)[e].filter(
        /** @param {any} fn */
        (i) => i !== r
      );
    };
  }
  $destroy() {
    f(this, we).$destroy();
  }
}
Ze = new WeakMap(), we = new WeakMap();
let ps;
typeof HTMLElement == "function" && (ps = class extends HTMLElement {
  /**
   * @param {*} $$componentCtor
   * @param {*} $$slots
   * @param {ShadowRootInit | undefined} shadow_root_init
   */
  constructor(e, n, r) {
    super();
    /** The Svelte component constructor */
    B(this, "$$ctor");
    /** Slots */
    B(this, "$$s");
    /** @type {any} The Svelte component instance */
    B(this, "$$c");
    /** Whether or not the custom element is connected */
    B(this, "$$cn", !1);
    /** @type {Record<string, any>} Component props data */
    B(this, "$$d", {});
    /** `true` if currently in the process of reflecting component props back to attributes */
    B(this, "$$r", !1);
    /** @type {Record<string, CustomElementPropDefinition>} Props definition (name, reflected, type etc) */
    B(this, "$$p_d", {});
    /** @type {Record<string, EventListenerOrEventListenerObject[]>} Event listeners */
    B(this, "$$l", {});
    /** @type {Map<EventListenerOrEventListenerObject, Function>} Event listener unsubscribe functions */
    B(this, "$$l_u", /* @__PURE__ */ new Map());
    /** @type {any} The managed render effect for reflecting attributes */
    B(this, "$$me");
    /** @type {ShadowRoot | null} The ShadowRoot of the custom element */
    B(this, "$$shadowRoot", null);
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
          const l = Sr("slot");
          i !== "default" && (l.name = i), te(s, l);
        };
      };
      if (await Promise.resolve(), !this.$$cn || this.$$c)
        return;
      const n = {}, r = la(this);
      for (const i of this.$$s)
        i in r && (i === "default" && !this.$$d.children ? (this.$$d.children = e(i), n.default = !0) : n[i] = e(i));
      for (const i of this.attributes) {
        const s = this.$$g_p(i.name);
        s in this.$$d || (this.$$d[s] = Mn(s, i.value, this.$$p_d, "toProp"));
      }
      for (const i in this.$$p_d)
        !(i in this.$$d) && this[i] !== void 0 && (this.$$d[i] = this[i], delete this[i]);
      this.$$c = ia({
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
          for (const s of In(this.$$c)) {
            if (!((i = this.$$p_d[s]) != null && i.reflect)) continue;
            this.$$d[s] = this.$$c[s];
            const l = Mn(
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
  attributeChangedCallback(e, n, r) {
    var i;
    this.$$r || (e = this.$$g_p(e), this.$$d[e] = Mn(e, r, this.$$p_d, "toProp"), (i = this.$$c) == null || i.$set({ [e]: this.$$d[e] }));
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
    return In(this.$$p_d).find(
      (n) => this.$$p_d[n].attribute === e || !this.$$p_d[n].attribute && n.toLowerCase() === e
    ) || e;
  }
});
function Mn(t, e, n, r) {
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
function la(t) {
  const e = {};
  return t.childNodes.forEach((n) => {
    e[
      /** @type {Element} node */
      n.slot || "default"
    ] = !0;
  }), e;
}
function pt(t, e, n, r, i, s) {
  let l = class extends ps {
    constructor() {
      super(t, n, i), this.$$p_d = e;
    }
    static get observedAttributes() {
      return In(e).map(
        (a) => (e[a].attribute || a).toLowerCase()
      );
    }
  };
  return In(e).forEach((a) => {
    Nn(l.prototype, a, {
      get() {
        return this.$$c && a in this.$$c ? this.$$c[a] : this.$$d[a];
      },
      set(o) {
        var b;
        o = Mn(a, o, e), this.$$d[a] = o;
        var u = this.$$c;
        if (u) {
          var v = (b = $t(u, a)) == null ? void 0 : b.get;
          v ? u[a] = o : u.$set({ [a]: o });
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
  }), t.element = /** @type {any} */
  l, l;
}
var aa = /* @__PURE__ */ le('<span class="lbl"> </span>'), oa = /* @__PURE__ */ le('<label class="xi-slider svelte-8xo3l7"><!> <input type="range" class="svelte-8xo3l7"/> <span class="val svelte-8xo3l7"> </span></label>');
const fa = {
  hash: "svelte-8xo3l7",
  code: '.xi-slider.svelte-8xo3l7 {display:inline-flex;align-items:center;gap:0.5rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}input[type="range"].svelte-8xo3l7 {accent-color:var(--xi-accent, #3b82f6);vertical-align:middle;}.val.svelte-8xo3l7 {min-width:2.5em;text-align:right;font-variant-numeric:tabular-nums;}'
};
function ua(t, e) {
  it(e, !0), vt(t, fa);
  let n = R(e, "value", 15, 0), r = R(e, "min", 7, 0), i = R(e, "max", 7, 100), s = R(e, "step", 7, 1), l = R(e, "label", 7, ""), a = R(e, "disabled", 7, !1);
  const o = e.$$host, u = (c) => o.dispatchEvent(new CustomEvent(c, { detail: { value: n() }, bubbles: !0, composed: !0 }));
  function v(c) {
    n(Number(c.target.value)), u("input");
  }
  function b(c) {
    n(Number(c.target.value)), u("change");
  }
  var d = {
    get value() {
      return n();
    },
    set value(c = 0) {
      n(c), O();
    },
    get min() {
      return r();
    },
    set min(c = 0) {
      r(c), O();
    },
    get max() {
      return i();
    },
    set max(c = 100) {
      i(c), O();
    },
    get step() {
      return s();
    },
    set step(c = 1) {
      s(c), O();
    },
    get label() {
      return l();
    },
    set label(c = "") {
      l(c), O();
    },
    get disabled() {
      return a();
    },
    set disabled(c = !1) {
      a(c), O();
    }
  }, g = oa(), _ = q(g);
  {
    var h = (c) => {
      var x = aa(), m = q(x, !0);
      V(x), me(() => Re(m, l())), te(c, x);
    };
    yn(_, (c) => {
      l() && c(h);
    });
  }
  var p = ke(_, 2);
  Xn(p);
  var y = ke(p, 2), w = q(y, !0);
  return V(y), V(g), me(() => {
    tt(p, "min", r()), tt(p, "max", i()), tt(p, "step", s()), Dr(p, n()), p.disabled = a(), Re(w, n());
  }), ee("input", p, v), ee("change", p, b), te(t, g), st(d);
}
Rt(["input", "change"]);
customElements.define("xi-slider", pt(
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
var ca = /* @__PURE__ */ le('<span class="lbl"> </span>'), da = /* @__PURE__ */ le('<label class="xi-number svelte-1f6ykwb"><!> <input type="number" class="svelte-1f6ykwb"/></label>');
const ha = {
  hash: "svelte-1f6ykwb",
  code: ".xi-number.svelte-1f6ykwb {display:inline-flex;align-items:center;gap:0.5rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}input.svelte-1f6ykwb {width:6em;padding:0.15rem 0.3rem;accent-color:var(--xi-accent, #3b82f6);}"
};
function va(t, e) {
  it(e, !0), vt(t, ha);
  let n = R(e, "value", 15, 0), r = R(e, "min", 7), i = R(e, "max", 7), s = R(e, "step", 7, 1), l = R(e, "label", 7, ""), a = R(e, "disabled", 7, !1);
  const o = e.$$host, u = (w) => o.dispatchEvent(new CustomEvent(w, { detail: { value: n() }, bubbles: !0, composed: !0 })), v = (w) => w.target.value === "" ? null : Number(w.target.value);
  function b(w) {
    n(v(w)), u("input");
  }
  function d(w) {
    n(v(w)), u("change");
  }
  var g = {
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
  }, _ = da(), h = q(_);
  {
    var p = (w) => {
      var c = ca(), x = q(c, !0);
      V(c), me(() => Re(x, l())), te(w, c);
    };
    yn(h, (w) => {
      l() && w(p);
    });
  }
  var y = ke(h, 2);
  return Xn(y), V(_), me(() => {
    tt(y, "min", r()), tt(y, "max", i()), tt(y, "step", s()), Dr(y, n()), y.disabled = a();
  }), ee("input", y, b), ee("change", y, d), te(t, _), st(g);
}
Rt(["input", "change"]);
customElements.define("xi-number", pt(
  va,
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
var pa = /* @__PURE__ */ le('<span class="lbl"> </span>'), ga = /* @__PURE__ */ le('<label class="xi-toggle svelte-141rfru"><input type="checkbox" class="svelte-141rfru"/> <!></label>');
const ma = {
  hash: "svelte-141rfru",
  code: ".xi-toggle.svelte-141rfru {display:inline-flex;align-items:center;gap:0.4rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);cursor:pointer;}input.svelte-141rfru {accent-color:var(--xi-accent, #3b82f6);}"
};
function _a(t, e) {
  it(e, !0), vt(t, ma);
  let n = R(e, "value", 15, !1), r = R(e, "label", 7, ""), i = R(e, "disabled", 7, !1);
  const s = e.$$host;
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
  }, o = ga(), u = q(o);
  Xn(u);
  var v = ke(u, 2);
  {
    var b = (d) => {
      var g = pa(), _ = q(g, !0);
      V(g), me(() => Re(_, r())), te(d, g);
    };
    yn(v, (d) => {
      r() && d(b);
    });
  }
  return V(o), me(() => {
    vs(u, n()), u.disabled = i();
  }), ee("change", u, l), te(t, o), st(a);
}
Rt(["change"]);
customElements.define("xi-toggle", pt(
  _a,
  {
    value: { reflect: !0, type: "Boolean" },
    label: {},
    disabled: {}
  },
  [],
  [],
  { mode: "open" }
));
function gs(t) {
  let e = t;
  if (typeof t == "string")
    try {
      e = JSON.parse(t);
    } catch {
      e = [];
    }
  return Array.isArray(e) ? e.map((n) => n && typeof n == "object" ? { value: n.value, label: n.label ?? String(n.value) } : { value: n, label: String(n) }) : [];
}
var ba = /* @__PURE__ */ le('<span class="lbl"> </span>'), ya = /* @__PURE__ */ le('<label class="opt svelte-mtcxej"><input type="radio" class="svelte-mtcxej"/> <span> </span></label>'), wa = /* @__PURE__ */ le('<div class="xi-radio svelte-mtcxej" role="radiogroup"><!> <!></div>');
const xa = {
  hash: "svelte-mtcxej",
  code: ".xi-radio.svelte-mtcxej {display:inline-flex;align-items:center;gap:0.75rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}.opt.svelte-mtcxej {display:inline-flex;align-items:center;gap:0.25rem;cursor:pointer;}input.svelte-mtcxej {accent-color:var(--xi-accent, #3b82f6);}"
};
function Ea(t, e) {
  it(e, !0), vt(t, xa);
  let n = R(e, "value", 15, ""), r = R(e, "options", 23, () => []), i = R(e, "label", 7, ""), s = R(e, "disabled", 7, !1), l = R(e, "name", 7, "xi-radio");
  const a = e.$$host, o = /* @__PURE__ */ Ai(() => gs(r()));
  function u(h) {
    n(h), a.dispatchEvent(new CustomEvent("change", { detail: { value: n() }, bubbles: !0, composed: !0 }));
  }
  var v = {
    get value() {
      return n();
    },
    set value(h = "") {
      n(h), O();
    },
    get options() {
      return r();
    },
    set options(h = []) {
      r(h), O();
    },
    get label() {
      return i();
    },
    set label(h = "") {
      i(h), O();
    },
    get disabled() {
      return s();
    },
    set disabled(h = !1) {
      s(h), O();
    },
    get name() {
      return l();
    },
    set name(h = "xi-radio") {
      l(h), O();
    }
  }, b = wa(), d = q(b);
  {
    var g = (h) => {
      var p = ba(), y = q(p, !0);
      V(p), me(() => Re(y, i())), te(h, p);
    };
    yn(d, (h) => {
      i() && h(g);
    });
  }
  var _ = ke(d, 2);
  return ds(_, 17, () => N(o), cs, (h, p) => {
    var y = ya(), w = q(y);
    Xn(w);
    var c = ke(w, 2), x = q(c, !0);
    V(c), V(y), me(() => {
      tt(w, "name", l()), Dr(w, N(p).value), vs(w, N(p).value === n()), w.disabled = s(), Re(x, N(p).label);
    }), ee("change", w, () => u(N(p).value)), te(h, y);
  }), V(b), te(t, b), st(v);
}
Rt(["change"]);
customElements.define("xi-radio", pt(
  Ea,
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
var ka = /* @__PURE__ */ le('<span class="lbl"> </span>'), $a = /* @__PURE__ */ le("<option> </option>"), Ta = /* @__PURE__ */ le('<label class="xi-dropdown svelte-1wd9iqr"><!> <select class="svelte-1wd9iqr"></select></label>');
const Sa = {
  hash: "svelte-1wd9iqr",
  code: ".xi-dropdown.svelte-1wd9iqr {display:inline-flex;align-items:center;gap:0.5rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}select.svelte-1wd9iqr {padding:0.15rem 0.3rem;}"
};
function Ca(t, e) {
  it(e, !0), vt(t, Sa);
  let n = R(e, "value", 15, ""), r = R(e, "options", 23, () => []), i = R(e, "label", 7, ""), s = R(e, "disabled", 7, !1);
  const l = e.$$host, a = /* @__PURE__ */ Ai(() => gs(r()));
  function o(h) {
    n(h.target.value), l.dispatchEvent(new CustomEvent("change", { detail: { value: n() }, bubbles: !0, composed: !0 }));
  }
  var u = {
    get value() {
      return n();
    },
    set value(h = "") {
      n(h), O();
    },
    get options() {
      return r();
    },
    set options(h = []) {
      r(h), O();
    },
    get label() {
      return i();
    },
    set label(h = "") {
      i(h), O();
    },
    get disabled() {
      return s();
    },
    set disabled(h = !1) {
      s(h), O();
    }
  }, v = Ta(), b = q(v);
  {
    var d = (h) => {
      var p = ka(), y = q(p, !0);
      V(p), me(() => Re(y, i())), te(h, p);
    };
    yn(b, (h) => {
      i() && h(d);
    });
  }
  var g = ke(b, 2);
  ds(g, 21, () => N(a), cs, (h, p) => {
    var y = $a(), w = q(y, !0);
    V(y);
    var c = {};
    me(() => {
      na(y, N(p).value === n()), Re(w, N(p).label), c !== (c = N(p).value) && (y.value = (y.__value = N(p).value) ?? "");
    }), te(h, y);
  }), V(g);
  var _;
  return Jl(g), V(v), me(() => {
    g.disabled = s(), _ !== (_ = n()) && (g.value = (g.__value = n()) ?? "", hs(g, n()));
  }), ee("change", g, o), te(t, v), st(u);
}
Rt(["change"]);
customElements.define("xi-dropdown", pt(
  Ca,
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
var Ma = /* @__PURE__ */ le('<div class="xi-trace svelte-1m1z2sd"><span class="lbl"> </span> <canvas class="svelte-1m1z2sd"></canvas> <span class="val svelte-1m1z2sd"> </span></div>');
const Aa = {
  hash: "svelte-1m1z2sd",
  code: ".xi-trace.svelte-1m1z2sd {display:inline-flex;align-items:center;gap:0.5rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}canvas.svelte-1m1z2sd {width:120px;height:28px;display:block;}.val.svelte-1m1z2sd {min-width:3em;text-align:right;font-variant-numeric:tabular-nums;}"
};
function Ia(t, e) {
  it(e, !0), vt(t, Aa);
  let n = R(e, "key", 7, ""), r = R(e, "label", 7, ""), i = R(e, "max", 7, 60);
  const s = e.$$host;
  let l, a = /* @__PURE__ */ De(null), o = /* @__PURE__ */ De(mt([]));
  function u() {
    if (!l) return;
    const c = l.getContext && l.getContext("2d");
    if (!c) return;
    const x = l.width = l.clientWidth || 120, m = l.height = l.clientHeight || 28;
    if (c.clearRect(0, 0, x, m), N(o).length < 2) return;
    const k = Math.min(...N(o)), C = Math.max(...N(o)), D = C - k || 1;
    c.beginPath(), N(o).forEach((P, U) => {
      const H = U / (N(o).length - 1) * (x - 2) + 1, z = m - 2 - (P - k) / D * (m - 4);
      U ? c.lineTo(H, z) : c.moveTo(H, z);
    }), c.strokeStyle = getComputedStyle(s).getPropertyValue("--xi-accent") || "#3b82f6", c.lineWidth = 1.5, c.stroke();
  }
  function v(c) {
    const x = c && c[n()];
    x && (Ae(a, x.value, !0), typeof x.value == "number" && Number.isFinite(x.value) && (Ae(o, [...N(o), x.value].slice(-i()), !0), u()), s.dispatchEvent(new CustomEvent("sample", { detail: { value: x.value }, bubbles: !0, composed: !0 })));
  }
  Ir(() => {
    s.update = v, Object.defineProperty(s, "latest", { get: () => N(a), configurable: !0 }), Object.defineProperty(s, "history", { get: () => N(o).slice(), configurable: !0 }), u();
  });
  const b = (c) => c == null ? "—" : typeof c == "number" ? Number.isInteger(c) ? c : c.toFixed(3) : String(c);
  var d = {
    get key() {
      return n();
    },
    set key(c = "") {
      n(c), O();
    },
    get label() {
      return r();
    },
    set label(c = "") {
      r(c), O();
    },
    get max() {
      return i();
    },
    set max(c = 60) {
      i(c), O();
    }
  }, g = Ma(), _ = q(g), h = q(_, !0);
  V(_);
  var p = ke(_, 2);
  jr(p, (c) => l = c, () => l);
  var y = ke(p, 2), w = q(y, !0);
  return V(y), V(g), me(
    (c) => {
      Re(h, r() || n()), Re(w, c);
    },
    [() => b(N(a))]
  ), te(t, g), st(d);
}
customElements.define("xi-trace", pt(Ia, { key: {}, label: {}, max: { type: "Number" } }, [], [], { mode: "open" }));
function ms() {
  return { scale: 1, panX: 0, panY: 0, imgW: 0, imgH: 0, viewW: 0, viewH: 0 };
}
function Fr(t, e, n) {
  return { x: (e - t.panX) / t.scale, y: (n - t.panY) / t.scale };
}
function Na(t, e, n) {
  return { x: t.panX + e * t.scale, y: t.panY + n * t.scale };
}
const Oa = 0.05, Ra = 64, La = (t) => Math.max(Oa, Math.min(Ra, t));
function gr(t) {
  return !t.imgW || !t.imgH || !t.viewW || !t.viewH || (t.scale = Math.min(t.viewW / t.imgW, t.viewH / t.imgH) * 0.95, t.panX = (t.viewW - t.imgW * t.scale) / 2, t.panY = (t.viewH - t.imgH * t.scale) / 2), t;
}
function Pa(t) {
  return t.scale = 1, t.panX = (t.viewW - t.imgW) / 2, t.panY = (t.viewH - t.imgH) / 2, t;
}
function _s(t, e, n, r) {
  const { x: i, y: s } = Fr(t, e, n);
  return t.scale = La(t.scale * r), t.panX = e - i * t.scale, t.panY = n - s * t.scale, t;
}
function Da(t, e, n) {
  return t.panX += e, t.panY += n, t;
}
var Ha = /* @__PURE__ */ le('<canvas class="svelte-1yjweo0"></canvas>');
const ja = {
  hash: "svelte-1yjweo0",
  code: ":host {display:block;width:100%;height:100%;}canvas.svelte-1yjweo0 {width:100%;height:100%;display:block;background:var(--xi-viewer-bg, #111);cursor:grab;touch-action:none;image-rendering:pixelated;}canvas.svelte-1yjweo0:active {cursor:grabbing;}"
};
function Fa(t, e) {
  it(e, !0), vt(t, ja);
  const n = e.$$host;
  let r;
  const i = ms();
  let s = null, l = null;
  function a() {
    if (!r) return;
    const m = r.getContext("2d");
    m.imageSmoothingEnabled = !1, m.clearRect(0, 0, r.width, r.height), s && (m.setTransform(i.scale, 0, 0, i.scale, i.panX, i.panY), m.drawImage(s, 0, 0), m.setTransform(1, 0, 0, 1, 0, 0));
  }
  function o() {
    if (!r) return;
    const m = r.getBoundingClientRect();
    r.width = Math.max(1, Math.round(m.width)), r.height = Math.max(1, Math.round(m.height)), i.viewW = r.width, i.viewH = r.height, a();
  }
  function u(m, k) {
    n.dispatchEvent(new CustomEvent(m, { detail: k, bubbles: !0, composed: !0 }));
  }
  function v(m) {
    return !!m && typeof m != "string" && !("dataUrl" in m) && (typeof HTMLImageElement < "u" && m instanceof HTMLImageElement || typeof HTMLCanvasElement < "u" && m instanceof HTMLCanvasElement || typeof OffscreenCanvas < "u" && m instanceof OffscreenCanvas || typeof ImageBitmap < "u" && m instanceof ImageBitmap);
  }
  function b(m) {
    if (v(m)) {
      d(m);
      return;
    }
    const k = new Image();
    k.onload = () => d(k), k.src = typeof m == "string" ? m : m.dataUrl;
  }
  function d(m) {
    const k = !i.imgW;
    s = m, i.imgW = m.naturalWidth || m.width, i.imgH = m.naturalHeight || m.height, l = document.createElement("canvas"), l.width = i.imgW, l.height = i.imgH, l.getContext("2d").drawImage(m, 0, 0), k && gr(i), a();
  }
  function g(m) {
    if (!s) return;
    m.preventDefault();
    const k = r.getBoundingClientRect();
    _s(i, m.clientX - k.left, m.clientY - k.top, m.deltaY < 0 ? 1.15 : 1 / 1.15), a(), u("viewchange", { scale: i.scale });
  }
  let _ = null, h = !1;
  function p(m) {
    var k;
    s && (_ = { x: m.clientX, y: m.clientY }, h = !1, (k = r.setPointerCapture) == null || k.call(r, m.pointerId));
  }
  function y(m) {
    if (!_) return;
    const k = m.clientX - _.x, C = m.clientY - _.y;
    (k || C) && (h = !0), Da(i, k, C), _ = { x: m.clientX, y: m.clientY }, a();
  }
  function w(m) {
    _ && !h && c(m), _ = null;
  }
  function c(m) {
    if (!s || !l) return;
    const k = r.getBoundingClientRect(), C = Fr(i, m.clientX - k.left, m.clientY - k.top), D = Math.floor(C.x), P = Math.floor(C.y);
    let U = null;
    if (D >= 0 && P >= 0 && D < i.imgW && P < i.imgH) {
      const H = l.getContext("2d").getImageData(D, P, 1, 1).data;
      U = [H[0], H[1], H[2]];
    }
    u("pixelpick", { x: D, y: P, rgb: U });
  }
  Ir(() => {
    n.setFrame = b, n.fit = () => {
      gr(i), a(), u("viewchange", { scale: i.scale });
    }, n.oneToOne = () => {
      Pa(i), a(), u("viewchange", { scale: i.scale });
    }, o();
    const m = new ResizeObserver(o);
    return m.observe(r), () => m.disconnect();
  });
  var x = Ha();
  jr(x, (m) => r = m, () => r), os("wheel", x, g), ee("pointerdown", x, p), ee("pointermove", x, y), ee("pointerup", x, w), te(t, x), st();
}
Rt(["pointerdown", "pointermove", "pointerup"]);
customElements.define("xi-image-viewer", pt(Fa, {}, [], [], { mode: "open" }));
function Wa() {
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
function za() {
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
      const l = s(t), a = s(e);
      i.strokeStyle = "#f59e0b", i.lineWidth = 1.5, i.strokeRect(Math.min(l.x, a.x), Math.min(l.y, a.y), Math.abs(a.x - l.x), Math.abs(a.y - l.y));
    }
  };
}
function Ba() {
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
          const l = r({ x: i[0], y: i[1] });
          s ? n.lineTo(l.x, l.y) : n.moveTo(l.x, l.y);
        }), e && n.closePath(), n.stroke();
        for (const i of t) {
          const s = r({ x: i[0], y: i[1] });
          n.beginPath(), n.arc(s.x, s.y, 3, 0, Math.PI * 2), n.fill();
        }
      }
    }
  };
}
const mr = { point: Wa, rect: za, polygon: Ba };
function uo(t, e) {
  mr[t] = e;
}
function ii(t) {
  return mr[t] ? mr[t]() : null;
}
var Ya = /* @__PURE__ */ le('<div class="xi-editor svelte-1qrk1oj"><div class="bar svelte-1qrk1oj"><span class="lbl"> </span> <span class="spacer svelte-1qrk1oj"></span> <button class="cancel svelte-1qrk1oj">Cancel</button> <button class="commit svelte-1qrk1oj">Commit</button></div> <canvas class="svelte-1qrk1oj"></canvas></div>');
const Va = {
  hash: "svelte-1qrk1oj",
  code: ":host {display:block;width:100%;height:100%;}.xi-editor.svelte-1qrk1oj {display:flex;flex-direction:column;height:100%;}.bar.svelte-1qrk1oj {display:flex;align-items:center;gap:0.5rem;padding:0.35rem 0.5rem;font:13px system-ui, sans-serif;background:var(--xi-bar-bg, #1f2937);color:#e5e7eb;}.spacer.svelte-1qrk1oj {flex:1;}button.svelte-1qrk1oj {padding:0.2rem 0.7rem;border:0;border-radius:4px;cursor:pointer;font:inherit;}.commit.svelte-1qrk1oj {background:var(--xi-accent, #6366f1);color:#fff;}.cancel.svelte-1qrk1oj {background:#374151;color:#e5e7eb;}canvas.svelte-1qrk1oj {flex:1;width:100%;display:block;background:var(--xi-viewer-bg, #111);cursor:crosshair;touch-action:none;image-rendering:pixelated;}"
};
function qa(t, e) {
  it(e, !0), vt(t, Va);
  let n = R(e, "tool", 7, "rect"), r = R(e, "label", 7, "");
  const i = e.$$host;
  let s;
  const l = ms();
  let a = null, o = ii(n());
  const u = (E) => Na(l, E.x, E.y);
  function v() {
    if (!s) return;
    const E = s.getContext("2d");
    E && (E.imageSmoothingEnabled = !1, E.setTransform(1, 0, 0, 1, 0, 0), E.clearRect(0, 0, s.width, s.height), a && (E.setTransform(l.scale, 0, 0, l.scale, l.panX, l.panY), E.drawImage(a, 0, 0), E.setTransform(1, 0, 0, 1, 0, 0)), o && o.draw(E, u));
  }
  function b() {
    if (!s) return;
    const E = s.getBoundingClientRect();
    s.width = Math.max(1, Math.round(E.width)), s.height = Math.max(1, Math.round(E.height)), l.viewW = s.width, l.viewH = s.height, v();
  }
  function d(E) {
    return !!E && typeof E != "string" && !("dataUrl" in E) && (typeof HTMLImageElement < "u" && E instanceof HTMLImageElement || typeof HTMLCanvasElement < "u" && E instanceof HTMLCanvasElement || typeof OffscreenCanvas < "u" && E instanceof OffscreenCanvas || typeof ImageBitmap < "u" && E instanceof ImageBitmap);
  }
  function g(E) {
    if (d(E)) {
      _(E);
      return;
    }
    const ae = new Image();
    ae.onload = () => _(ae), ae.src = typeof E == "string" ? E : E.dataUrl;
  }
  function _(E) {
    const ae = !l.imgW;
    a = E, l.imgW = E.naturalWidth || E.width, l.imgH = E.naturalHeight || E.height, ae && gr(l), v();
  }
  function h(E) {
    o = ii(E) || o, v();
  }
  const p = (E) => {
    const ae = s.getBoundingClientRect();
    return Fr(l, E.clientX - ae.left, E.clientY - ae.top);
  };
  function y(E) {
    o && (o.onDown(p(E)), v());
  }
  function w(E) {
    o && E.buttons && (o.onMove(p(E)), v());
  }
  function c(E) {
    o && (o.onUp(p(E)), v());
  }
  function x(E) {
    o && (o.onDbl(p(E)), v());
  }
  function m(E) {
    if (!a) return;
    E.preventDefault();
    const ae = s.getBoundingClientRect();
    _s(l, E.clientX - ae.left, E.clientY - ae.top, E.deltaY < 0 ? 1.15 : 1 / 1.15), v();
  }
  function k() {
    !o || !o.done() || i.dispatchEvent(new CustomEvent("commit", {
      detail: { tool: o.type, result: o.result() },
      bubbles: !0,
      composed: !0
    }));
  }
  function C() {
    i.dispatchEvent(new CustomEvent("cancel", { detail: {}, bubbles: !0, composed: !0 }));
  }
  Ir(() => {
    i.setFrame = g, i.setTool = h, i.getResult = () => o && o.done() ? o.result() : null, b();
    const E = new ResizeObserver(b);
    return E.observe(s), () => E.disconnect();
  });
  var D = {
    get tool() {
      return n();
    },
    set tool(E = "rect") {
      n(E), O();
    },
    get label() {
      return r();
    },
    set label(E = "") {
      r(E), O();
    }
  }, P = Ya(), U = q(P), H = q(U), z = q(H, !0);
  V(H);
  var Q = ke(H, 4), Pe = ke(Q, 2);
  V(U);
  var qe = ke(U, 2);
  return jr(qe, (E) => s = E, () => s), V(P), me(() => Re(z, r() || n())), ee("click", Q, C), ee("click", Pe, k), ee("pointerdown", qe, y), ee("pointermove", qe, w), ee("pointerup", qe, c), ee("dblclick", qe, x), os("wheel", qe, m), te(t, P), st(D);
}
Rt([
  "click",
  "pointerdown",
  "pointermove",
  "pointerup",
  "dblclick"
]);
customElements.define("xi-image-editor", pt(qa, { tool: {}, label: {} }, [], [], { mode: "open" }));
function bs(t) {
  const e = typeof t == "string" ? JSON.parse(t) : t, n = e.items || e.vars || [], r = {};
  for (const i of n) r[i.name] = i;
  return { run_id: e.run_id, items: r };
}
const Ua = { 0: "image/jpeg", 1: "image/bmp", 2: "image/png" };
function ys(t) {
  const e = t instanceof Uint8Array ? t : new Uint8Array(t);
  if (e.byteLength < 20) throw new Error("preview frame shorter than 20-byte header");
  const n = new DataView(e.buffer, e.byteOffset, e.byteLength), r = n.getUint32(0, !1), i = n.getUint32(4, !1), s = n.getUint32(8, !1), l = n.getUint32(12, !1), a = n.getUint32(16, !1), o = e.subarray(20), u = Ua[i] || "application/octet-stream";
  return {
    gid: r,
    codec: i,
    width: s,
    height: l,
    channels: a,
    dataUrl: `data:${u};base64,${ws(o)}`
  };
}
function ws(t) {
  if (typeof Buffer < "u") return Buffer.from(t).toString("base64");
  let e = "";
  const n = 32768;
  for (let r = 0; r < t.length; r += n)
    e += String.fromCharCode.apply(null, t.subarray(r, r + n));
  return btoa(e);
}
const co = /* @__PURE__ */ Object.freeze(/* @__PURE__ */ Object.defineProperty({
  __proto__: null,
  bytesToBase64: ws,
  decodePreviewFrame: ys,
  parseVars: bs
}, Symbol.toStringTag, { value: "Module" }));
class ho {
  /**
   * @param {string} url  e.g. "ws://127.0.0.1:7823/"
   * @param {{WebSocketImpl?: any}} [opts]  inject a WebSocket impl (node tests)
   */
  constructor(e, n = {}) {
    if (this.url = e, this._WS = n.WebSocketImpl || (typeof WebSocket < "u" ? WebSocket : null), !this._WS) throw new Error("no WebSocket implementation (pass opts.WebSocketImpl in node)");
    this.ws = null, this._id = 0, this._pending = /* @__PURE__ */ new Map(), this._listeners = {
      // type -> Set<cb>
      vars: /* @__PURE__ */ new Set(),
      instances: /* @__PURE__ */ new Set(),
      log: /* @__PURE__ */ new Set(),
      event: /* @__PURE__ */ new Set(),
      preview: /* @__PURE__ */ new Set(),
      hello: /* @__PURE__ */ new Set()
    }, this._imgSubs = /* @__PURE__ */ new Map();
  }
  // --- preview subscription (ref-counted) ---------------------------------
  // The backend sends NO image previews by default — encode + transmit only
  // happens for names someone is actually viewing. A component calls
  // subscribeImage(name) when it shows an image and unsubscribeImage(name) when
  // it hides/unmounts; we ref-count so M viewers of one name → one subscription,
  // and push the union to the backend. Re-asserted on (re)connect.
  subscribeImage(e) {
    if (!e) return;
    const n = (this._imgSubs.get(e) || 0) + 1;
    this._imgSubs.set(e, n), n === 1 && this._pushImageSubs();
  }
  unsubscribeImage(e) {
    if (!e) return;
    const n = (this._imgSubs.get(e) || 0) - 1;
    n <= 0 ? this._imgSubs.delete(e) && this._pushImageSubs() : this._imgSubs.set(e, n);
  }
  _pushImageSubs() {
    !this.ws || this.ws.readyState !== 1 || this.cmd("subscribe", { names: [...this._imgSubs.keys()] }).catch(() => {
    });
  }
  // Open the socket; resolves once it's open. If opts.checkVersion is set, also
  // runs `cmd:version` and rejects on mismatch (fail-fast on protocol drift).
  //   checkVersion: (info) => boolean | RegExp | string   (string/RegExp tests info.version)
  connect(e = {}) {
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
          if (e.checkVersion) {
            const s = await this.cmd("version"), l = s && s.version;
            if (!(typeof e.checkVersion == "function" ? e.checkVersion(s) : e.checkVersion instanceof RegExp ? e.checkVersion.test(l) : l === e.checkVersion)) {
              r(new Error(`backend version mismatch: got ${l}`)), i.close();
              return;
            }
          }
          this._imgSubs.size && this._pushImageSubs(), n(this);
        } catch (s) {
          r(s);
        }
      };
    });
  }
  _onMessage(e) {
    const n = e.data;
    if (n instanceof ArrayBuffer || typeof Buffer < "u" && n instanceof Buffer) {
      try {
        this._deliverPreview(ys(n));
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
      this._emit("vars", bs(r));
      return;
    }
    this._listeners[r.type] && this._emit(r.type, r);
  }
  // Decode each preview frame ONCE here and share the GC-managed <img> to every
  // consumer (the backend already sends one frame per canon image, so this is one
  // JPEG decode per image instead of one per component). Consumers hold the
  // reference while they show it; GC reclaims when nothing references it — no
  // close, no ring. In non-browser/jsdom (tests) we skip the decode and emit the
  // dataUrl as before; a consumer there falls back to decoding it itself.
  _deliverPreview(e) {
    if (!(typeof window < "u" && typeof Image < "u" && !/jsdom/i.test(typeof navigator < "u" && navigator.userAgent || ""))) {
      this._emit("preview", e);
      return;
    }
    const r = new Image();
    let i = !1;
    const s = () => {
      i || (i = !0, this._emit("preview", e));
    };
    r.onload = () => {
      e.image = r, s();
    }, r.onerror = s, r.src = e.dataUrl;
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
      const l = { type: "cmd", id: r, name: e };
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
  getInstanceDef(e) {
    return this.cmd("get_instance_def", { name: e });
  }
  setInstanceDef(e, n) {
    return this.cmd("set_instance_def", { name: e, def: n });
  }
  exchange(e, n) {
    return this.cmd("exchange_instance", { name: e, cmd: n });
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
  onVars(e) {
    return this.on("vars", e);
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
  onPreview(e) {
    return this.on("preview", e);
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
const Xa = {
  slider: "xi-slider",
  number: "xi-number",
  toggle: "xi-toggle",
  radio: "xi-radio",
  dropdown: "xi-dropdown"
};
function Ga(t, { section: e = "Config", tag: n = "control" } = {}) {
  const r = [];
  for (const [i, s] of Object.entries(t || {})) {
    let l = "number";
    if (typeof s == "boolean") l = "toggle";
    else if (typeof s == "string") l = "text";
    else if (typeof s == "number") l = "number";
    else continue;
    r.push({ type: l, key: i, label: i });
  }
  return r.length ? [{ section: e, tag: n, controls: r }] : [];
}
async function vo(t, e) {
  const { client: n, instance: r, sectionFilter: i } = e, s = t.ownerDocument || globalThis.document, l = await n.getInstanceDef(r) || {}, a = { ...l }, o = e.descriptor && e.descriptor.length ? e.descriptor : Ga(l), u = [];
  t.innerHTML = "";
  for (const v of o) {
    if (i && !i(v)) continue;
    const b = s.createElement("section");
    if (b.className = "xi-section", b.dataset.tag = v.tag || "control", v.section) {
      const d = s.createElement("h3");
      d.className = "xi-section-title", d.textContent = v.section, b.appendChild(d);
    }
    for (const d of v.controls || []) {
      const g = Xa[d.type] || "xi-number", _ = s.createElement(g);
      d.label && _.setAttribute("label", d.label);
      for (const p of ["min", "max", "step"]) d[p] != null && _.setAttribute(p, String(d[p]));
      const h = s.createElement("div");
      h.className = "xi-control", h.appendChild(_), b.appendChild(h), d.options != null && (_.options = d.options), d.key in a && (_.value = a[d.key]), _.addEventListener("change", async (p) => {
        a[d.key] = p.detail.value;
        try {
          await n.setInstanceDef(r, { ...a });
        } catch {
        }
        t.dispatchEvent(new CustomEvent("xi-change", { detail: { key: d.key, value: p.detail.value }, bubbles: !0 }));
      }), u.push({ el: _, key: d.key });
    }
    t.appendChild(b);
  }
  return {
    // Re-read the def from the backend and push values back into the controls.
    async refresh() {
      const v = await n.getInstanceDef(r) || {};
      Object.assign(a, v);
      for (const { el: b, key: d } of u) d in a && (b.value = a[d]);
    },
    destroy() {
      t.innerHTML = "";
    }
  };
}
function po(t) {
  const e = [];
  for (const n of t || [])
    if ((n.tag || "") === "status")
      for (const r of n.controls || [])
        e.push({
          type: r.type === "image" ? "image" : r.type === "trace" ? "trace" : "value",
          key: r.key,
          label: r.label || r.key
        });
  return e;
}
function go(t, { client: e, items: n, columns: r = 3 }) {
  var _;
  const i = t.ownerDocument || globalThis.document;
  t.innerHTML = "";
  const s = i.createElement("div");
  s.className = "xi-monitor", s.style.display = "grid", s.style.gap = "0.75rem", s.style.gridTemplateColumns = `repeat(${r}, minmax(0, 1fr))`, t.appendChild(s);
  const l = /* @__PURE__ */ new Map(), a = /* @__PURE__ */ new Map(), o = /* @__PURE__ */ new Map();
  for (const h of n) {
    const p = i.createElement("div");
    p.className = "xi-tile", p.dataset.key = h.key;
    const y = i.createElement("div");
    y.className = "xi-tile-label", y.textContent = h.label, p.appendChild(y);
    let w;
    h.type === "trace" ? (w = i.createElement("xi-trace"), w.setAttribute("key", h.key)) : h.type === "image" ? (w = i.createElement("xi-image-viewer"), w.style.height = "180px") : (w = i.createElement("div"), w.className = "xi-value", w.textContent = "—"), p.appendChild(w), s.appendChild(p), l.set(h.key, { type: h.type, el: w });
  }
  const u = n.filter((h) => h.type === "image").map((h) => h.key);
  for (const h of u) (_ = e.subscribeImage) == null || _.call(e, h);
  const v = (h) => {
    const p = h.items || {};
    a.clear(), o.clear();
    for (const [y, w] of l) {
      const c = p[y];
      if (c)
        if (w.type === "trace") w.el.update(p);
        else if (w.type === "image") {
          if (c.gid != null) {
            const x = c.src != null ? c.src : c.gid;
            o.set(c.gid, x);
            let m = a.get(x);
            m || a.set(x, m = /* @__PURE__ */ new Set()), m.add(w.el);
          }
        } else w.el.textContent = Ka(c.value);
    }
  }, b = (h) => {
    const p = o.has(h.gid) ? o.get(h.gid) : h.gid, y = a.get(p), w = h.image || h.dataUrl;
    if (y) for (const c of y) c.setFrame(w);
  }, d = e.onVars(v), g = e.onPreview(b);
  return { destroy() {
    var h;
    d(), g();
    for (const p of u) (h = e.unsubscribeImage) == null || h.call(e, p);
    t.innerHTML = "";
  } };
}
function Ka(t) {
  return t == null ? "—" : typeof t == "number" ? Number.isInteger(t) ? String(t) : t.toFixed(3) : typeof t == "boolean" ? t ? "true" : "false" : String(t);
}
const Ja = `
  :host { display:block; height:100%; box-sizing:border-box; background:#1e1e1e;
          border:1px solid #333; border-radius:6px; color:#ddd; overflow:hidden;
          font:13px/1.4 system-ui,sans-serif; }
  .hd { padding:4px 8px; font-size:11px; letter-spacing:.04em; text-transform:uppercase;
        color:#888; border-bottom:1px solid #2a2a2a; }
  .body { padding:8px; height:calc(100% - 24px); box-sizing:border-box; }
`;
function Lt(t, e) {
  return t.attachShadow({ mode: "open" }), t.shadowRoot.innerHTML = `<style>${Ja}</style>
    <div class="hd">${e || ""}</div><div class="body"></div>`, t.shadowRoot.querySelector(".body");
}
const Gn = (t, e) => t.config && t.config.title || t.binding && t.binding.var || e, Kn = (t, e) => e && t.vars[e.var] ? t.vars[e.var].value : void 0;
function xs(t) {
  return t == null ? { kind: "none", label: "—", color: "#bbb" } : t <= -99e4 ? { kind: "sys", label: "SYS", color: "#c084fc" } : t > 0 ? { kind: "ok", label: t > 1 ? `OK${t}` : "OK", color: "#3ad17a" } : t < 0 ? { kind: "ng", label: t < -1 ? `NG${-t}` : "NG", color: "#ff5b5b" } : { kind: "na", label: "NA", color: "#ffb454" };
}
const Es = (t) => !t || t.result === !0 || !t.var;
class Za extends HTMLElement {
  connectedCallback() {
    this.body = Lt(this, Gn(this, "Verdict")), this.body.style.cssText = "display:flex;flex-direction:column;align-items:center;justify-content:center;gap:6px;text-align:center", this.big = document.createElement("div"), this.big.style.cssText = "font-weight:800;font-size:clamp(20px,7vw,72px);line-height:1", this.sub = document.createElement("div"), this.sub.style.cssText = "font-size:12px;color:#888;max-width:96%;overflow:hidden;text-overflow:ellipsis;white-space:nowrap", this.body.append(this.big, this.sub);
  }
  feed(e) {
    const n = this.binding || {};
    if (Es(n)) {
      const l = e.result, a = xs(l ? l.code : null);
      this.big.textContent = a.label, this.big.style.color = a.color, this.sub.textContent = l && l.msg ? l.msg : "";
      return;
    }
    const r = Kn(e, n), i = r === !0 || r === "OK" || r === "ok" || r === "PASS", s = r === !1 || r === "NG" || r === "ng" || r === "FAIL";
    this.big.textContent = r === void 0 ? "—" : i ? "OK" : s ? "NG" : String(r), this.big.style.color = i ? "#3ad17a" : s ? "#ff5b5b" : "#ccc", this.sub.textContent = "";
  }
}
class Qa extends HTMLElement {
  connectedCallback() {
    this.body = Lt(this, Gn(this, "Value")), this.body.style.cssText = "display:flex;align-items:center;justify-content:center;font-size:clamp(16px,5vw,40px);font-weight:600";
  }
  feed(e) {
    var r;
    const n = Kn(e, this.binding);
    this.body.textContent = n === void 0 ? "—" : typeof n == "number" ? +n.toFixed(((r = this.config) == null ? void 0 : r.decimals) ?? 3) : String(n);
  }
}
class eo extends HTMLElement {
  connectedCallback() {
    this.body = Lt(this, Gn(this, "Image")), this.body.style.cssText = "padding:0", this.viewer = document.createElement("xi-image-viewer"), this.viewer.style.cssText = "width:100%;height:100%;display:block", this.body.appendChild(this.viewer);
  }
  feed(e) {
    const n = this.binding && e.vars[this.binding.var], r = n ? n.src != null ? n.src : n.gid : void 0, i = n && r != null ? e.images[r] : void 0;
    i && i !== this._u && typeof this.viewer.setFrame == "function" && (this.viewer.setFrame(i), this._u = i);
  }
}
class to extends HTMLElement {
  connectedCallback() {
    this.body = Lt(this, Gn(this, "SPC")), this.buf = [], this.last = -1, this.cv = document.createElement("canvas"), this.cv.style.cssText = "width:100%;height:100%", this.body.appendChild(this.cv);
  }
  feed(e) {
    var n;
    if (e.run_id !== this.last) {
      this.last = e.run_id;
      const r = Kn(e, this.binding);
      if (typeof r == "number") {
        this.buf.push(r);
        const i = ((n = this.config) == null ? void 0 : n.window) || 100;
        this.buf.length > i && this.buf.shift();
      }
    }
    this.draw();
  }
  draw() {
    var d, g, _;
    const e = this.cv, n = e.getBoundingClientRect();
    if (!n.width) return;
    e.width = n.width, e.height = n.height;
    const r = e.getContext("2d");
    if (r.clearRect(0, 0, e.width, e.height), !this.buf.length) return;
    const i = ((d = this.config) == null ? void 0 : d.mean) ?? this.buf.reduce((h, p) => h + p, 0) / this.buf.length, s = (g = this.config) == null ? void 0 : g.ucl, l = (_ = this.config) == null ? void 0 : _.lcl;
    let a = Math.min(...this.buf), o = Math.max(...this.buf);
    s != null && (o = Math.max(o, s)), l != null && (a = Math.min(a, l));
    const u = (o - a) * 0.1 || 1;
    a -= u, o += u;
    const v = (h) => e.height - (h - a) / (o - a) * e.height, b = (h, p, y) => {
      h != null && (r.strokeStyle = p, r.setLineDash(y || []), r.beginPath(), r.moveTo(0, v(h)), r.lineTo(e.width, v(h)), r.stroke(), r.setLineDash([]));
    };
    b(i, "#666"), b(s, "#ff5b5b", [4, 3]), b(l, "#ff5b5b", [4, 3]), r.strokeStyle = "#4aa0f0", r.lineWidth = 1.5, r.beginPath(), this.buf.forEach((h, p) => {
      const y = p / Math.max(1, this.buf.length - 1) * e.width;
      p ? r.lineTo(y, v(h)) : r.moveTo(y, v(h));
    }), r.stroke();
  }
}
class no extends HTMLElement {
  connectedCallback() {
    var e;
    this.body = Lt(this, ((e = this.config) == null ? void 0 : e.title) || "Throughput"), this.buf = [], this.last = -1, this.body.style.cssText = "display:flex;flex-direction:column;align-items:center;justify-content:center;gap:4px", this.big = document.createElement("div"), this.big.style.cssText = "font-size:clamp(18px,6vw,44px);font-weight:700;color:#9ad", this.sub = document.createElement("div"), this.sub.style.cssText = "font-size:12px;color:#888", this.body.append(this.big, this.sub);
  }
  feed(e) {
    if (e.run_id !== this.last && e.run_ms != null && (this.last = e.run_id, this.buf.push(e.run_ms), this.buf.length > 30 && this.buf.shift()), this.buf.length) {
      const n = this.buf.reduce((r, i) => r + i, 0) / this.buf.length;
      this.big.textContent = `${(6e4 / n).toFixed(0)} /min`, this.sub.textContent = `cycle ${n.toFixed(1)} ms`;
    }
  }
}
class ro extends HTMLElement {
  connectedCallback() {
    var e;
    this.body = Lt(this, ((e = this.config) == null ? void 0 : e.title) || "Yield"), this.ok = 0, this.ng = 0, this.last = -1, this.body.style.cssText = "display:flex;flex-direction:column;align-items:center;justify-content:center;gap:4px", this.big = document.createElement("div"), this.big.style.cssText = "font-size:clamp(18px,6vw,44px);font-weight:700", this.sub = document.createElement("div"), this.sub.style.cssText = "font-size:12px;color:#888", this.body.append(this.big, this.sub);
  }
  feed(e) {
    var s;
    const n = this.binding || {};
    if (Es(n)) {
      const l = e.result;
      if (l && l.run_id != null && l.run_id !== this.last) {
        this.last = l.run_id;
        const a = xs(l.code);
        a.kind === "ok" ? this.ok++ : a.kind === "ng" ? this.ng++ : a.kind === "na" && (this.na = (this.na || 0) + 1);
      }
    } else if (e.run_id !== this.last) {
      this.last = e.run_id;
      const l = Kn(e, n);
      l !== void 0 && (l === !0 || l === "OK" || l === "ok" || l === "PASS" ? this.ok++ : this.ng++);
    }
    const r = this.ok + this.ng, i = r ? 100 * this.ok / r : 0;
    this.big.textContent = `${i.toFixed(1)}%`, this.big.style.color = i >= (((s = this.config) == null ? void 0 : s.warn) ?? 95) ? "#3ad17a" : "#ffb454", this.sub.textContent = `OK ${this.ok} / NG ${this.ng}` + (this.na ? ` · NA ${this.na}` : "");
  }
}
class io extends HTMLElement {
  connectedCallback() {
    var e;
    this.body = Lt(this, ((e = this.config) == null ? void 0 : e.title) || "Dispatch groups"), this.body.style.cssText = "display:flex;flex-direction:column;gap:10px;padding:10px;overflow:auto", this.peak = {}, this.rows = {};
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
      let l = this.rows[r.name];
      if (!l) {
        l = document.createElement("div"), l.style.cssText = "display:flex;flex-direction:column;gap:3px";
        const a = document.createElement("div");
        a.style.cssText = "display:flex;justify-content:space-between;font-size:12px";
        const o = document.createElement("span");
        o.style.fontWeight = "600";
        const u = document.createElement("span");
        u.style.color = "#888", a.append(o, u);
        const v = document.createElement("div");
        v.style.cssText = "display:flex;gap:3px;height:18px", l.append(a, v), this.body.appendChild(l), this.rows[r.name] = l = { row: l, name: o, meta: u, bar: v, cells: [] };
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
const ks = {
  verdict: Za,
  value: Qa,
  image: eo,
  spc: to,
  throughput: no,
  yield: ro,
  groups: io
};
for (const [t, e] of Object.entries(ks)) customElements.define(`xi-card-${t}`, e);
const jn = (t) => !!(t && t.card), ht = (t) => !!(t && (t.dir === "row" || t.dir === "col") && Array.isArray(t.children) && t.children.length >= 1), Le = (t) => !!(t && Array.isArray(t.tabs) && t.tabs.length >= 1 && t.tabs.every((e) => e && e.child)), wn = () => ({ type: "value", bind: {}, config: { title: "(empty)" } });
function Wr(t) {
  const e = t.children.length;
  return (Array.isArray(t.weights) && t.weights.length === e ? t.weights.slice() : Array(e).fill(1)).map((r) => typeof r == "number" && r > 0 ? r : 1);
}
function so(t) {
  const e = Wr(t), n = e.reduce((r, i) => r + i, 0) || 1;
  return e.map((r) => r / n);
}
function $s(t, e) {
  return Le(t) ? t.tabs[e].child : t.children[e];
}
function lo(t, e, n) {
  if (Le(t)) {
    const i = t.tabs.slice();
    return i[e] = { ...i[e], child: n }, { ...t, tabs: i };
  }
  const r = t.children.slice();
  return r[e] = n, { ...t, children: r };
}
function _r(t, e, n = []) {
  if (jn(t)) {
    e(t.card, n);
    return;
  }
  ht(t) ? t.children.forEach((r, i) => _r(r, e, [...n, i])) : Le(t) && t.tabs.forEach((r, i) => _r(r.child, e, [...n, i]));
}
function mo(t) {
  let e = 0;
  return _r(t, () => e++), e;
}
function ao(t, e) {
  let n = t;
  for (const r of e)
    if (ht(n) || Le(n)) n = $s(n, r);
    else return;
  return n;
}
function Te(t, e, n) {
  if (e.length === 0) return n(t);
  const [r, ...i] = e;
  return lo(t, r, Te($s(t, r), i, n));
}
function _o(t, e, n, r = wn()) {
  return Te(t, e, (i) => ({ dir: n === "col" ? "col" : "row", children: [i, { card: r }], weights: [1, 1] }));
}
function bo(t, e, n, r = wn()) {
  if (n = n === "col" ? "col" : "row", e.length === 0) return { dir: n, children: [t, { card: r }], weights: [1, 1] };
  const i = e.slice(0, -1), s = e[e.length - 1], l = ao(t, i);
  return ht(l) && l.dir === n ? Te(t, i, (a) => {
    const o = a.children.slice();
    o.splice(s + 1, 0, { card: r });
    const u = Wr(a);
    return u.splice(s + 1, 0, u[s]), { ...a, children: o, weights: u };
  }) : Te(t, e, (a) => ({ dir: n, children: [a, { card: r }], weights: [1, 1] }));
}
function yo(t, e) {
  if (e.length === 0) return { card: wn() };
  const n = e.slice(0, -1), r = e[e.length - 1];
  return Te(t, n, (i) => {
    if (!ht(i)) return i;
    const s = i.children.filter((a, o) => o !== r), l = Wr(i).filter((a, o) => o !== r);
    return s.length === 1 ? s[0] : { ...i, children: s, weights: l };
  });
}
function wo(t, e, n) {
  return Te(t, e, () => ({ card: n }));
}
function xo(t, e, n) {
  return Te(t, e, (r) => ht(r) ? { ...r, weights: n.slice() } : r);
}
function Eo(t, e) {
  return Te(t, e, (n) => ({ tabs: [{ name: "Page 1", child: n }, { name: "Page 2", child: { card: wn() } }], active: 0 }));
}
function ko(t, e, n, r = { card: wn() }) {
  return Te(t, e, (i) => Le(i) ? { ...i, tabs: [...i.tabs, { name: n || `Page ${i.tabs.length + 1}`, child: r }], active: i.tabs.length } : i);
}
function $o(t, e, n) {
  return Te(t, e, (r) => {
    if (!Le(r)) return r;
    const i = r.tabs.filter((s, l) => l !== n);
    return i.length === 1 ? i[0].child : { ...r, tabs: i, active: Math.max(0, Math.min(r.active || 0, i.length - 1)) };
  });
}
function To(t, e, n, r) {
  return Te(t, e, (i) => Le(i) ? { ...i, tabs: i.tabs.map((s, l) => l === n ? { ...s, name: r } : s) } : i);
}
function So(t, e, n) {
  return Te(t, e, (r) => Le(r) ? { ...r, active: n } : r);
}
function si(t, e = "root") {
  return jn(t) ? t.card.type ? [] : [`${e}: leaf has no card.type`] : ht(t) ? t.children.flatMap((n, r) => si(n, `${e}.${r}`)) : Le(t) ? t.tabs.flatMap((n, r) => si(n.child, `${e}.${n.name || r}`)) : [`${e}: node is not a leaf {card}, split {dir,children}, or tabs {tabs}`];
}
function Co(t, { client: e, dashboard: n, pollStatsMs: r = 200 }) {
  const i = t.ownerDocument || globalThis.document, s = globalThis.requestAnimationFrame || ((c) => setTimeout(c, 16)), l = { run_id: -1, vars: {}, images: {}, run_ms: null, status: null, result: null, groups: [] };
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
  function v(c) {
    const x = ks[c.type], m = i.createElement(x ? `xi-card-${c.type}` : "div");
    return x || (m.textContent = `unknown card: ${c.type}`, m.style.cssText = "color:#f88;padding:8px"), m.binding = c.bind || {}, m.config = c.config || {}, m.style.minWidth = "0", m.style.minHeight = "0", m.style.overflow = "hidden", x && a.push(m), m;
  }
  function b(c) {
    let x = Math.min(c.active || 0, c.tabs.length - 1);
    const m = i.createElement("div");
    m.style.cssText = "display:flex;flex-direction:column;min-width:0;min-height:0;width:100%;height:100%";
    const k = i.createElement("div");
    k.style.cssText = "display:flex;gap:2px;flex:0 0 auto;align-items:center;padding:2px 2px 0;overflow:auto";
    const C = i.createElement("div");
    C.style.cssText = "flex:1 1 0;min-width:0;min-height:0;position:relative";
    const D = [], P = [], U = () => {
      D.forEach((H, z) => {
        const Q = z === x;
        H.style.background = Q ? "#1e1e1e" : "#121212", H.style.color = Q ? "#ddd" : "#888";
      }), P.forEach((H, z) => {
        H.style.display = z === x ? "" : "none";
      });
    };
    return c.tabs.forEach((H, z) => {
      const Q = i.createElement("div");
      Q.style.cssText = "padding:4px 11px;cursor:pointer;border-radius:6px 6px 0 0;border:1px solid #333;border-bottom:none;font:12px system-ui,sans-serif", Q.textContent = H.name || `Page ${z + 1}`, Q.onclick = () => {
        x = z, U();
      }, D.push(Q), k.appendChild(Q);
      const Pe = d(H.child);
      Pe.style.cssText += ";position:absolute;inset:0;min-width:0;min-height:0;border:1px solid #333;border-radius:0 6px 6px 6px;overflow:hidden", P.push(Pe), C.appendChild(Pe);
    }), U(), m.append(k, C), m;
  }
  function d(c) {
    if (jn(c)) return v(c.card);
    if (Le(c)) return b(c);
    if (!ht(c)) {
      const C = i.createElement("div");
      return C.textContent = "bad layout node", C.style.color = "#f88", C;
    }
    const x = c.dir === "col", m = i.createElement("div");
    m.style.cssText = `display:flex;flex-direction:${x ? "column" : "row"};min-width:0;min-height:0;width:100%;height:100%`;
    const k = so(c);
    return c.children.forEach((C, D) => {
      const P = d(C);
      P.style.flex = `${k[D]} 1 0`, P.style.minWidth = "0", P.style.minHeight = "0", m.appendChild(P);
    }), m;
  }
  let g = [];
  function _(c, x) {
    if (!c) return x;
    if (jn(c)) {
      const m = c.card;
      m && m.type === "image" && m.bind && m.bind.var && x.push(m.bind.var);
    } else Le(c) ? (c.tabs || []).forEach((m) => _(m.child || m, x)) : ht(c) && (c.children || []).forEach((m) => _(m, x));
    return x;
  }
  function h() {
    var k, C;
    const c = [...new Set(_(n && n.layout, []))], x = new Set(c), m = new Set(g);
    for (const D of g) x.has(D) || (k = e.unsubscribeImage) == null || k.call(e, D);
    for (const D of c) m.has(D) || (C = e.subscribeImage) == null || C.call(e, D);
    g = c;
  }
  function p() {
    a = [], t.replaceChildren(), t.style.cssText += ";display:flex;min-width:0;min-height:0", h();
    const c = n && n.layout;
    if (!c) return;
    const x = d(c);
    x.style.flex = "1 1 0", x.style.minWidth = "0", x.style.minHeight = "0", t.appendChild(x), u();
  }
  const y = [
    e.onVars((c) => {
      l.run_id = c.run_id, l.vars = c.items;
      const x = {};
      for (const m of Object.keys(c.items || {})) {
        const k = c.items[m];
        k && k.gid != null && (x[k.gid] = k.src != null ? k.src : k.gid);
      }
      l.gidToCanon = x, u();
    }),
    e.onPreview((c) => {
      const x = l.gidToCanon && c.gid in l.gidToCanon ? l.gidToCanon[c.gid] : c.gid;
      l.images[x] = c.image || c.dataUrl, u();
    }),
    e.onEvent((c) => {
      c.name === "run_finished" && c.data && typeof c.data.ms == "number" ? l.run_ms = c.data.ms : c.name === "run_result" && c.data ? (l.result = c.data, u()) : (c.name === "safe_state" || c.name === "status") && (l.status = c.data, u());
    })
  ], w = setInterval(() => {
    e.cmd("dispatch_stats").then((c) => {
      c && Array.isArray(c.groups) && (l.groups = c.groups, u());
    }).catch(() => {
    });
  }, r);
  return p(), {
    setDashboard(c) {
      n = c, p();
    },
    state: l,
    destroy() {
      var c;
      y.forEach((x) => x());
      for (const x of g) (c = e.unsubscribeImage) == null || c.call(e, x);
      g = [], clearInterval(w), t.replaceChildren();
    }
  };
}
const Mo = [
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
  ks as CARDS,
  Xa as CONTROL_TAGS,
  mr as TOOLS,
  Mo as XI_COMPONENTS,
  ho as XiClient,
  bo as addSibling,
  ko as addTab,
  ws as bytesToBase64,
  po as collectStatusItems,
  mo as countLeaves,
  ys as decodePreviewFrame,
  _r as eachLeaf,
  wn as emptyCard,
  ao as getNode,
  Ga as inferDescriptor,
  jn as isLeaf,
  ht as isSplit,
  Le as isTabs,
  ii as makeTool,
  Co as mountDashboard,
  go as mountMonitor,
  vo as mountPanel,
  bs as parseVars,
  co as protocol,
  uo as registerTool,
  yo as removePane,
  $o as removeTab,
  To as renameTab,
  So as setActive,
  wo as setCard,
  xo as setWeights,
  _o as splitLeaf,
  si as validate,
  so as weightsOf,
  Eo as wrapInTabs
};
