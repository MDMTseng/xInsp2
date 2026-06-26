var Ts = Object.defineProperty;
var zr = (e) => {
  throw TypeError(e);
};
var Cs = (e, t, n) => t in e ? Ts(e, t, { enumerable: !0, configurable: !0, writable: !0, value: n }) : e[t] = n;
var W = (e, t, n) => Cs(e, typeof t != "symbol" ? t + "" : t, n), Kn = (e, t, n) => t.has(e) || zr("Cannot " + n);
var u = (e, t, n) => (Kn(e, t, "read from private field"), n ? n.call(e) : t.get(e)), C = (e, t, n) => t.has(e) ? zr("Cannot add the same private member more than once") : t instanceof WeakSet ? t.add(e) : t.set(e, n), $ = (e, t, n, r) => (Kn(e, t, "write to private field"), r ? r.call(e, n) : t.set(e, n), n), A = (e, t, n) => (Kn(e, t, "access private method"), n);
var li;
typeof window < "u" && ((li = window.__svelte ?? (window.__svelte = {})).v ?? (li.v = /* @__PURE__ */ new Set())).add("5");
const Ss = 1, Ms = 2, fi = 4, As = 8, Ns = 16, Os = 1, Is = 4, Rs = 8, Ls = 16, Ps = 2, ui = "[", _r = "[!", qr = "[?", br = "]", Ut = {}, U = Symbol("uninitialized"), Ds = "http://www.w3.org/1999/xhtml", ci = !1;
var yr = Array.isArray, Hs = Array.prototype.indexOf, An = Array.prototype.includes, Bn = Array.from, Nn = Object.keys, On = Object.defineProperty, Et = Object.getOwnPropertyDescriptor, js = Object.getOwnPropertyDescriptors, Fs = Object.prototype, Ws = Array.prototype, di = Object.getPrototypeOf, Vr = Object.isExtensible;
const Bs = () => {
};
function Ys(e) {
  for (var t = 0; t < e.length; t++)
    e[t]();
}
function hi() {
  var e, t, n = new Promise((r, i) => {
    e = r, t = i;
  });
  return { promise: n, resolve: e, reject: t };
}
const Z = 2, Xt = 4, Yn = 8, vi = 1 << 24, Ae = 16, Oe = 32, et = 64, tr = 128, xe = 512, X = 1024, G = 2048, Fe = 4096, ne = 8192, ve = 16384, At = 32768, nr = 1 << 25, Gt = 65536, In = 1 << 17, zs = 1 << 18, Nt = 1 << 19, qs = 1 << 20, De = 1 << 25, St = 65536, Rn = 1 << 21, jt = 1 << 22, ft = 1 << 23, kt = Symbol("$state"), pi = Symbol("legacy props"), Vs = Symbol(""), kn = Symbol("attributes"), Us = Symbol("class"), Xs = Symbol("style"), en = Symbol("text"), gi = Symbol("form reset"), zn = new class extends Error {
  constructor() {
    super(...arguments);
    W(this, "name", "StaleReactionError");
    W(this, "message", "The reaction that called `getAbortSignal()` was re-run or destroyed");
  }
}();
var ai;
const mi = (
  // We gotta write it like this because after downleveling the pure comment may end up in the wrong location
  !!((ai = globalThis.document) != null && ai.contentType) && /* @__PURE__ */ globalThis.document.contentType.includes("xml")
), wr = 3, _n = 8;
function Gs() {
  throw new Error("https://svelte.dev/e/async_derived_orphan");
}
function Ks(e, t, n) {
  throw new Error("https://svelte.dev/e/each_key_duplicate");
}
function Js(e) {
  throw new Error("https://svelte.dev/e/effect_in_teardown");
}
function Zs() {
  throw new Error("https://svelte.dev/e/effect_in_unowned_derived");
}
function Qs(e) {
  throw new Error("https://svelte.dev/e/effect_orphan");
}
function el() {
  throw new Error("https://svelte.dev/e/effect_update_depth_exceeded");
}
function tl() {
  throw new Error("https://svelte.dev/e/hydration_failed");
}
function nl(e) {
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
function qn(e) {
  console.warn("https://svelte.dev/e/hydration_mismatch");
}
function ol() {
  console.warn("https://svelte.dev/e/select_multiple_invalid_value");
}
function fl() {
  console.warn("https://svelte.dev/e/svelte_boundary_reset_noop");
}
let D = !1;
function Je(e) {
  D = e;
}
let H;
function pe(e) {
  if (e === null)
    throw qn(), Ut;
  return H = e;
}
function Vn() {
  return pe(/* @__PURE__ */ it(H));
}
function q(e) {
  if (D) {
    if (/* @__PURE__ */ it(H) !== null)
      throw qn(), Ut;
    H = e;
  }
}
function ul(e = 1) {
  if (D) {
    for (var t = e, n = H; t--; )
      n = /** @type {TemplateNode} */
      /* @__PURE__ */ it(n);
    H = n;
  }
}
function Ln(e = !0) {
  for (var t = 0, n = H; ; ) {
    if (n.nodeType === _n) {
      var r = (
        /** @type {Comment} */
        n.data
      );
      if (r === br) {
        if (t === 0) return n;
        t -= 1;
      } else (r === ui || r === _r || // "[1", "[2", etc. for if blocks
      r[0] === "[" && !isNaN(Number(r.slice(1)))) && (t += 1);
    }
    var i = (
      /** @type {TemplateNode} */
      /* @__PURE__ */ it(n)
    );
    e && n.remove(), n = i;
  }
}
function _i(e) {
  if (!e || e.nodeType !== _n)
    throw qn(), Ut;
  return (
    /** @type {Comment} */
    e.data
  );
}
function bi(e) {
  return e === this.v;
}
function cl(e, t) {
  return e != e ? t == t : e !== t || e !== null && typeof e == "object" || typeof e == "function";
}
function yi(e) {
  return !cl(e, this.v);
}
let dl = !1, re = null;
function Kt(e) {
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
      S
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
function wi() {
  return !0;
}
let vt = [];
function xi() {
  var e = vt;
  vt = [], Ys(e);
}
function Ze(e) {
  if (vt.length === 0 && !an) {
    var t = vt;
    queueMicrotask(() => {
      t === vt && xi();
    });
  }
  vt.push(e);
}
function hl() {
  for (; vt.length > 0; )
    xi();
}
function Ei(e) {
  var t = S;
  if (t === null)
    return M.f |= ft, e;
  if ((t.f & At) === 0 && (t.f & Xt) === 0)
    throw e;
  ot(e, t);
}
function ot(e, t) {
  if (!(t !== null && (t.f & ve) !== 0)) {
    for (; t !== null; ) {
      if ((t.f & tr) !== 0) {
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
const vl = -7169;
function z(e, t) {
  e.f = e.f & vl | t;
}
function xr(e) {
  (e.f & xe) !== 0 || e.deps === null ? z(e, X) : z(e, Fe);
}
function ki(e) {
  if (e !== null)
    for (const t of e)
      (t.f & Z) === 0 || (t.f & St) === 0 || (t.f ^= St, ki(
        /** @type {Derived} */
        t.deps
      ));
}
function $i(e, t, n) {
  (e.f & G) !== 0 ? t.add(e) : (e.f & Fe) !== 0 && n.add(e), ki(e.deps), z(e, X);
}
let xn = !1;
function pl(e) {
  var t = xn;
  try {
    return xn = !1, [e(), xn];
  } finally {
    xn = t;
  }
}
function gl(e) {
  let t = 0, n = Mt(0), r;
  return () => {
    Sr() && (N(n), Nr(() => (t === 0 && (r = Lr(() => e(() => on(n)))), t += 1, () => {
      Ze(() => {
        t -= 1, t === 0 && (r == null || r(), r = void 0, on(n));
      });
    })));
  };
}
var ml = Gt | Nt;
function _l(e, t, n, r) {
  new bl(e, t, n, r);
}
var ce, dn, _e, _t, ae, be, te, de, Ve, bt, lt, Ft, hn, vn, Ue, jn, F, Ti, Ci, Si, rr, $n, Tn, ir, sr;
class bl {
  /**
   * @param {TemplateNode} node
   * @param {BoundaryProps} props
   * @param {((anchor: Node) => void)} children
   * @param {((error: unknown) => unknown) | undefined} [transform_error]
   */
  constructor(t, n, r, i) {
    C(this, F);
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
    C(this, ce);
    /** @type {TemplateNode | null} */
    C(this, dn, D ? H : null);
    /** @type {BoundaryProps} */
    C(this, _e);
    /** @type {((anchor: Node) => void)} */
    C(this, _t);
    /** @type {Effect} */
    C(this, ae);
    /** @type {Effect | null} */
    C(this, be, null);
    /** @type {Effect | null} */
    C(this, te, null);
    /** @type {Effect | null} */
    C(this, de, null);
    /** @type {DocumentFragment | null} */
    C(this, Ve, null);
    C(this, bt, 0);
    C(this, lt, 0);
    C(this, Ft, !1);
    /** @type {Set<Effect>} */
    C(this, hn, /* @__PURE__ */ new Set());
    /** @type {Set<Effect>} */
    C(this, vn, /* @__PURE__ */ new Set());
    /**
     * A source containing the number of pending async deriveds/expressions.
     * Only created if `$effect.pending()` is used inside the boundary,
     * otherwise updating the source results in needless `Batch.ensure()`
     * calls followed by no-op flushes
     * @type {Source<number> | null}
     */
    C(this, Ue, null);
    C(this, jn, gl(() => ($(this, Ue, Mt(u(this, bt))), () => {
      $(this, Ue, null);
    })));
    var s;
    $(this, ce, t), $(this, _e, n), $(this, _t, (l) => {
      var a = (
        /** @type {Effect} */
        S
      );
      a.b = this, a.f |= tr, r(l);
    }), this.parent = /** @type {Effect} */
    S.b, this.transform_error = i ?? ((s = this.parent) == null ? void 0 : s.transform_error) ?? ((l) => l), $(this, ae, Or(() => {
      if (D) {
        const l = (
          /** @type {Comment} */
          u(this, dn)
        );
        Vn();
        const a = l.data === _r;
        if (l.data.startsWith(qr)) {
          const c = JSON.parse(l.data.slice(qr.length));
          A(this, F, Ci).call(this, c);
        } else a ? A(this, F, Si).call(this) : A(this, F, Ti).call(this);
      } else
        A(this, F, rr).call(this);
    }, ml)), D && $(this, ce, H);
  }
  /**
   * Defer an effect inside a pending boundary until the boundary resolves
   * @param {Effect} effect
   */
  defer_effect(t) {
    $i(t, u(this, hn), u(this, vn));
  }
  /**
   * Returns `false` if the effect exists inside a boundary whose pending snippet is shown
   * @returns {boolean}
   */
  is_rendered() {
    return !this.is_pending && (!this.parent || this.parent.is_rendered());
  }
  has_pending_snippet() {
    return !!u(this, _e).pending;
  }
  /**
   * Update the source that powers `$effect.pending()` inside this boundary,
   * and controls when the current `pending` snippet (if any) is removed.
   * Do not call from inside the class
   * @param {1 | -1} d
   * @param {Batch} batch
   */
  update_pending_count(t, n) {
    A(this, F, ir).call(this, t, n), $(this, bt, u(this, bt) + t), !(!u(this, Ue) || u(this, Ft)) && ($(this, Ft, !0), Ze(() => {
      $(this, Ft, !1), u(this, Ue) && Jt(u(this, Ue), u(this, bt));
    }));
  }
  get_effect_pending() {
    return u(this, jn).call(this), N(
      /** @type {Source<number>} */
      u(this, Ue)
    );
  }
  /** @param {unknown} error */
  error(t) {
    if (!u(this, _e).onerror && !u(this, _e).failed)
      throw t;
    T != null && T.is_fork ? (u(this, be) && T.skip_effect(u(this, be)), u(this, te) && T.skip_effect(u(this, te)), u(this, de) && T.skip_effect(u(this, de)), T.oncommit(() => {
      A(this, F, sr).call(this, t);
    })) : A(this, F, sr).call(this, t);
  }
}
ce = new WeakMap(), dn = new WeakMap(), _e = new WeakMap(), _t = new WeakMap(), ae = new WeakMap(), be = new WeakMap(), te = new WeakMap(), de = new WeakMap(), Ve = new WeakMap(), bt = new WeakMap(), lt = new WeakMap(), Ft = new WeakMap(), hn = new WeakMap(), vn = new WeakMap(), Ue = new WeakMap(), jn = new WeakMap(), F = new WeakSet(), Ti = function() {
  try {
    $(this, be, we(() => u(this, _t).call(this, u(this, ce))));
  } catch (t) {
    this.error(t);
  }
}, /**
 * @param {unknown} error The deserialized error from the server's hydration comment
 */
Ci = function(t) {
  const n = u(this, _e).failed;
  n && $(this, de, we(() => {
    n(
      u(this, ce),
      () => t,
      () => () => {
      }
    );
  }));
}, Si = function() {
  const t = u(this, _e).pending;
  t && (this.is_pending = !0, $(this, te, we(() => t(u(this, ce)))), Ze(() => {
    var n = $(this, Ve, document.createDocumentFragment()), r = He();
    n.append(r), $(this, be, A(this, F, Tn).call(this, () => we(() => u(this, _t).call(this, r)))), u(this, lt) === 0 && (u(this, ce).before(n), $(this, Ve, null), Tt(
      /** @type {Effect} */
      u(this, te),
      () => {
        $(this, te, null);
      }
    ), A(this, F, $n).call(
      this,
      /** @type {Batch} */
      T
    ));
  }));
}, rr = function() {
  try {
    if (this.is_pending = this.has_pending_snippet(), $(this, lt, 0), $(this, bt, 0), $(this, be, we(() => {
      u(this, _t).call(this, u(this, ce));
    })), u(this, lt) > 0) {
      var t = $(this, Ve, document.createDocumentFragment());
      Rr(u(this, be), t);
      const n = (
        /** @type {(anchor: Node) => void} */
        u(this, _e).pending
      );
      $(this, te, we(() => n(u(this, ce))));
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
  this.is_pending = !1, t.transfer_effects(u(this, hn), u(this, vn));
}, /**
 * @template T
 * @param {() => T} fn
 */
Tn = function(t) {
  var n = S, r = M, i = re;
  We(u(this, ae)), ke(u(this, ae)), Kt(u(this, ae).ctx);
  try {
    return ut.ensure(), t();
  } catch (s) {
    return Ei(s), null;
  } finally {
    We(n), ke(r), Kt(i);
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
    this.parent && A(r = this.parent, F, ir).call(r, t, n);
    return;
  }
  $(this, lt, u(this, lt) + t), u(this, lt) === 0 && (A(this, F, $n).call(this, n), u(this, te) && Tt(u(this, te), () => {
    $(this, te, null);
  }), u(this, Ve) && (u(this, ce).before(u(this, Ve)), $(this, Ve, null)));
}, /**
 * @param {unknown} error
 */
sr = function(t) {
  u(this, be) && (ie(u(this, be)), $(this, be, null)), u(this, te) && (ie(u(this, te)), $(this, te, null)), u(this, de) && (ie(u(this, de)), $(this, de, null)), D && (pe(
    /** @type {TemplateNode} */
    u(this, dn)
  ), ul(), pe(Ln()));
  var n = u(this, _e).onerror;
  let r = u(this, _e).failed;
  var i = !1, s = !1;
  const l = () => {
    if (i) {
      fl();
      return;
    }
    i = !0, s && ll(), u(this, de) !== null && Tt(u(this, de), () => {
      $(this, de, null);
    }), A(this, F, Tn).call(this, () => {
      A(this, F, rr).call(this);
    });
  }, a = (o) => {
    try {
      s = !0, n == null || n(o, l), s = !1;
    } catch (c) {
      ot(c, u(this, ae) && u(this, ae).parent);
    }
    r && $(this, de, A(this, F, Tn).call(this, () => {
      try {
        return we(() => {
          var c = (
            /** @type {Effect} */
            S
          );
          c.b = this, c.f |= tr, r(
            u(this, ce),
            () => o,
            () => l
          );
        });
      } catch (c) {
        return ot(
          c,
          /** @type {Effect} */
          u(this, ae).parent
        ), null;
      }
    }));
  };
  Ze(() => {
    var o;
    try {
      o = this.transform_error(t);
    } catch (c) {
      ot(c, u(this, ae) && u(this, ae).parent);
      return;
    }
    o !== null && typeof o == "object" && typeof /** @type {any} */
    o.then == "function" ? o.then(
      a,
      /** @param {unknown} e */
      (c) => ot(c, u(this, ae) && u(this, ae).parent)
    ) : a(o);
  });
};
function yl(e, t, n, r) {
  const i = fn;
  var s = e.filter((v) => !v.settled), l = t.map(i);
  if (n.length === 0 && s.length === 0) {
    r(l);
    return;
  }
  var a = (
    /** @type {Effect} */
    S
  ), o = wl(), c = s.length === 1 ? s[0].promise : s.length > 1 ? Promise.all(s.map((v) => v.promise)) : null;
  function h(v) {
    if ((a.f & ve) === 0) {
      o();
      try {
        r([...l, ...v]);
      } catch (g) {
        ot(g, a);
      }
      Pn();
    }
  }
  var b = Mi();
  if (n.length === 0) {
    c.then(() => h([])).finally(b);
    return;
  }
  function d() {
    Promise.all(n.map((v) => /* @__PURE__ */ xl(v))).then(h).catch((v) => ot(v, a)).finally(b);
  }
  c ? c.then(() => {
    o(), d(), Pn();
  }) : d();
}
function wl() {
  var e = (
    /** @type {Effect} */
    S
  ), t = M, n = re, r = (
    /** @type {Batch} */
    T
  );
  return function(s = !0) {
    We(e), ke(t), Kt(n), s && (e.f & ve) === 0 && (r == null || r.activate(), r == null || r.apply());
  };
}
function Pn(e = !0) {
  We(null), ke(null), Kt(null), e && (T == null || T.deactivate());
}
function Mi() {
  var e = (
    /** @type {Effect} */
    S
  ), t = e.b, n = (
    /** @type {Batch} */
    T
  ), r = !!(t != null && t.is_rendered());
  return t == null || t.update_pending_count(1, n), n.increment(r, e), () => {
    t == null || t.update_pending_count(-1, n), n.decrement(r, e);
  };
}
// @__NO_SIDE_EFFECTS__
function fn(e) {
  var t = Z | G;
  return S !== null && (S.f |= Nt), {
    ctx: re,
    deps: null,
    effects: null,
    equals: bi,
    f: t,
    fn: e,
    reactions: null,
    rv: 0,
    v: (
      /** @type {V} */
      U
    ),
    wv: 0,
    parent: S,
    ac: null
  };
}
const tn = Symbol("obsolete");
// @__NO_SIDE_EFFECTS__
function xl(e, t, n) {
  let r = (
    /** @type {Effect | null} */
    S
  );
  r === null && Gs();
  var i = (
    /** @type {Promise<V>} */
    /** @type {unknown} */
    void 0
  ), s = Mt(
    /** @type {V} */
    U
  ), l = !M, a = /* @__PURE__ */ new Set();
  return Ll(() => {
    var v, g;
    var o = (
      /** @type {Effect} */
      S
    ), c = hi();
    i = c.promise;
    try {
      Promise.resolve(e()).then(c.resolve, (p) => {
        p !== zn && c.reject(p);
      }).finally(Pn);
    } catch (p) {
      c.reject(p), Pn();
    }
    var h = (
      /** @type {Batch} */
      T
    );
    if (l) {
      if ((o.f & At) !== 0)
        var b = Mi();
      if (
        // boundary can be null if the async derived is inside an $effect.root not connected to the component render tree
        (v = r.b) != null && v.is_rendered()
      )
        (g = h.async_deriveds.get(o)) == null || g.reject(tn);
      else
        for (const p of a.values())
          p.reject(tn);
      a.add(c), h.async_deriveds.set(o, c);
    }
    const d = (p, f = void 0) => {
      b == null || b(), a.delete(c), f !== tn && (h.activate(), f ? (s.f |= ft, Jt(s, f)) : ((s.f & ft) !== 0 && (s.f ^= ft), Jt(s, p)), h.deactivate());
    };
    c.promise.then(d, (p) => d(null, p || "unknown"));
  }), Mr(() => {
    for (const o of a)
      o.reject(tn);
  }), new Promise((o) => {
    function c(h) {
      function b() {
        h === i ? o(s) : c(i);
      }
      h.then(b, b);
    }
    c(i);
  });
}
// @__NO_SIDE_EFFECTS__
function Ai(e) {
  const t = /* @__PURE__ */ fn(e);
  return es(t), t;
}
// @__NO_SIDE_EFFECTS__
function Ni(e) {
  const t = /* @__PURE__ */ fn(e);
  return t.equals = yi, t;
}
function El(e) {
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
function Er(e) {
  var t, n = S, r = e.parent;
  if (!tt && r !== null && e.v !== U && // if it was never evaluated before, it's guaranteed to fail downstream, so we try to execute instead
  (r.f & (ve | ne)) !== 0)
    return al(), e.v;
  We(r);
  try {
    e.f &= ~St, El(e), t = is(e);
  } finally {
    We(n);
  }
  return t;
}
function Oi(e) {
  var t = Er(e);
  if (!e.equals(t) && (e.wv = ns(), (!(T != null && T.is_fork) || e.deps === null) && (T !== null ? (T.capture(e, t, !0), ln == null || ln.capture(e, t, !0)) : e.v = t, e.deps === null))) {
    z(e, X);
    return;
  }
  tt || (J !== null ? (Sr() || T != null && T.is_fork) && J.set(e, t) : xr(e));
}
function kl(e) {
  var t, n;
  if (e.effects !== null)
    for (const r of e.effects)
      (r.teardown || r.ac) && ((t = r.teardown) == null || t.call(r), (n = r.ac) == null || n.abort(zn), r.fn !== null && (r.teardown = Bs), r.ac = null, cn(r, 0), Ir(r));
}
function Ii(e) {
  if (e.effects !== null)
    for (const t of e.effects)
      t.teardown && t.fn !== null && Zt(t);
}
let Jn = null, Pt = null, T = null, ln = null, J = null, lr = null, an = !1, Zn = !1, Ht = null, Cn = null;
var Ur = 0;
let $l = 1;
var Wt, at, yt, Bt, Yt, zt, Xe, qt, oe, pn, Ge, Ce, Le, Vt, wt, L, ar, nn, or, Ri, Li, Dt, Tl, rn;
const Fn = class Fn {
  constructor() {
    C(this, L);
    W(this, "id", $l++);
    /** True as soon as `#process` was called */
    C(this, Wt, !1);
    W(this, "linked", !0);
    /** @type {Batch | null} */
    C(this, at, null);
    /** @type {Batch | null} */
    C(this, yt, null);
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
    C(this, Bt, /* @__PURE__ */ new Set());
    /**
     * If a fork is discarded, we need to destroy any effects that are no longer needed
     * @type {Set<(batch: Batch) => void>}
     */
    C(this, Yt, /* @__PURE__ */ new Set());
    /**
     * The number of async effects that are currently in flight
     */
    C(this, zt, 0);
    /**
     * Async effects that are currently in flight, _not_ inside a pending boundary
     * @type {Map<Effect, number>}
     */
    C(this, Xe, /* @__PURE__ */ new Map());
    /**
     * A deferred that resolves when the batch is committed, used with `settled()`
     * TODO replace with Promise.withResolvers once supported widely enough
     * @type {{ promise: Promise<void>, resolve: (value?: any) => void, reject: (reason: unknown) => void } | null}
     */
    C(this, qt, null);
    /**
     * The root effects that need to be flushed
     * @type {Effect[]}
     */
    C(this, oe, []);
    /**
     * Effects created while this batch was active.
     * @type {Effect[]}
     */
    C(this, pn, []);
    /**
     * Deferred effects (which run after async work has completed) that are DIRTY
     * @type {Set<Effect>}
     */
    C(this, Ge, /* @__PURE__ */ new Set());
    /**
     * Deferred effects that are MAYBE_DIRTY
     * @type {Set<Effect>}
     */
    C(this, Ce, /* @__PURE__ */ new Set());
    /**
     * A map of branches that still exist, but will be destroyed when this batch
     * is committed — we skip over these during `process`.
     * The value contains child effects that were dirty/maybe_dirty before being reset,
     * so they can be rescheduled if the branch survives.
     * @type {Map<Effect, { d: Effect[], m: Effect[] }>}
     */
    C(this, Le, /* @__PURE__ */ new Map());
    /**
     * Inverse of #skipped_branches which we need to tell prior batches to unskip them when committing
     * @type {Set<Effect>}
     */
    C(this, Vt, /* @__PURE__ */ new Set());
    W(this, "is_fork", !1);
    C(this, wt, !1);
    Pt === null ? Jn = Pt = this : ($(Pt, yt, this), $(this, at, Pt)), Pt = this;
  }
  /**
   * Add an effect to the #skipped_branches map and reset its children
   * @param {Effect} effect
   */
  skip_effect(t) {
    u(this, Le).has(t) || u(this, Le).set(t, { d: [], m: [] }), u(this, Vt).delete(t);
  }
  /**
   * Remove an effect from the #skipped_branches map and reschedule
   * any tracked dirty/maybe_dirty child effects
   * @param {Effect} effect
   * @param {(e: Effect) => void} callback
   */
  unskip_effect(t, n = (r) => this.schedule(r)) {
    var r = u(this, Le).get(t);
    if (r) {
      u(this, Le).delete(t);
      for (var i of r.d)
        z(i, G), n(i);
      for (i of r.m)
        z(i, Fe), n(i);
    }
    u(this, Vt).add(t);
  }
  /**
   * Associate a change to a given source with the current
   * batch, noting its previous and current values
   * @param {Value} source
   * @param {any} value
   * @param {boolean} [is_derived]
   */
  capture(t, n, r = !1) {
    t.v !== U && !this.previous.has(t) && this.previous.set(t, t.v), (t.f & ft) === 0 && (this.current.set(t, [n, r]), J == null || J.set(t, n)), this.is_fork || (t.v = n);
  }
  activate() {
    T = this;
  }
  deactivate() {
    T = null, J = null;
  }
  flush() {
    try {
      Zn = !0, T = this, A(this, L, nn).call(this);
    } finally {
      Ur = 0, lr = null, Ht = null, Cn = null, Zn = !1, T = null, J = null, $t.clear();
    }
  }
  discard() {
    var t;
    for (const n of u(this, Yt)) n(this);
    u(this, Yt).clear();
    for (const n of this.async_deriveds.values())
      n.reject(tn);
    A(this, L, rn).call(this), (t = u(this, qt)) == null || t.resolve();
  }
  /**
   * @param {Effect} effect
   */
  register_created_effect(t) {
    u(this, pn).push(t);
  }
  /**
   * @param {boolean} blocking
   * @param {Effect} effect
   */
  increment(t, n) {
    if ($(this, zt, u(this, zt) + 1), t) {
      let r = u(this, Xe).get(n) ?? 0;
      u(this, Xe).set(n, r + 1);
    }
  }
  /**
   * @param {boolean} blocking
   * @param {Effect} effect
   */
  decrement(t, n) {
    if ($(this, zt, u(this, zt) - 1), t) {
      let r = u(this, Xe).get(n) ?? 0;
      r === 1 ? u(this, Xe).delete(n) : u(this, Xe).set(n, r - 1);
    }
    u(this, wt) || ($(this, wt, !0), Ze(() => {
      $(this, wt, !1), this.linked && this.flush();
    }));
  }
  /**
   * @param {Set<Effect>} dirty_effects
   * @param {Set<Effect>} maybe_dirty_effects
   */
  transfer_effects(t, n) {
    for (const r of t)
      u(this, Ge).add(r);
    for (const r of n)
      u(this, Ce).add(r);
    t.clear(), n.clear();
  }
  /** @param {(batch: Batch) => void} fn */
  oncommit(t) {
    u(this, Bt).add(t);
  }
  /** @param {(batch: Batch) => void} fn */
  ondiscard(t) {
    u(this, Yt).add(t);
  }
  settled() {
    return (u(this, qt) ?? $(this, qt, hi())).promise;
  }
  static ensure() {
    if (T === null) {
      const t = T = new Fn();
      !Zn && !an && Ze(() => {
        u(t, Wt) || t.flush();
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
    if (lr = t, (i = t.b) != null && i.is_pending && (t.f & (Xt | Yn | vi)) !== 0 && (t.f & At) === 0) {
      t.b.defer_effect(t);
      return;
    }
    for (var n = t; n.parent !== null; ) {
      n = n.parent;
      var r = n.f;
      if (Ht !== null && n === S && (M === null || (M.f & Z) === 0))
        return;
      if ((r & (et | Oe)) !== 0) {
        if ((r & X) === 0)
          return;
        n.f ^= X;
      }
    }
    u(this, oe).push(n);
  }
};
Wt = new WeakMap(), at = new WeakMap(), yt = new WeakMap(), Bt = new WeakMap(), Yt = new WeakMap(), zt = new WeakMap(), Xe = new WeakMap(), qt = new WeakMap(), oe = new WeakMap(), pn = new WeakMap(), Ge = new WeakMap(), Ce = new WeakMap(), Le = new WeakMap(), Vt = new WeakMap(), wt = new WeakMap(), L = new WeakSet(), ar = function() {
  if (this.is_fork) return !0;
  for (const r of u(this, Xe).keys()) {
    for (var t = r, n = !1; t.parent !== null; ) {
      if (u(this, Le).has(t)) {
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
  var o, c, h, b;
  $(this, Wt, !0), Ur++ > 1e3 && (A(this, L, rn).call(this), Cl());
  for (const d of u(this, Ge))
    u(this, Ce).delete(d), z(d, G), this.schedule(d);
  for (const d of u(this, Ce))
    z(d, Fe), this.schedule(d);
  const t = u(this, oe);
  $(this, oe, []), this.apply();
  var n = Ht = [], r = [], i = Cn = [];
  for (const d of t)
    try {
      A(this, L, or).call(this, d, n, r);
    } catch (v) {
      throw Hi(d), A(this, L, ar).call(this) || this.discard(), v;
    }
  if (T = null, i.length > 0) {
    var s = Fn.ensure();
    for (const d of i)
      s.schedule(d);
  }
  if (Ht = null, Cn = null, A(this, L, ar).call(this)) {
    A(this, L, Dt).call(this, r), A(this, L, Dt).call(this, n);
    for (const [d, v] of u(this, Le))
      Di(d, v);
    i.length > 0 && /** @type {unknown} */
    A(o = T, L, nn).call(o);
    return;
  }
  const l = A(this, L, Ri).call(this);
  if (l) {
    A(this, L, Dt).call(this, r), A(this, L, Dt).call(this, n), A(c = l, L, Li).call(c, this);
    return;
  }
  u(this, Ge).clear(), u(this, Ce).clear();
  for (const d of u(this, Bt)) d(this);
  u(this, Bt).clear(), ln = this, Xr(r), Xr(n), ln = null, (h = u(this, qt)) == null || h.resolve();
  var a = (
    /** @type {Batch | null} */
    /** @type {unknown} */
    T
  );
  if (u(this, zt) === 0 && (u(this, oe).length === 0 || a !== null) && A(this, L, rn).call(this), u(this, oe).length > 0)
    if (a !== null) {
      const d = a;
      u(d, oe).push(...u(this, oe).filter((v) => !u(d, oe).includes(v)));
    } else
      a = this;
  a !== null && A(b = a, L, nn).call(b);
}, /**
 * Traverse the effect tree, executing effects or stashing
 * them for later execution as appropriate
 * @param {Effect} root
 * @param {Effect[]} effects
 * @param {Effect[]} render_effects
 */
or = function(t, n, r) {
  t.f ^= X;
  for (var i = t.first; i !== null; ) {
    var s = i.f, l = (s & (Oe | et)) !== 0, a = l && (s & X) !== 0, o = a || (s & ne) !== 0 || u(this, Le).has(i);
    if (!o && i.fn !== null) {
      l ? i.f ^= X : (s & Xt) !== 0 ? n.push(i) : bn(i) && ((s & Ae) !== 0 && u(this, Ce).add(i), Zt(i));
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
}, Ri = function() {
  for (var t = u(this, at); t !== null; ) {
    if (!t.is_fork) {
      for (const [n, [, r]] of this.current)
        if (t.current.has(n) && !r)
          return t;
    }
    t = u(t, at);
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
  t.async_deriveds.clear(), this.transfer_effects(u(t, Ge), u(t, Ce));
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
          l & (jt | Ae) && !this.async_deriveds.has(a) && (u(this, Ce).delete(a), z(a, G), this.schedule(a));
        }
      }
  };
  for (const i of this.current.keys())
    n(i);
  this.oncommit(() => t.discard()), A(r = t, L, rn).call(r), T = this, A(this, L, nn).call(this);
}, /**
 * @param {Effect[]} effects
 */
Dt = function(t) {
  for (var n = 0; n < t.length; n += 1)
    $i(t[n], u(this, Ge), u(this, Ce));
}, Tl = function() {
  var b;
  for (let d = Jn; d !== null; d = u(d, yt)) {
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
    if (!(!u(d, Wt) || i.length === 0)) {
      var s = i.filter((v) => !this.current.has(v));
      if (s.length === 0)
        t && d.discard();
      else if (n.length > 0) {
        if (t)
          for (const v of u(this, Vt))
            d.unskip_effect(v, (g) => {
              var p;
              (g.f & (Ae | jt)) !== 0 ? d.schedule(g) : A(p = d, L, Dt).call(p, [g]);
            });
        d.activate();
        var l = /* @__PURE__ */ new Set(), a = /* @__PURE__ */ new Map();
        for (var o of n)
          Pi(o, s, l, a);
        a = /* @__PURE__ */ new Map();
        var c = [...d.current].filter(([v, g]) => {
          const p = this.current.get(v);
          return p ? p[0] !== g[0] || p[1] !== g[1] : !0;
        }).map(([v]) => v);
        if (c.length > 0)
          for (const v of u(this, pn))
            (v.f & (ve | ne | In)) === 0 && kr(v, c, a) && ((v.f & (jt | Ae)) !== 0 ? (z(v, G), d.schedule(v)) : u(d, Ge).add(v));
        if (u(d, oe).length > 0 && !u(d, wt)) {
          d.apply();
          for (var h of u(d, oe))
            A(b = d, L, or).call(b, h, [], []);
          $(d, oe, []);
        }
        d.deactivate();
      }
    }
  }
}, rn = function() {
  if (this.linked) {
    var t = u(this, at), n = u(this, yt);
    t === null ? Jn = n : $(t, yt, n), n === null ? Pt = t : $(n, at, t), this.linked = !1;
  }
};
let ut = Fn;
function O(e) {
  var t = an;
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
    an = t;
  }
}
function Cl() {
  try {
    el();
  } catch (e) {
    ot(e, lr);
  }
}
let Te = null;
function Xr(e) {
  var t = e.length;
  if (t !== 0) {
    for (var n = 0; n < t; ) {
      var r = e[n++];
      if ((r.f & (ve | ne)) === 0 && bn(r) && (Te = /* @__PURE__ */ new Set(), Zt(r), r.deps === null && r.first === null && r.nodes === null && r.teardown === null && r.ac === null && Ji(r), (Te == null ? void 0 : Te.size) > 0)) {
        $t.clear();
        for (const i of Te) {
          if ((i.f & (ve | ne)) !== 0) continue;
          const s = [i];
          let l = i.parent;
          for (; l !== null; )
            Te.has(l) && (Te.delete(l), s.push(l)), l = l.parent;
          for (let a = s.length - 1; a >= 0; a--) {
            const o = s[a];
            (o.f & (ve | ne)) === 0 && Zt(o);
          }
        }
        Te.clear();
      }
    }
    Te = null;
  }
}
function Pi(e, t, n, r) {
  if (!n.has(e) && (n.add(e), e.reactions !== null))
    for (const i of e.reactions) {
      const s = i.f;
      (s & Z) !== 0 ? Pi(
        /** @type {Derived} */
        i,
        t,
        n,
        r
      ) : (s & (jt | Ae)) !== 0 && (s & G) === 0 && kr(i, t, r) && (z(i, G), $r(
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
      if (An.call(t, i))
        return !0;
      if ((i.f & Z) !== 0 && kr(
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
function $r(e) {
  T.schedule(e);
}
function Di(e, t) {
  if (!((e.f & Oe) !== 0 && (e.f & X) !== 0)) {
    (e.f & G) !== 0 ? t.d.push(e) : (e.f & Fe) !== 0 && t.m.push(e), z(e, X);
    for (var n = e.first; n !== null; )
      Di(n, t), n = n.next;
  }
}
function Hi(e) {
  z(e, X);
  for (var t = e.first; t !== null; )
    Hi(t), t = t.next;
}
let Dn = /* @__PURE__ */ new Set();
const $t = /* @__PURE__ */ new Map();
let ji = !1;
function Mt(e, t) {
  var n = {
    f: 0,
    // TODO ideally we could skip this altogether, but it causes type errors
    v: e,
    reactions: null,
    equals: bi,
    rv: 0,
    wv: 0
  };
  return n;
}
// @__NO_SIDE_EFFECTS__
function Re(e, t) {
  const n = Mt(e);
  return es(n), n;
}
// @__NO_SIDE_EFFECTS__
function Fi(e, t = !1, n = !0) {
  const r = Mt(e);
  return t || (r.equals = yi), r;
}
function Me(e, t, n = !1) {
  M !== null && // since we are untracking the function inside `$inspect.with` we need to add this check
  // to ensure we error if state is set inside an inspect effect
  (!Ne || (M.f & In) !== 0) && wi() && (M.f & (Z | Ae | jt | In)) !== 0 && (je === null || !je.has(e)) && sl();
  let r = n ? pt(t) : t;
  return Jt(e, r, Cn);
}
function Jt(e, t, n = null) {
  if (!e.equals(t)) {
    $t.set(e, tt ? t : e.v);
    var r = ut.ensure();
    if (r.capture(e, t), (e.f & Z) !== 0) {
      const i = (
        /** @type {Derived} */
        e
      );
      (e.f & G) !== 0 && Er(i), J === null && xr(i);
    }
    e.wv = ns(), Wi(e, G, n), S !== null && (S.f & X) !== 0 && (S.f & (Oe | et)) === 0 && (me === null ? Hl([e]) : me.push(e)), !r.is_fork && Dn.size > 0 && !ji && Sl();
  }
  return t;
}
function Sl() {
  ji = !1;
  for (const e of Dn) {
    (e.f & X) !== 0 && z(e, Fe);
    let t;
    try {
      t = bn(e);
    } catch {
      t = !0;
    }
    t && Zt(e);
  }
  Dn.clear();
}
function on(e) {
  Me(e, e.v + 1);
}
function Wi(e, t, n) {
  var r = e.reactions;
  if (r !== null)
    for (var i = r.length, s = 0; s < i; s++) {
      var l = r[s], a = l.f, o = (a & G) === 0;
      if (o && z(l, t), (a & In) !== 0)
        Dn.add(
          /** @type {Effect} */
          l
        );
      else if ((a & Z) !== 0) {
        var c = (
          /** @type {Derived} */
          l
        );
        J == null || J.delete(c), (a & St) === 0 && (a & xe && (S === null || (S.f & Rn) === 0) && (l.f |= St), Wi(c, Fe, n));
      } else if (o) {
        var h = (
          /** @type {Effect} */
          l
        );
        (a & Ae) !== 0 && Te !== null && Te.add(h), n !== null ? n.push(h) : $r(h);
      }
    }
}
function pt(e) {
  if (typeof e != "object" || e === null || kt in e)
    return e;
  const t = di(e);
  if (t !== Fs && t !== Ws)
    return e;
  var n = /* @__PURE__ */ new Map(), r = yr(e), i = /* @__PURE__ */ Re(0), s = Ct, l = (a) => {
    if (Ct === s)
      return a();
    var o = M, c = Ct;
    ke(null), Qr(s);
    var h = a();
    return ke(o), Qr(c), h;
  };
  return r && n.set("length", /* @__PURE__ */ Re(
    /** @type {any[]} */
    e.length
  )), new Proxy(
    /** @type {any} */
    e,
    {
      defineProperty(a, o, c) {
        (!("value" in c) || c.configurable === !1 || c.enumerable === !1 || c.writable === !1) && rl();
        var h = n.get(o);
        return h === void 0 ? l(() => {
          var b = /* @__PURE__ */ Re(c.value);
          return n.set(o, b), b;
        }) : Me(h, c.value, !0), !0;
      },
      deleteProperty(a, o) {
        var c = n.get(o);
        if (c === void 0) {
          if (o in a) {
            const h = l(() => /* @__PURE__ */ Re(U));
            n.set(o, h), on(i);
          }
        } else
          Me(c, U), on(i);
        return !0;
      },
      get(a, o, c) {
        var v;
        if (o === kt)
          return e;
        var h = n.get(o), b = o in a;
        if (h === void 0 && (!b || (v = Et(a, o)) != null && v.writable) && (h = l(() => {
          var g = pt(b ? a[o] : U), p = /* @__PURE__ */ Re(g);
          return p;
        }), n.set(o, h)), h !== void 0) {
          var d = N(h);
          return d === U ? void 0 : d;
        }
        return Reflect.get(a, o, c);
      },
      getOwnPropertyDescriptor(a, o) {
        var c = Reflect.getOwnPropertyDescriptor(a, o);
        if (c && "value" in c) {
          var h = n.get(o);
          h && (c.value = N(h));
        } else if (c === void 0) {
          var b = n.get(o), d = b == null ? void 0 : b.v;
          if (b !== void 0 && d !== U)
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
        if (o === kt)
          return !0;
        var c = n.get(o), h = c !== void 0 && c.v !== U || Reflect.has(a, o);
        if (c !== void 0 || S !== null && (!h || (d = Et(a, o)) != null && d.writable)) {
          c === void 0 && (c = l(() => {
            var v = h ? pt(a[o]) : U, g = /* @__PURE__ */ Re(v);
            return g;
          }), n.set(o, c));
          var b = N(c);
          if (b === U)
            return !1;
        }
        return h;
      },
      set(a, o, c, h) {
        var _;
        var b = n.get(o), d = o in a;
        if (r && o === "length")
          for (var v = c; v < /** @type {Source<number>} */
          b.v; v += 1) {
            var g = n.get(v + "");
            g !== void 0 ? Me(g, U) : v in a && (g = l(() => /* @__PURE__ */ Re(U)), n.set(v + "", g));
          }
        if (b === void 0)
          (!d || (_ = Et(a, o)) != null && _.writable) && (b = l(() => /* @__PURE__ */ Re(void 0)), Me(b, pt(c)), n.set(o, b));
        else {
          d = b.v !== U;
          var p = l(() => pt(c));
          Me(b, p);
        }
        var f = Reflect.getOwnPropertyDescriptor(a, o);
        if (f != null && f.set && f.set.call(h, c), !d) {
          if (r && typeof o == "string") {
            var m = (
              /** @type {Source<number>} */
              n.get("length")
            ), y = Number(o);
            Number.isInteger(y) && y >= m.v && Me(m, y + 1);
          }
          on(i);
        }
        return !0;
      },
      ownKeys(a) {
        N(i);
        var o = Reflect.ownKeys(a).filter((b) => {
          var d = n.get(b);
          return d === void 0 || d.v !== U;
        });
        for (var [c, h] of n)
          h.v !== U && !(c in a) && o.push(c);
        return o;
      },
      setPrototypeOf() {
        il();
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
function Ml(e, t) {
  return Object.is(Gr(e), Gr(t));
}
var Kr, Bi, Yi, zi;
function fr() {
  if (Kr === void 0) {
    Kr = window, Bi = /Firefox/.test(navigator.userAgent);
    var e = Element.prototype, t = Node.prototype, n = Text.prototype;
    Yi = Et(t, "firstChild").get, zi = Et(t, "nextSibling").get, Vr(e) && (e[Us] = void 0, e[kn] = null, e[Xs] = void 0, e.__e = void 0), Vr(n) && (n[en] = void 0);
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
    zi.call(e)
  );
}
function V(e, t) {
  if (!D)
    return /* @__PURE__ */ un(e);
  var n = /* @__PURE__ */ un(H);
  if (n === null)
    n = H.appendChild(He());
  else if (t && n.nodeType !== wr) {
    var r = He();
    return n == null || n.before(r), pe(r), r;
  }
  return t && Ui(
    /** @type {Text} */
    n
  ), pe(n), n;
}
function Ee(e, t = 1, n = !1) {
  let r = D ? H : e;
  for (var i; t--; )
    i = r, r = /** @type {TemplateNode} */
    /* @__PURE__ */ it(r);
  if (!D)
    return r;
  if (n) {
    if ((r == null ? void 0 : r.nodeType) !== wr) {
      var s = He();
      return r === null ? i == null || i.after(s) : r.before(s), pe(s), s;
    }
    Ui(
      /** @type {Text} */
      r
    );
  }
  return pe(r), r;
}
function qi(e) {
  e.textContent = "";
}
function Vi() {
  return !1;
}
function Tr(e, t, n) {
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
  for (; t !== null && t.nodeType === wr; )
    t.remove(), e.nodeValue += /** @type {string} */
    t.nodeValue, t = e.nextSibling;
}
let Jr = !1;
function Al() {
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
            (t = n[gi]) == null || t.call(n);
      });
    },
    // In the capture phase to guarantee we get noticed of it (no possibility of stopPropagation)
    { capture: !0 }
  ));
}
function Cr(e) {
  var t = M, n = S;
  ke(null), We(null);
  try {
    return e();
  } finally {
    ke(t), We(n);
  }
}
function Nl(e) {
  S === null && (M === null && Qs(), Zs()), tt && Js();
}
function Ol(e, t) {
  var n = t.last;
  n === null ? t.last = t.first = e : (n.next = e, e.prev = n, t.last = e);
}
function Be(e, t) {
  var n = S;
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
  if ((e & Xt) !== 0)
    Ht !== null ? Ht.push(r) : ut.ensure().schedule(r);
  else if (t !== null) {
    try {
      Zt(r);
    } catch (l) {
      throw ie(r), l;
    }
    i.deps === null && i.teardown === null && i.nodes === null && i.first === i.last && // either `null`, or a singular child
    (i.f & Nt) === 0 && (i = i.first, (e & Ae) !== 0 && (e & Gt) !== 0 && i !== null && (i.f |= Gt));
  }
  if (i !== null && (i.parent = n, n !== null && Ol(i, n), M !== null && (M.f & Z) !== 0 && (e & et) === 0)) {
    var s = (
      /** @type {Derived} */
      M
    );
    (s.effects ?? (s.effects = [])).push(i);
  }
  return r;
}
function Sr() {
  return M !== null && !Ne;
}
function Mr(e) {
  const t = Be(Yn, null);
  return z(t, X), t.teardown = e, t;
}
function Ar(e) {
  Nl();
  var t = (
    /** @type {Effect} */
    S.f
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
  return Be(Xt | qs, e);
}
function Il(e) {
  ut.ensure();
  const t = Be(et | Nt, e);
  return () => {
    ie(t);
  };
}
function Rl(e) {
  ut.ensure();
  const t = Be(et | Nt, e);
  return (n = {}) => new Promise((r) => {
    n.outro ? Tt(t, () => {
      ie(t), r(void 0);
    }) : (ie(t), r(void 0));
  });
}
function Gi(e) {
  return Be(Xt, e);
}
function Ll(e) {
  return Be(jt | Nt, e);
}
function Nr(e, t = 0) {
  return Be(Yn | t, e);
}
function ge(e, t = [], n = [], r = []) {
  yl(r, t, n, (i) => {
    Be(Yn, () => {
      e(...i.map(N));
    });
  });
}
function Or(e, t = 0) {
  var n = Be(Ae | t, e);
  return n;
}
function we(e) {
  return Be(Oe | Nt, e);
}
function Ki(e) {
  var t = e.teardown;
  if (t !== null) {
    const n = tt, r = M;
    Zr(!0), ke(null);
    try {
      t.call(null);
    } finally {
      Zr(n), ke(r);
    }
  }
}
function Ir(e, t = !1) {
  var n = e.first;
  for (e.first = e.last = null; n !== null; ) {
    const i = n.ac;
    i !== null && Cr(() => {
      i.abort(zn);
    });
    var r = n.next;
    (n.f & et) !== 0 ? n.parent = null : ie(n, t), n = r;
  }
}
function Pl(e) {
  for (var t = e.first; t !== null; ) {
    var n = t.next;
    (t.f & Oe) === 0 && ie(t), t = n;
  }
}
function ie(e, t = !0) {
  var n = !1;
  (t || (e.f & zs) !== 0) && e.nodes !== null && e.nodes.end !== null && (Dl(
    e.nodes.start,
    /** @type {TemplateNode} */
    e.nodes.end
  ), n = !0), e.f |= nr, Ir(e, t && !n), cn(e, 0);
  var r = e.nodes && e.nodes.t;
  if (r !== null)
    for (const s of r)
      s.stop();
  Ki(e), e.f ^= nr, e.f |= ve;
  var i = e.parent;
  i !== null && i.first !== null && Ji(e), e.next = e.prev = e.teardown = e.ctx = e.deps = e.fn = e.nodes = e.ac = e.b = null;
}
function Dl(e, t) {
  for (; e !== null; ) {
    var n = e === t ? null : /* @__PURE__ */ it(e);
    e.remove(), e = n;
  }
}
function Ji(e) {
  var t = e.parent, n = e.prev, r = e.next;
  n !== null && (n.next = r), r !== null && (r.prev = n), t !== null && (t.first === e && (t.first = r), t.last === e && (t.last = n));
}
function Tt(e, t, n = !0) {
  var r = [];
  Zi(e, r, !0);
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
function Zi(e, t, n) {
  if ((e.f & ne) === 0) {
    e.f ^= ne;
    var r = e.nodes && e.nodes.t;
    if (r !== null)
      for (const a of r)
        (a.is_global || n) && t.push(a);
    for (var i = e.first; i !== null; ) {
      var s = i.next;
      if ((i.f & et) === 0) {
        var l = (i.f & Gt) !== 0 || // If this is a branch effect without a block effect parent,
        // it means the parent block effect was pruned. In that case,
        // transparency information was transferred to the branch effect.
        (i.f & Oe) !== 0 && (e.f & Ae) !== 0;
        Zi(i, t, l ? n : !1);
      }
      i = s;
    }
  }
}
function Hn(e) {
  Qi(e, !0);
}
function Qi(e, t) {
  if ((e.f & ne) !== 0) {
    e.f ^= ne, (e.f & X) === 0 && (z(e, G), ut.ensure().schedule(e));
    for (var n = e.first; n !== null; ) {
      var r = n.next, i = (n.f & Gt) !== 0 || (n.f & Oe) !== 0;
      Qi(n, i ? t : !1), n = r;
    }
    var s = e.nodes && e.nodes.t;
    if (s !== null)
      for (const l of s)
        (l.is_global || t) && l.in();
  }
}
function Rr(e, t) {
  if (e.nodes)
    for (var n = e.nodes.start, r = e.nodes.end; n !== null; ) {
      var i = n === r ? null : /* @__PURE__ */ it(n);
      t.append(n), n = i;
    }
}
let Sn = !1, tt = !1;
function Zr(e) {
  tt = e;
}
let M = null, Ne = !1;
function ke(e) {
  M = e;
}
let S = null;
function We(e) {
  S = e;
}
let je = null;
function es(e) {
  M !== null && (je ?? (je = /* @__PURE__ */ new Set())).add(e);
}
let fe = null, ue = 0, me = null;
function Hl(e) {
  me = e;
}
let ts = 1, gt = 0, Ct = gt;
function Qr(e) {
  Ct = e;
}
function ns() {
  return ++ts;
}
function bn(e) {
  var t = e.f;
  if ((t & G) !== 0)
    return !0;
  if (t & Z && (e.f &= ~St), (t & Fe) !== 0) {
    for (var n = (
      /** @type {Value[]} */
      e.deps
    ), r = n.length, i = 0; i < r; i++) {
      var s = n[i];
      if (bn(
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
    J === null && z(e, X);
  }
  return !1;
}
function rs(e, t, n = !0) {
  var r = e.reactions;
  if (r !== null && !(je !== null && je.has(e)))
    for (var i = 0; i < r.length; i++) {
      var s = r[i];
      (s.f & Z) !== 0 ? rs(
        /** @type {Derived} */
        s,
        t,
        !1
      ) : t === s && (n ? z(s, G) : (s.f & X) !== 0 && z(s, Fe), $r(
        /** @type {Effect} */
        s
      ));
    }
}
function is(e) {
  var p;
  var t = fe, n = ue, r = me, i = M, s = je, l = re, a = Ne, o = Ct, c = e.f;
  fe = /** @type {null | Value[]} */
  null, ue = 0, me = null, M = (c & (Oe | et)) === 0 ? e : null, je = null, Kt(e.ctx), Ne = !1, Ct = ++gt, e.ac !== null && (Cr(() => {
    e.ac.abort(zn);
  }), e.ac = null);
  try {
    e.f |= Rn;
    var h = (
      /** @type {Function} */
      e.fn
    ), b = h();
    e.f |= At;
    var d = e.deps, v = T == null ? void 0 : T.is_fork;
    if (fe !== null) {
      var g;
      if (v || cn(e, ue), d !== null && ue > 0)
        for (d.length = ue + fe.length, g = 0; g < fe.length; g++)
          d[ue + g] = fe[g];
      else
        e.deps = d = fe;
      if (Sr() && (e.f & xe) !== 0)
        for (g = ue; g < d.length; g++)
          ((p = d[g]).reactions ?? (p.reactions = [])).push(e);
    } else !v && d !== null && ue < d.length && (cn(e, ue), d.length = ue);
    if (wi() && me !== null && !Ne && d !== null && (e.f & (Z | Fe | G)) === 0)
      for (g = 0; g < /** @type {Source[]} */
      me.length; g++)
        rs(
          me[g],
          /** @type {Effect} */
          e
        );
    if (i !== null && i !== e) {
      if (gt++, i.deps !== null)
        for (let f = 0; f < n; f += 1)
          i.deps[f].rv = gt;
      if (t !== null)
        for (const f of t)
          f.rv = gt;
      me !== null && (r === null ? r = me : r.push(.../** @type {Source[]} */
      me));
    }
    return (e.f & ft) !== 0 && (e.f ^= ft), b;
  } catch (f) {
    return Ei(f);
  } finally {
    e.f ^= Rn, fe = t, ue = n, me = r, M = i, je = s, Kt(l), Ne = a, Ct = o;
  }
}
function jl(e, t) {
  let n = t.reactions;
  if (n !== null) {
    var r = Hs.call(n, e);
    if (r !== -1) {
      var i = n.length - 1;
      i === 0 ? n = t.reactions = null : (n[r] = n[i], n.pop());
    }
  }
  if (n === null && (t.f & Z) !== 0 && // Destroying a child effect while updating a parent effect can cause a dependency to appear
  // to be unused, when in fact it is used by the currently-updating parent. Checking `new_deps`
  // allows us to skip the expensive work of disconnecting and immediately reconnecting it
  (fe === null || !An.call(fe, t))) {
    var s = (
      /** @type {Derived} */
      t
    );
    (s.f & xe) !== 0 && (s.f ^= xe, s.f &= ~St), s.v !== U && xr(s), kl(s), cn(s, 0);
  }
}
function cn(e, t) {
  var n = e.deps;
  if (n !== null)
    for (var r = t; r < n.length; r++)
      jl(e, n[r]);
}
function Zt(e) {
  var t = e.f;
  if ((t & ve) === 0) {
    z(e, X);
    var n = S, r = Sn;
    S = e, Sn = !0;
    try {
      (t & (Ae | vi)) !== 0 ? Pl(e) : Ir(e), Ki(e);
      var i = is(e);
      e.teardown = typeof i == "function" ? i : null, e.wv = ts;
      var s;
      ci && dl && (e.f & G) !== 0 && e.deps;
    } finally {
      Sn = r, S = n;
    }
  }
}
function N(e) {
  var t = e.f, n = (t & Z) !== 0;
  if (M !== null && !Ne) {
    var r = S !== null && (S.f & ve) !== 0;
    if (!r && (je === null || !je.has(e))) {
      var i = M.deps;
      if ((M.f & Rn) !== 0)
        e.rv < gt && (e.rv = gt, fe === null && i !== null && i[ue] === e ? ue++ : fe === null ? fe = [e] : fe.push(e));
      else {
        M.deps ?? (M.deps = []), An.call(M.deps, e) || M.deps.push(e);
        var s = e.reactions;
        s === null ? e.reactions = [M] : An.call(s, M) || s.push(M);
      }
    }
  }
  if (tt && $t.has(e))
    return $t.get(e);
  if (n) {
    var l = (
      /** @type {Derived} */
      e
    );
    if (tt) {
      var a = l.v;
      return ((l.f & X) === 0 && l.reactions !== null || ls(l)) && (a = Er(l)), $t.set(l, a), a;
    }
    var o = (l.f & xe) === 0 && !Ne && M !== null && (Sn || (M.f & xe) !== 0), c = (l.f & At) === 0;
    bn(l) && (o && (l.f |= xe), Oi(l)), o && !c && (Ii(l), ss(l));
  }
  if (J != null && J.has(e))
    return J.get(e);
  if ((e.f & ft) !== 0)
    throw e.v;
  return e.v;
}
function ss(e) {
  if (e.f |= xe, e.deps !== null)
    for (const t of e.deps)
      (t.reactions ?? (t.reactions = [])).push(e), (t.f & Z) !== 0 && (t.f & xe) === 0 && (Ii(
        /** @type {Derived} */
        t
      ), ss(
        /** @type {Derived} */
        t
      ));
}
function ls(e) {
  if (e.v === U) return !0;
  if (e.deps === null) return !1;
  for (const t of e.deps)
    if ($t.has(t) || (t.f & Z) !== 0 && ls(
      /** @type {Derived} */
      t
    ))
      return !0;
  return !1;
}
function Lr(e) {
  var t = Ne;
  try {
    return Ne = !0, e();
  } finally {
    Ne = t;
  }
}
const mt = Symbol("events"), as = /* @__PURE__ */ new Set(), ur = /* @__PURE__ */ new Set();
function Fl(e, t, n, r = {}) {
  function i(s) {
    if (r.capture || cr.call(t, s), !s.cancelBubble)
      return Cr(() => n == null ? void 0 : n.call(this, s));
  }
  return Ze(() => {
    t.addEventListener(e, i, r);
  }), i;
}
function os(e, t, n, r, i) {
  var s = { capture: r, passive: i }, l = Fl(e, t, n, s);
  (t === document.body || // @ts-ignore
  t === window || // @ts-ignore
  t === document || // Firefox has quirky behavior, it can happen that we still get "canplay" events when the element is already removed
  t instanceof HTMLMediaElement) && Mr(() => {
    t.removeEventListener(e, l, s);
  });
}
function Q(e, t, n) {
  (t[mt] ?? (t[mt] = {}))[e] = n;
}
function Ot(e) {
  for (var t = 0; t < e.length; t++)
    as.add(e[t]);
  for (var n of ur)
    n(e);
}
let ei = null;
function cr(e) {
  var p, f;
  var t = this, n = (
    /** @type {Node} */
    t.ownerDocument
  ), r = e.type, i = ((p = e.composedPath) == null ? void 0 : p.call(e)) || [], s = (
    /** @type {null | Element} */
    i[0] || e.target
  );
  ei = e;
  var l = 0, a = ei === e && e[mt];
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
    On(e, "currentTarget", {
      configurable: !0,
      get() {
        return s || n;
      }
    });
    var h = M, b = S;
    ke(null), We(null);
    try {
      for (var d, v = []; s !== null && s !== t; ) {
        try {
          var g = (f = s[mt]) == null ? void 0 : f[r];
          g != null && (!/** @type {any} */
          s.disabled || // DOM could've been updated already by the time this is reached, so we check this as well
          // -> the target could not have been disabled because it emits the event in the first place
          e.target === s) && g.call(s, e);
        } catch (m) {
          d ? v.push(m) : d = m;
        }
        if (e.cancelBubble) break;
        l++, s = l < i.length ? (
          /** @type {Element} */
          i[l]
        ) : null;
      }
      if (d) {
        for (let m of v)
          queueMicrotask(() => {
            throw m;
          });
        throw d;
      }
    } finally {
      e[mt] = t, delete e.currentTarget, ke(h), We(b);
    }
  }
}
var oi;
const Qn = (
  // We gotta write it like this because after downleveling the pure comment may end up in the wrong location
  ((oi = globalThis == null ? void 0 : globalThis.window) == null ? void 0 : oi.trustedTypes) && /* @__PURE__ */ globalThis.window.trustedTypes.createPolicy("svelte-trusted-html", {
    /** @param {string} html */
    createHTML: (e) => e
  })
);
function Wl(e) {
  return (
    /** @type {string} */
    (Qn == null ? void 0 : Qn.createHTML(e)) ?? e
  );
}
function Bl(e) {
  var t = Tr("template");
  return t.innerHTML = Wl(e.replaceAll("<!>", "<!---->")), t.content;
}
function dr(e, t) {
  var n = (
    /** @type {Effect} */
    S
  );
  n.nodes === null && (n.nodes = { start: e, end: t, a: null, t: null });
}
// @__NO_SIDE_EFFECTS__
function se(e, t) {
  var n = (t & Ps) !== 0, r, i = !e.startsWith("<!>");
  return () => {
    if (D)
      return dr(H, null), H;
    r === void 0 && (r = Bl(i ? e : "<!>" + e), r = /** @type {TemplateNode} */
    /* @__PURE__ */ un(r));
    var s = (
      /** @type {TemplateNode} */
      n || Bi ? document.importNode(r, !0) : r.cloneNode(!0)
    );
    return dr(s, s), s;
  };
}
function ee(e, t) {
  if (D) {
    var n = (
      /** @type {Effect & { nodes: EffectNodes }} */
      S
    );
    ((n.f & At) === 0 || n.nodes.end === null) && (n.nodes.end = H), Vn();
    return;
  }
  e !== null && e.before(
    /** @type {Node} */
    t
  );
}
const Yl = ["touchstart", "touchmove"];
function zl(e) {
  return Yl.includes(e);
}
function Ie(e, t) {
  var n = t == null ? "" : typeof t == "object" ? `${t}` : t;
  n !== /** @type {any} */
  (e[en] ?? (e[en] = e.nodeValue)) && (e[en] = n, e.nodeValue = `${n}`);
}
function fs(e, t) {
  return us(e, t);
}
function ql(e, t) {
  fr(), t.intro = t.intro ?? !1;
  const n = t.target, r = D, i = H;
  try {
    for (var s = /* @__PURE__ */ un(n); s && (s.nodeType !== _n || /** @type {Comment} */
    s.data !== ui); )
      s = /* @__PURE__ */ it(s);
    if (!s)
      throw Ut;
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
    return l !== Ut && console.warn("Failed to hydrate: ", l), t.recover === !1 && tl(), fr(), qi(n), Je(!1), fs(e, t);
  } finally {
    Je(r), pe(i);
  }
}
const En = /* @__PURE__ */ new Map();
function us(e, { target: t, anchor: n, props: r = {}, events: i, context: s, intro: l = !0, transformError: a }) {
  fr();
  var o = void 0, c = Rl(() => {
    var h = n ?? t.appendChild(He());
    _l(
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
        if (s && (g.c = s), i && (r.$$events = i), D && dr(
          /** @type {TemplateNode} */
          v,
          null
        ), o = e(v, r) || {}, D && (S.nodes.end = H, H === null || H.nodeType !== _n || /** @type {Comment} */
        H.data !== br))
          throw qn(), Ut;
        rt();
      },
      a
    );
    var b = /* @__PURE__ */ new Set(), d = (v) => {
      for (var g = 0; g < v.length; g++) {
        var p = v[g];
        if (!b.has(p)) {
          b.add(p);
          var f = zl(p);
          for (const _ of [t, document]) {
            var m = En.get(_);
            m === void 0 && (m = /* @__PURE__ */ new Map(), En.set(_, m));
            var y = m.get(p);
            y === void 0 ? (_.addEventListener(p, cr, { passive: f }), m.set(p, 1)) : m.set(p, y + 1);
          }
        }
      }
    };
    return d(Bn(as)), ur.add(d), () => {
      var f;
      for (var v of b)
        for (const m of [t, document]) {
          var g = (
            /** @type {Map<string, number>} */
            En.get(m)
          ), p = (
            /** @type {number} */
            g.get(v)
          );
          --p == 0 ? (m.removeEventListener(v, cr), g.delete(v), g.size === 0 && En.delete(m)) : g.set(v, p);
        }
      ur.delete(d), h !== n && ((f = h.parentNode) == null || f.removeChild(h));
    };
  });
  return hr.set(o, c), o;
}
let hr = /* @__PURE__ */ new WeakMap();
function Vl(e, t) {
  const n = hr.get(e);
  return n ? (hr.delete(e), n(t)) : Promise.resolve();
}
var Se, Pe, he, xt, gn, mn, Wn;
class Ul {
  /**
   * @param {TemplateNode} anchor
   * @param {boolean} transition
   */
  constructor(t, n = !0) {
    /** @type {TemplateNode} */
    W(this, "anchor");
    /** @type {Map<Batch, Key>} */
    C(this, Se, /* @__PURE__ */ new Map());
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
    C(this, Pe, /* @__PURE__ */ new Map());
    /**
     * Similar to #onscreen with respect to the keys, but contains branches that are not yet
     * in the DOM, because their insertion is deferred.
     * @type {Map<Key, Branch>}
     */
    C(this, he, /* @__PURE__ */ new Map());
    /**
     * Keys of effects that are currently outroing
     * @type {Set<Key>}
     */
    C(this, xt, /* @__PURE__ */ new Set());
    /**
     * Whether to pause (i.e. outro) on change, or destroy immediately.
     * This is necessary for `<svelte:element>`
     */
    C(this, gn, !0);
    /**
     * @param {Batch} batch
     */
    C(this, mn, (t) => {
      if (u(this, Se).has(t)) {
        var n = (
          /** @type {Key} */
          u(this, Se).get(t)
        ), r = u(this, Pe).get(n);
        if (r)
          Hn(r), u(this, xt).delete(n);
        else {
          var i = u(this, he).get(n);
          i && (Hn(i.effect), u(this, Pe).set(n, i.effect), u(this, he).delete(n), i.fragment.lastChild.remove(), this.anchor.before(i.fragment), r = i.effect);
        }
        for (const [s, l] of u(this, Se)) {
          if (u(this, Se).delete(s), s === t)
            break;
          const a = u(this, he).get(l);
          a && (ie(a.effect), u(this, he).delete(l));
        }
        for (const [s, l] of u(this, Pe)) {
          if (s === n || u(this, xt).has(s)) continue;
          const a = () => {
            if (Array.from(u(this, Se).values()).includes(s)) {
              var c = document.createDocumentFragment();
              Rr(l, c), c.append(He()), u(this, he).set(s, { effect: l, fragment: c });
            } else
              ie(l);
            u(this, xt).delete(s), u(this, Pe).delete(s);
          };
          u(this, gn) || !r ? (u(this, xt).add(s), Tt(l, a, !1)) : a();
        }
      }
    });
    /**
     * @param {Batch} batch
     */
    C(this, Wn, (t) => {
      u(this, Se).delete(t);
      const n = Array.from(u(this, Se).values());
      for (const [r, i] of u(this, he))
        n.includes(r) || (ie(i.effect), u(this, he).delete(r));
    });
    this.anchor = t, $(this, gn, n);
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
    if (n && !u(this, Pe).has(t) && !u(this, he).has(t))
      if (i) {
        var s = document.createDocumentFragment(), l = He();
        s.append(l), u(this, he).set(t, {
          effect: we(() => n(l)),
          fragment: s
        });
      } else
        u(this, Pe).set(
          t,
          we(() => n(this.anchor))
        );
    if (u(this, Se).set(r, t), i) {
      for (const [a, o] of u(this, Pe))
        a === t ? r.unskip_effect(o) : r.skip_effect(o);
      for (const [a, o] of u(this, he))
        a === t ? r.unskip_effect(o.effect) : r.skip_effect(o.effect);
      r.oncommit(u(this, mn)), r.ondiscard(u(this, Wn));
    } else
      D && (this.anchor = H), u(this, mn).call(this, r);
  }
}
Se = new WeakMap(), Pe = new WeakMap(), he = new WeakMap(), xt = new WeakMap(), gn = new WeakMap(), mn = new WeakMap(), Wn = new WeakMap();
function yn(e, t, n = !1) {
  var r;
  D && (r = H, Vn());
  var i = new Ul(e), s = n ? Gt : 0;
  function l(a, o) {
    if (D) {
      var c = _i(
        /** @type {TemplateNode} */
        r
      );
      if (a !== parseInt(c.substring(1))) {
        var h = Ln();
        pe(h), i.anchor = h, Je(!1), i.ensure(a, o), Je(!0);
        return;
      }
    }
    i.ensure(a, o);
  }
  Or(() => {
    var a = !1;
    t((o, c = 0) => {
      a = !0, l(c, o);
    }), a || l(-1, null);
  }, s);
}
function cs(e, t) {
  return t;
}
function Xl(e, t, n) {
  for (var r = [], i = t.length, s, l = t.length, a = 0; a < i; a++) {
    let b = t[a];
    Tt(
      b,
      () => {
        if (s) {
          if (s.pending.delete(b), s.done.add(b), s.pending.size === 0) {
            var d = (
              /** @type {Set<EachOutroGroup>} */
              e.outrogroups
            );
            vr(e, Bn(s.done)), d.delete(s), d.size === 0 && (e.outrogroups = null);
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
      s.f |= De;
      const l = document.createDocumentFragment();
      Rr(s, l);
    } else
      ie(t[i], n);
  }
}
var ti;
function ds(e, t, n, r, i, s = null) {
  var l = e, a = /* @__PURE__ */ new Map(), o = (t & fi) !== 0;
  if (o) {
    var c = (
      /** @type {Element} */
      e
    );
    l = D ? pe(/* @__PURE__ */ un(c)) : c.appendChild(He());
  }
  D && Vn();
  var h = null, b = /* @__PURE__ */ Ni(() => {
    var _ = n();
    return (
      /** @type {V[]} */
      yr(_) ? _ : _ == null ? [] : Bn(_)
    );
  }), d, v = /* @__PURE__ */ new Map(), g = !0;
  function p(_) {
    (y.effect.f & ve) === 0 && (y.pending.delete(_), y.fallback = h, Gl(y, d, l, t, r), h !== null && (d.length === 0 ? (h.f & De) === 0 ? Hn(h) : (h.f ^= De, sn(h, null, l)) : Tt(h, () => {
      h = null;
    })));
  }
  function f(_) {
    y.pending.delete(_);
  }
  var m = Or(() => {
    d = /** @type {V[]} */
    N(b);
    var _ = d.length;
    let E = !1;
    if (D) {
      var w = _i(l) === _r;
      w !== (_ === 0) && (l = Ln(), pe(l), Je(!1), E = !0);
    }
    for (var k = /* @__PURE__ */ new Set(), I = (
      /** @type {Batch} */
      T
    ), B = Vi(), P = 0; P < _; P += 1) {
      D && H.nodeType === _n && /** @type {Comment} */
      H.data === br && (l = /** @type {Comment} */
      H, E = !0, Je(!1));
      var j = d[P], Y = r(j, P), K = g ? null : a.get(Y);
      K ? (K.v && Jt(K.v, j), K.i && Jt(K.i, P), B && I.unskip_effect(K.e)) : (K = Kl(
        a,
        g ? l : ti ?? (ti = He()),
        j,
        Y,
        P,
        i,
        t,
        n
      ), g || (K.e.f |= De), a.set(Y, K)), k.add(Y);
    }
    if (_ === 0 && s && !h && (g ? h = we(() => s(l)) : (h = we(() => s(ti ?? (ti = He()))), h.f |= De)), _ > k.size && Ks(), D && _ > 0 && pe(Ln()), !g)
      if (v.set(I, k), B) {
        for (const [ht, Lt] of a)
          k.has(ht) || I.skip_effect(Lt.e);
        I.oncommit(p), I.ondiscard(f);
      } else
        p(I);
    E && Je(!0), N(b);
  }), y = { effect: m, items: a, pending: v, outrogroups: null, fallback: h };
  g = !1, D && (l = H);
}
function Qt(e) {
  for (; e !== null && (e.f & Oe) === 0; )
    e = e.next;
  return e;
}
function Gl(e, t, n, r, i) {
  var j, Y, K, ht, Lt, ze, x, le, Br;
  var s = (r & As) !== 0, l = t.length, a = e.items, o = Qt(e.effect.first), c, h = null, b, d = [], v = [], g, p, f, m;
  if (s)
    for (m = 0; m < l; m += 1)
      g = t[m], p = i(g, m), f = /** @type {EachItem} */
      a.get(p).e, (f.f & De) === 0 && ((Y = (j = f.nodes) == null ? void 0 : j.a) == null || Y.measure(), (b ?? (b = /* @__PURE__ */ new Set())).add(f));
  for (m = 0; m < l; m += 1) {
    if (g = t[m], p = i(g, m), f = /** @type {EachItem} */
    a.get(p).e, e.outrogroups !== null)
      for (const qe of e.outrogroups)
        qe.pending.delete(f), qe.done.delete(f);
    if ((f.f & ne) !== 0 && (Hn(f), s && ((ht = (K = f.nodes) == null ? void 0 : K.a) == null || ht.unfix(), (b ?? (b = /* @__PURE__ */ new Set())).delete(f))), (f.f & De) !== 0)
      if (f.f ^= De, f === o)
        sn(f, null, n);
      else {
        var y = h ? h.next : o;
        f === e.effect.last && (e.effect.last = f.prev), f.prev && (f.prev.next = f.next), f.next && (f.next.prev = f.prev), st(e, h, f), st(e, f, y), sn(f, y, n), h = f, d = [], v = [], o = Qt(h.next);
        continue;
      }
    if (f !== o) {
      if (c !== void 0 && c.has(f)) {
        if (d.length < v.length) {
          var _ = v[0], E;
          h = _.prev;
          var w = d[0], k = d[d.length - 1];
          for (E = 0; E < d.length; E += 1)
            sn(d[E], _, n);
          for (E = 0; E < v.length; E += 1)
            c.delete(v[E]);
          st(e, w.prev, k.next), st(e, h, w), st(e, k, _), o = _, h = k, m -= 1, d = [], v = [];
        } else
          c.delete(f), sn(f, o, n), st(e, f.prev, f.next), st(e, f, h === null ? e.effect.first : h.next), st(e, h, f), h = f;
        continue;
      }
      for (d = [], v = []; o !== null && o !== f; )
        (c ?? (c = /* @__PURE__ */ new Set())).add(o), v.push(o), o = Qt(o.next);
      if (o === null)
        continue;
    }
    (f.f & De) === 0 && d.push(f), h = f, o = Qt(f.next);
  }
  if (e.outrogroups !== null) {
    for (const qe of e.outrogroups)
      qe.pending.size === 0 && (vr(e, Bn(qe.done)), (Lt = e.outrogroups) == null || Lt.delete(qe));
    e.outrogroups.size === 0 && (e.outrogroups = null);
  }
  if (o !== null || c !== void 0) {
    var I = [];
    if (c !== void 0)
      for (f of c)
        (f.f & ne) === 0 && I.push(f);
    for (; o !== null; )
      (o.f & ne) === 0 && o !== e.fallback && I.push(o), o = Qt(o.next);
    var B = I.length;
    if (B > 0) {
      var P = (r & fi) !== 0 && l === 0 ? n : null;
      if (s) {
        for (m = 0; m < B; m += 1)
          (x = (ze = I[m].nodes) == null ? void 0 : ze.a) == null || x.measure();
        for (m = 0; m < B; m += 1)
          (Br = (le = I[m].nodes) == null ? void 0 : le.a) == null || Br.fix();
      }
      Xl(e, I, P);
    }
  }
  s && Ze(() => {
    var qe, Yr;
    if (b !== void 0)
      for (f of b)
        (Yr = (qe = f.nodes) == null ? void 0 : qe.a) == null || Yr.apply();
  });
}
function Kl(e, t, n, r, i, s, l, a) {
  var o = (l & Ss) !== 0 ? (l & Ns) === 0 ? /* @__PURE__ */ Fi(n, !1, !1) : Mt(n) : null, c = (l & Ms) !== 0 ? Mt(i) : null;
  return {
    v: o,
    i: c,
    e: we(() => (s(t, o ?? n, c ?? i, a), () => {
      e.delete(r);
    }))
  };
}
function sn(e, t, n) {
  if (e.nodes)
    for (var r = e.nodes.start, i = e.nodes.end, s = t && (t.f & De) === 0 ? (
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
  Gi(() => {
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
      const i = Tr("style");
      i.id = t.hash, i.textContent = t.code, r.appendChild(i);
    }
  });
}
function hs(e, t, n = !1) {
  if (e.multiple) {
    if (t == null)
      return;
    if (!yr(t))
      return ol();
    for (var r of e.options)
      r.selected = t.includes(ni(r));
    return;
  }
  for (r of e.options) {
    var i = ni(r);
    if (Ml(i, t)) {
      r.selected = !0;
      return;
    }
  }
  (!n || t !== void 0) && (e.selectedIndex = -1);
}
function Jl(e) {
  var t = new MutationObserver(() => {
    hs(e, e.__value);
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
  }), Mr(() => {
    t.disconnect();
  });
}
function ni(e) {
  return "__value" in e ? e.__value : e.value;
}
const Zl = Symbol("is custom element"), Ql = Symbol("is html"), ea = mi ? "link" : "LINK", ta = mi ? "progress" : "PROGRESS";
function Un(e) {
  if (D) {
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
    e[gi] = n, Ze(n), Al();
  }
}
function Pr(e, t) {
  var n = Dr(e);
  n.value === (n.value = // treat null and undefined the same for the initial value
  t ?? void 0) || // @ts-expect-error
  // `progress` elements always need their value set when it's `0`
  e.value === t && (t !== 0 || e.nodeName !== ta) || (e.value = t ?? "");
}
function vs(e, t) {
  var n = Dr(e);
  n.checked !== (n.checked = // treat null and undefined the same for the initial value
  t ?? void 0) && (e.checked = t);
}
function na(e, t) {
  t ? e.hasAttribute("selected") || e.setAttribute("selected", "") : e.removeAttribute("selected");
}
function Qe(e, t, n, r) {
  var i = Dr(e);
  D && (i[t] = e.getAttribute(t), t === "src" || t === "srcset" || t === "href" && e.nodeName === ea) || i[t] !== (i[t] = n) && (t === "loading" && (e[Vs] = n), n == null ? e.removeAttribute(t) : typeof n != "string" && ra(e).includes(t) ? e[t] = n : e.setAttribute(t, n));
}
function Dr(e) {
  return (
    /** @type {Record<string | symbol, unknown>} **/
    /** @type {any} */
    e[kn] ?? (e[kn] = {
      [Zl]: e.nodeName.includes("-"),
      [Ql]: e.namespaceURI === Ds
    })
  );
}
var ri = /* @__PURE__ */ new Map();
function ra(e) {
  var t = e.getAttribute("is") || e.nodeName, n = ri.get(t);
  if (n) return n;
  ri.set(t, n = []);
  for (var r, i = e, s = Element.prototype; s !== i; ) {
    r = js(i);
    for (var l in r)
      r[l].set && // better safe than sorry, we don't want spread attributes to mess with HTML content
      l !== "innerHTML" && l !== "textContent" && l !== "innerText" && n.push(l);
    i = di(i);
  }
  return n;
}
function er(e, t) {
  return e === t || (e == null ? void 0 : e[kt]) === t;
}
function Hr(e = {}, t, n, r) {
  var i = (
    /** @type {ComponentContext} */
    re.r
  ), s = (
    /** @type {Effect} */
    S
  );
  return Gi(() => {
    var l, a;
    return Nr(() => {
      l = a, a = [], Lr(() => {
        er(n(...a), e) || (t(e, ...a), l && er(n(...l), e) && t(null, ...l));
      });
    }), () => {
      let o = s;
      for (; o !== i && o.parent !== null && o.parent.f & nr; )
        o = o.parent;
      const c = () => {
        a && er(n(...a), e) && t(null, ...a);
      }, h = o.teardown;
      o.teardown = () => {
        c(), h == null || h();
      };
    };
  }), e;
}
function R(e, t, n, r) {
  var E;
  var i = !0, s = (n & Rs) !== 0, l = (n & Ls) !== 0, a = (
    /** @type {V} */
    r
  ), o = !0, c = (
    /** @type {Derived<V> | undefined} */
    void 0
  ), h = () => l && i ? (c ?? (c = /* @__PURE__ */ fn(
    /** @type {() => V} */
    r
  )), N(c)) : (o && (o = !1, a = l ? Lr(
    /** @type {() => V} */
    r
  ) : (
    /** @type {V} */
    r
  )), a);
  let b;
  if (s) {
    var d = kt in e || pi in e;
    b = ((E = Et(e, t)) == null ? void 0 : E.set) ?? (d && t in e ? (w) => e[t] = w : void 0);
  }
  var v, g = !1;
  s ? [v, g] = pl(() => (
    /** @type {V} */
    e[t]
  )) : v = /** @type {V} */
  e[t], v === void 0 && r !== void 0 && (v = h(), b && (nl(), b(v)));
  var p;
  if (p = () => {
    var w = (
      /** @type {V} */
      e[t]
    );
    return w === void 0 ? h() : (o = !0, w);
  }, (n & Is) === 0)
    return p;
  if (b) {
    var f = e.$$legacy;
    return (
      /** @type {() => V} */
      (function(w, k) {
        return arguments.length > 0 ? ((!k || f || g) && b(k ? p() : w), w) : p();
      })
    );
  }
  var m = !1, y = ((n & Os) !== 0 ? fn : Ni)(() => (m = !1, p()));
  s && N(y);
  var _ = (
    /** @type {Effect} */
    S
  );
  return (
    /** @type {() => V} */
    (function(w, k) {
      if (arguments.length > 0) {
        const I = k ? N(y) : s ? pt(w) : w;
        return Me(y, I), m = !0, a !== void 0 && (a = I), w;
      }
      return tt && m || (_.f & ve) !== 0 ? y.v : N(y);
    })
  );
}
function ia(e) {
  return new sa(e);
}
var Ke, ye;
class sa {
  /**
   * @param {ComponentConstructorOptions & {
   *  component: any;
   * }} options
   */
  constructor(t) {
    /** @type {any} */
    C(this, Ke);
    /** @type {Record<string, any>} */
    C(this, ye);
    var s;
    var n = /* @__PURE__ */ new Map(), r = (l, a) => {
      var o = /* @__PURE__ */ Fi(a, !1, !1);
      return n.set(l, o), o;
    };
    const i = new Proxy(
      { ...t.props || {}, $$events: {} },
      {
        get(l, a) {
          return N(n.get(a) ?? r(a, Reflect.get(l, a)));
        },
        has(l, a) {
          return a === pi ? !0 : (N(n.get(a) ?? r(a, Reflect.get(l, a))), Reflect.has(l, a));
        },
        set(l, a, o) {
          return Me(n.get(a) ?? r(a, o), o), Reflect.set(l, a, o);
        }
      }
    );
    $(this, ye, (t.hydrate ? ql : fs)(t.component, {
      target: t.target,
      anchor: t.anchor,
      props: i,
      context: t.context,
      intro: t.intro ?? !1,
      recover: t.recover,
      transformError: t.transformError
    })), (!((s = t == null ? void 0 : t.props) != null && s.$$host) || t.sync === !1) && O(), $(this, Ke, i.$$events);
    for (const l of Object.keys(u(this, ye)))
      l === "$set" || l === "$destroy" || l === "$on" || On(this, l, {
        get() {
          return u(this, ye)[l];
        },
        /** @param {any} value */
        set(a) {
          u(this, ye)[l] = a;
        },
        enumerable: !0
      });
    u(this, ye).$set = /** @param {Record<string, any>} next */
    (l) => {
      Object.assign(i, l);
    }, u(this, ye).$destroy = () => {
      Vl(u(this, ye));
    };
  }
  /** @param {Record<string, any>} props */
  $set(t) {
    u(this, ye).$set(t);
  }
  /**
   * @param {string} event
   * @param {(...args: any[]) => any} callback
   * @returns {any}
   */
  $on(t, n) {
    u(this, Ke)[t] = u(this, Ke)[t] || [];
    const r = (...i) => n.call(this, ...i);
    return u(this, Ke)[t].push(r), () => {
      u(this, Ke)[t] = u(this, Ke)[t].filter(
        /** @param {any} fn */
        (i) => i !== r
      );
    };
  }
  $destroy() {
    u(this, ye).$destroy();
  }
}
Ke = new WeakMap(), ye = new WeakMap();
let ps;
typeof HTMLElement == "function" && (ps = class extends HTMLElement {
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
          const l = Tr("slot");
          i !== "default" && (l.name = i), ee(s, l);
        };
      };
      if (await Promise.resolve(), !this.$$cn || this.$$c)
        return;
      const n = {}, r = la(this);
      for (const i of this.$$s)
        i in r && (i === "default" && !this.$$d.children ? (this.$$d.children = t(i), n.default = !0) : n[i] = t(i));
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
      }), this.$$me = Il(() => {
        Nr(() => {
          var i;
          this.$$r = !0;
          for (const s of Nn(this.$$c)) {
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
  attributeChangedCallback(t, n, r) {
    var i;
    this.$$r || (t = this.$$g_p(t), this.$$d[t] = Mn(t, r, this.$$p_d, "toProp"), (i = this.$$c) == null || i.$set({ [t]: this.$$d[t] }));
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
function Mn(e, t, n, r) {
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
function la(e) {
  const t = {};
  return e.childNodes.forEach((n) => {
    t[
      /** @type {Element} node */
      n.slot || "default"
    ] = !0;
  }), t;
}
function dt(e, t, n, r, i, s) {
  let l = class extends ps {
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
    On(l.prototype, a, {
      get() {
        return this.$$c && a in this.$$c ? this.$$c[a] : this.$$d[a];
      },
      set(o) {
        var b;
        o = Mn(a, o, t), this.$$d[a] = o;
        var c = this.$$c;
        if (c) {
          var h = (b = Et(c, a)) == null ? void 0 : b.get;
          h ? c[a] = o : c.$set({ [a]: o });
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
var aa = /* @__PURE__ */ se('<span class="lbl"> </span>'), oa = /* @__PURE__ */ se('<label class="xi-slider svelte-8xo3l7"><!> <input type="range" class="svelte-8xo3l7"/> <span class="val svelte-8xo3l7"> </span></label>');
const fa = {
  hash: "svelte-8xo3l7",
  code: '.xi-slider.svelte-8xo3l7 {display:inline-flex;align-items:center;gap:0.5rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}input[type="range"].svelte-8xo3l7 {accent-color:var(--xi-accent, #3b82f6);vertical-align:middle;}.val.svelte-8xo3l7 {min-width:2.5em;text-align:right;font-variant-numeric:tabular-nums;}'
};
function ua(e, t) {
  nt(t, !0), ct(e, fa);
  let n = R(t, "value", 15, 0), r = R(t, "min", 7, 0), i = R(t, "max", 7, 100), s = R(t, "step", 7, 1), l = R(t, "label", 7, ""), a = R(t, "disabled", 7, !1);
  const o = t.$$host, c = (_) => o.dispatchEvent(new CustomEvent(_, { detail: { value: n() }, bubbles: !0, composed: !0 }));
  function h(_) {
    n(Number(_.target.value)), c("input");
  }
  function b(_) {
    n(Number(_.target.value)), c("change");
  }
  var d = {
    get value() {
      return n();
    },
    set value(_ = 0) {
      n(_), O();
    },
    get min() {
      return r();
    },
    set min(_ = 0) {
      r(_), O();
    },
    get max() {
      return i();
    },
    set max(_ = 100) {
      i(_), O();
    },
    get step() {
      return s();
    },
    set step(_ = 1) {
      s(_), O();
    },
    get label() {
      return l();
    },
    set label(_ = "") {
      l(_), O();
    },
    get disabled() {
      return a();
    },
    set disabled(_ = !1) {
      a(_), O();
    }
  }, v = oa(), g = V(v);
  {
    var p = (_) => {
      var E = aa(), w = V(E, !0);
      q(E), ge(() => Ie(w, l())), ee(_, E);
    };
    yn(g, (_) => {
      l() && _(p);
    });
  }
  var f = Ee(g, 2);
  Un(f);
  var m = Ee(f, 2), y = V(m, !0);
  return q(m), q(v), ge(() => {
    Qe(f, "min", r()), Qe(f, "max", i()), Qe(f, "step", s()), Pr(f, n()), f.disabled = a(), Ie(y, n());
  }), Q("input", f, h), Q("change", f, b), ee(e, v), rt(d);
}
Ot(["input", "change"]);
customElements.define("xi-slider", dt(
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
var ca = /* @__PURE__ */ se('<span class="lbl"> </span>'), da = /* @__PURE__ */ se('<label class="xi-number svelte-1f6ykwb"><!> <input type="number" class="svelte-1f6ykwb"/></label>');
const ha = {
  hash: "svelte-1f6ykwb",
  code: ".xi-number.svelte-1f6ykwb {display:inline-flex;align-items:center;gap:0.5rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}input.svelte-1f6ykwb {width:6em;padding:0.15rem 0.3rem;accent-color:var(--xi-accent, #3b82f6);}"
};
function va(e, t) {
  nt(t, !0), ct(e, ha);
  let n = R(t, "value", 15, 0), r = R(t, "min", 7), i = R(t, "max", 7), s = R(t, "step", 7, 1), l = R(t, "label", 7, ""), a = R(t, "disabled", 7, !1);
  const o = t.$$host, c = (y) => o.dispatchEvent(new CustomEvent(y, { detail: { value: n() }, bubbles: !0, composed: !0 })), h = (y) => y.target.value === "" ? null : Number(y.target.value);
  function b(y) {
    n(h(y)), c("input");
  }
  function d(y) {
    n(h(y)), c("change");
  }
  var v = {
    get value() {
      return n();
    },
    set value(y = 0) {
      n(y), O();
    },
    get min() {
      return r();
    },
    set min(y) {
      r(y), O();
    },
    get max() {
      return i();
    },
    set max(y) {
      i(y), O();
    },
    get step() {
      return s();
    },
    set step(y = 1) {
      s(y), O();
    },
    get label() {
      return l();
    },
    set label(y = "") {
      l(y), O();
    },
    get disabled() {
      return a();
    },
    set disabled(y = !1) {
      a(y), O();
    }
  }, g = da(), p = V(g);
  {
    var f = (y) => {
      var _ = ca(), E = V(_, !0);
      q(_), ge(() => Ie(E, l())), ee(y, _);
    };
    yn(p, (y) => {
      l() && y(f);
    });
  }
  var m = Ee(p, 2);
  return Un(m), q(g), ge(() => {
    Qe(m, "min", r()), Qe(m, "max", i()), Qe(m, "step", s()), Pr(m, n()), m.disabled = a();
  }), Q("input", m, b), Q("change", m, d), ee(e, g), rt(v);
}
Ot(["input", "change"]);
customElements.define("xi-number", dt(
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
var pa = /* @__PURE__ */ se('<span class="lbl"> </span>'), ga = /* @__PURE__ */ se('<label class="xi-toggle svelte-141rfru"><input type="checkbox" class="svelte-141rfru"/> <!></label>');
const ma = {
  hash: "svelte-141rfru",
  code: ".xi-toggle.svelte-141rfru {display:inline-flex;align-items:center;gap:0.4rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);cursor:pointer;}input.svelte-141rfru {accent-color:var(--xi-accent, #3b82f6);}"
};
function _a(e, t) {
  nt(t, !0), ct(e, ma);
  let n = R(t, "value", 15, !1), r = R(t, "label", 7, ""), i = R(t, "disabled", 7, !1);
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
  }, o = ga(), c = V(o);
  Un(c);
  var h = Ee(c, 2);
  {
    var b = (d) => {
      var v = pa(), g = V(v, !0);
      q(v), ge(() => Ie(g, r())), ee(d, v);
    };
    yn(h, (d) => {
      r() && d(b);
    });
  }
  return q(o), ge(() => {
    vs(c, n()), c.disabled = i();
  }), Q("change", c, l), ee(e, o), rt(a);
}
Ot(["change"]);
customElements.define("xi-toggle", dt(
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
function gs(e) {
  let t = e;
  if (typeof e == "string")
    try {
      t = JSON.parse(e);
    } catch {
      t = [];
    }
  return Array.isArray(t) ? t.map((n) => n && typeof n == "object" ? { value: n.value, label: n.label ?? String(n.value) } : { value: n, label: String(n) }) : [];
}
var ba = /* @__PURE__ */ se('<span class="lbl"> </span>'), ya = /* @__PURE__ */ se('<label class="opt svelte-mtcxej"><input type="radio" class="svelte-mtcxej"/> <span> </span></label>'), wa = /* @__PURE__ */ se('<div class="xi-radio svelte-mtcxej" role="radiogroup"><!> <!></div>');
const xa = {
  hash: "svelte-mtcxej",
  code: ".xi-radio.svelte-mtcxej {display:inline-flex;align-items:center;gap:0.75rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}.opt.svelte-mtcxej {display:inline-flex;align-items:center;gap:0.25rem;cursor:pointer;}input.svelte-mtcxej {accent-color:var(--xi-accent, #3b82f6);}"
};
function Ea(e, t) {
  nt(t, !0), ct(e, xa);
  let n = R(t, "value", 15, ""), r = R(t, "options", 23, () => []), i = R(t, "label", 7, ""), s = R(t, "disabled", 7, !1), l = R(t, "name", 7, "xi-radio");
  const a = t.$$host, o = /* @__PURE__ */ Ai(() => gs(r()));
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
  }, b = wa(), d = V(b);
  {
    var v = (p) => {
      var f = ba(), m = V(f, !0);
      q(f), ge(() => Ie(m, i())), ee(p, f);
    };
    yn(d, (p) => {
      i() && p(v);
    });
  }
  var g = Ee(d, 2);
  return ds(g, 17, () => N(o), cs, (p, f) => {
    var m = ya(), y = V(m);
    Un(y);
    var _ = Ee(y, 2), E = V(_, !0);
    q(_), q(m), ge(() => {
      Qe(y, "name", l()), Pr(y, N(f).value), vs(y, N(f).value === n()), y.disabled = s(), Ie(E, N(f).label);
    }), Q("change", y, () => c(N(f).value)), ee(p, m);
  }), q(b), ee(e, b), rt(h);
}
Ot(["change"]);
customElements.define("xi-radio", dt(
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
var ka = /* @__PURE__ */ se('<span class="lbl"> </span>'), $a = /* @__PURE__ */ se("<option> </option>"), Ta = /* @__PURE__ */ se('<label class="xi-dropdown svelte-1wd9iqr"><!> <select class="svelte-1wd9iqr"></select></label>');
const Ca = {
  hash: "svelte-1wd9iqr",
  code: ".xi-dropdown.svelte-1wd9iqr {display:inline-flex;align-items:center;gap:0.5rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}select.svelte-1wd9iqr {padding:0.15rem 0.3rem;}"
};
function Sa(e, t) {
  nt(t, !0), ct(e, Ca);
  let n = R(t, "value", 15, ""), r = R(t, "options", 23, () => []), i = R(t, "label", 7, ""), s = R(t, "disabled", 7, !1);
  const l = t.$$host, a = /* @__PURE__ */ Ai(() => gs(r()));
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
  }, h = Ta(), b = V(h);
  {
    var d = (p) => {
      var f = ka(), m = V(f, !0);
      q(f), ge(() => Ie(m, i())), ee(p, f);
    };
    yn(b, (p) => {
      i() && p(d);
    });
  }
  var v = Ee(b, 2);
  ds(v, 21, () => N(a), cs, (p, f) => {
    var m = $a(), y = V(m, !0);
    q(m);
    var _ = {};
    ge(() => {
      na(m, N(f).value === n()), Ie(y, N(f).label), _ !== (_ = N(f).value) && (m.value = (m.__value = N(f).value) ?? "");
    }), ee(p, m);
  }), q(v);
  var g;
  return Jl(v), q(h), ge(() => {
    v.disabled = s(), g !== (g = n()) && (v.value = (v.__value = n()) ?? "", hs(v, n()));
  }), Q("change", v, o), ee(e, h), rt(c);
}
Ot(["change"]);
customElements.define("xi-dropdown", dt(
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
var Ma = /* @__PURE__ */ se('<div class="xi-trace svelte-1m1z2sd"><span class="lbl"> </span> <canvas class="svelte-1m1z2sd"></canvas> <span class="val svelte-1m1z2sd"> </span></div>');
const Aa = {
  hash: "svelte-1m1z2sd",
  code: ".xi-trace.svelte-1m1z2sd {display:inline-flex;align-items:center;gap:0.5rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}canvas.svelte-1m1z2sd {width:120px;height:28px;display:block;}.val.svelte-1m1z2sd {min-width:3em;text-align:right;font-variant-numeric:tabular-nums;}"
};
function Na(e, t) {
  nt(t, !0), ct(e, Aa);
  let n = R(t, "key", 7, ""), r = R(t, "label", 7, ""), i = R(t, "max", 7, 60);
  const s = t.$$host;
  let l, a = /* @__PURE__ */ Re(null), o = /* @__PURE__ */ Re(pt([]));
  function c() {
    if (!l) return;
    const _ = l.getContext && l.getContext("2d");
    if (!_) return;
    const E = l.width = l.clientWidth || 120, w = l.height = l.clientHeight || 28;
    if (_.clearRect(0, 0, E, w), N(o).length < 2) return;
    const k = Math.min(...N(o)), I = Math.max(...N(o)), B = I - k || 1;
    _.beginPath(), N(o).forEach((P, j) => {
      const Y = j / (N(o).length - 1) * (E - 2) + 1, K = w - 2 - (P - k) / B * (w - 4);
      j ? _.lineTo(Y, K) : _.moveTo(Y, K);
    }), _.strokeStyle = getComputedStyle(s).getPropertyValue("--xi-accent") || "#3b82f6", _.lineWidth = 1.5, _.stroke();
  }
  function h(_) {
    const E = _ && _[n()];
    E && (Me(a, E.value, !0), typeof E.value == "number" && Number.isFinite(E.value) && (Me(o, [...N(o), E.value].slice(-i()), !0), c()), s.dispatchEvent(new CustomEvent("sample", { detail: { value: E.value }, bubbles: !0, composed: !0 })));
  }
  Ar(() => {
    s.update = h, Object.defineProperty(s, "latest", { get: () => N(a), configurable: !0 }), Object.defineProperty(s, "history", { get: () => N(o).slice(), configurable: !0 }), c();
  });
  const b = (_) => _ == null ? "—" : typeof _ == "number" ? Number.isInteger(_) ? _ : _.toFixed(3) : String(_);
  var d = {
    get key() {
      return n();
    },
    set key(_ = "") {
      n(_), O();
    },
    get label() {
      return r();
    },
    set label(_ = "") {
      r(_), O();
    },
    get max() {
      return i();
    },
    set max(_ = 60) {
      i(_), O();
    }
  }, v = Ma(), g = V(v), p = V(g, !0);
  q(g);
  var f = Ee(g, 2);
  Hr(f, (_) => l = _, () => l);
  var m = Ee(f, 2), y = V(m, !0);
  return q(m), q(v), ge(
    (_) => {
      Ie(p, r() || n()), Ie(y, _);
    },
    [() => b(N(a))]
  ), ee(e, v), rt(d);
}
customElements.define("xi-trace", dt(Na, { key: {}, label: {}, max: { type: "Number" } }, [], [], { mode: "open" }));
function ms() {
  return { scale: 1, panX: 0, panY: 0, imgW: 0, imgH: 0, viewW: 0, viewH: 0 };
}
function jr(e, t, n) {
  return { x: (t - e.panX) / e.scale, y: (n - e.panY) / e.scale };
}
function Oa(e, t, n) {
  return { x: e.panX + t * e.scale, y: e.panY + n * e.scale };
}
const Ia = 0.05, Ra = 64, La = (e) => Math.max(Ia, Math.min(Ra, e));
function pr(e) {
  return !e.imgW || !e.imgH || !e.viewW || !e.viewH || (e.scale = Math.min(e.viewW / e.imgW, e.viewH / e.imgH) * 0.95, e.panX = (e.viewW - e.imgW * e.scale) / 2, e.panY = (e.viewH - e.imgH * e.scale) / 2), e;
}
function Pa(e) {
  return e.scale = 1, e.panX = (e.viewW - e.imgW) / 2, e.panY = (e.viewH - e.imgH) / 2, e;
}
function _s(e, t, n, r) {
  const { x: i, y: s } = jr(e, t, n);
  return e.scale = La(e.scale * r), e.panX = t - i * e.scale, e.panY = n - s * e.scale, e;
}
function Da(e, t, n) {
  return e.panX += t, e.panY += n, e;
}
var Ha = /* @__PURE__ */ se('<canvas class="svelte-1yjweo0"></canvas>');
const ja = {
  hash: "svelte-1yjweo0",
  code: ":host {display:block;width:100%;height:100%;}canvas.svelte-1yjweo0 {width:100%;height:100%;display:block;background:var(--xi-viewer-bg, #111);cursor:grab;touch-action:none;image-rendering:pixelated;}canvas.svelte-1yjweo0:active {cursor:grabbing;}"
};
function Fa(e, t) {
  nt(t, !0), ct(e, ja);
  const n = t.$$host;
  let r;
  const i = ms();
  let s = null, l = null;
  function a() {
    if (!r) return;
    const w = r.getContext("2d");
    w.imageSmoothingEnabled = !1, w.clearRect(0, 0, r.width, r.height), s && (w.setTransform(i.scale, 0, 0, i.scale, i.panX, i.panY), w.drawImage(s, 0, 0), w.setTransform(1, 0, 0, 1, 0, 0));
  }
  function o() {
    if (!r) return;
    const w = r.getBoundingClientRect();
    r.width = Math.max(1, Math.round(w.width)), r.height = Math.max(1, Math.round(w.height)), i.viewW = r.width, i.viewH = r.height, a();
  }
  function c(w, k) {
    n.dispatchEvent(new CustomEvent(w, { detail: k, bubbles: !0, composed: !0 }));
  }
  function h(w) {
    return !!w && typeof w != "string" && !("dataUrl" in w) && (typeof HTMLImageElement < "u" && w instanceof HTMLImageElement || typeof HTMLCanvasElement < "u" && w instanceof HTMLCanvasElement || typeof OffscreenCanvas < "u" && w instanceof OffscreenCanvas || typeof ImageBitmap < "u" && w instanceof ImageBitmap);
  }
  function b(w) {
    if (h(w)) {
      d(w);
      return;
    }
    const k = new Image();
    k.onload = () => d(k), k.src = typeof w == "string" ? w : w.dataUrl;
  }
  function d(w) {
    const k = !i.imgW;
    s = w, i.imgW = w.naturalWidth || w.width, i.imgH = w.naturalHeight || w.height, l = document.createElement("canvas"), l.width = i.imgW, l.height = i.imgH, l.getContext("2d").drawImage(w, 0, 0), k && pr(i), a();
  }
  function v(w) {
    if (!s) return;
    w.preventDefault();
    const k = r.getBoundingClientRect();
    _s(i, w.clientX - k.left, w.clientY - k.top, w.deltaY < 0 ? 1.15 : 1 / 1.15), a(), c("viewchange", { scale: i.scale });
  }
  let g = null, p = !1;
  function f(w) {
    var k;
    s && (g = { x: w.clientX, y: w.clientY }, p = !1, (k = r.setPointerCapture) == null || k.call(r, w.pointerId));
  }
  function m(w) {
    if (!g) return;
    const k = w.clientX - g.x, I = w.clientY - g.y;
    (k || I) && (p = !0), Da(i, k, I), g = { x: w.clientX, y: w.clientY }, a();
  }
  function y(w) {
    g && !p && _(w), g = null;
  }
  function _(w) {
    if (!s || !l) return;
    const k = r.getBoundingClientRect(), I = jr(i, w.clientX - k.left, w.clientY - k.top), B = Math.floor(I.x), P = Math.floor(I.y);
    let j = null;
    if (B >= 0 && P >= 0 && B < i.imgW && P < i.imgH) {
      const Y = l.getContext("2d").getImageData(B, P, 1, 1).data;
      j = [Y[0], Y[1], Y[2]];
    }
    c("pixelpick", { x: B, y: P, rgb: j });
  }
  Ar(() => {
    n.setFrame = b, n.fit = () => {
      pr(i), a(), c("viewchange", { scale: i.scale });
    }, n.oneToOne = () => {
      Pa(i), a(), c("viewchange", { scale: i.scale });
    }, o();
    const w = new ResizeObserver(o);
    return w.observe(r), () => w.disconnect();
  });
  var E = Ha();
  Hr(E, (w) => r = w, () => r), os("wheel", E, v), Q("pointerdown", E, f), Q("pointermove", E, m), Q("pointerup", E, y), ee(e, E), rt();
}
Ot(["pointerdown", "pointermove", "pointerup"]);
customElements.define("xi-image-viewer", dt(Fa, {}, [], [], { mode: "open" }));
function Wa() {
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
function Ba() {
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
function Ya() {
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
const gr = { point: Wa, rect: Ba, polygon: Ya };
function uo(e, t) {
  gr[e] = t;
}
function ii(e) {
  return gr[e] ? gr[e]() : null;
}
var za = /* @__PURE__ */ se('<div class="xi-editor svelte-1qrk1oj"><div class="bar svelte-1qrk1oj"><span class="lbl"> </span> <span class="spacer svelte-1qrk1oj"></span> <button class="cancel svelte-1qrk1oj">Cancel</button> <button class="commit svelte-1qrk1oj">Commit</button></div> <canvas class="svelte-1qrk1oj"></canvas></div>');
const qa = {
  hash: "svelte-1qrk1oj",
  code: ":host {display:block;width:100%;height:100%;}.xi-editor.svelte-1qrk1oj {display:flex;flex-direction:column;height:100%;}.bar.svelte-1qrk1oj {display:flex;align-items:center;gap:0.5rem;padding:0.35rem 0.5rem;font:13px system-ui, sans-serif;background:var(--xi-bar-bg, #1f2937);color:#e5e7eb;}.spacer.svelte-1qrk1oj {flex:1;}button.svelte-1qrk1oj {padding:0.2rem 0.7rem;border:0;border-radius:4px;cursor:pointer;font:inherit;}.commit.svelte-1qrk1oj {background:var(--xi-accent, #6366f1);color:#fff;}.cancel.svelte-1qrk1oj {background:#374151;color:#e5e7eb;}canvas.svelte-1qrk1oj {flex:1;width:100%;display:block;background:var(--xi-viewer-bg, #111);cursor:crosshair;touch-action:none;image-rendering:pixelated;}"
};
function Va(e, t) {
  nt(t, !0), ct(e, qa);
  let n = R(t, "tool", 7, "rect"), r = R(t, "label", 7, "");
  const i = t.$$host;
  let s;
  const l = ms();
  let a = null, o = ii(n());
  const c = (x) => Oa(l, x.x, x.y);
  function h() {
    if (!s) return;
    const x = s.getContext("2d");
    x && (x.imageSmoothingEnabled = !1, x.setTransform(1, 0, 0, 1, 0, 0), x.clearRect(0, 0, s.width, s.height), a && (x.setTransform(l.scale, 0, 0, l.scale, l.panX, l.panY), x.drawImage(a, 0, 0), x.setTransform(1, 0, 0, 1, 0, 0)), o && o.draw(x, c));
  }
  function b() {
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
    a = x, l.imgW = x.naturalWidth || x.width, l.imgH = x.naturalHeight || x.height, le && pr(l), h();
  }
  function p(x) {
    o = ii(x) || o, h();
  }
  const f = (x) => {
    const le = s.getBoundingClientRect();
    return jr(l, x.clientX - le.left, x.clientY - le.top);
  };
  function m(x) {
    o && (o.onDown(f(x)), h());
  }
  function y(x) {
    o && x.buttons && (o.onMove(f(x)), h());
  }
  function _(x) {
    o && (o.onUp(f(x)), h());
  }
  function E(x) {
    o && (o.onDbl(f(x)), h());
  }
  function w(x) {
    if (!a) return;
    x.preventDefault();
    const le = s.getBoundingClientRect();
    _s(l, x.clientX - le.left, x.clientY - le.top, x.deltaY < 0 ? 1.15 : 1 / 1.15), h();
  }
  function k() {
    !o || !o.done() || i.dispatchEvent(new CustomEvent("commit", {
      detail: { tool: o.type, result: o.result() },
      bubbles: !0,
      composed: !0
    }));
  }
  function I() {
    i.dispatchEvent(new CustomEvent("cancel", { detail: {}, bubbles: !0, composed: !0 }));
  }
  Ar(() => {
    i.setFrame = v, i.setTool = p, i.getResult = () => o && o.done() ? o.result() : null, b();
    const x = new ResizeObserver(b);
    return x.observe(s), () => x.disconnect();
  });
  var B = {
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
  }, P = za(), j = V(P), Y = V(j), K = V(Y, !0);
  q(Y);
  var ht = Ee(Y, 4), Lt = Ee(ht, 2);
  q(j);
  var ze = Ee(j, 2);
  return Hr(ze, (x) => s = x, () => s), q(P), ge(() => Ie(K, r() || n())), Q("click", ht, I), Q("click", Lt, k), Q("pointerdown", ze, m), Q("pointermove", ze, y), Q("pointerup", ze, _), Q("dblclick", ze, E), os("wheel", ze, w), ee(e, P), rt(B);
}
Ot([
  "click",
  "pointerdown",
  "pointermove",
  "pointerup",
  "dblclick"
]);
customElements.define("xi-image-editor", dt(Va, { tool: {}, label: {} }, [], [], { mode: "open" }));
function bs(e) {
  const t = typeof e == "string" ? JSON.parse(e) : e, n = t.items || t.vars || [], r = {};
  for (const i of n) r[i.name] = i;
  return { run_id: t.run_id, items: r };
}
const Ua = { 0: "image/jpeg", 1: "image/bmp", 2: "image/png" };
function ys(e) {
  const t = e instanceof Uint8Array ? e : new Uint8Array(e);
  if (t.byteLength < 20) throw new Error("preview frame shorter than 20-byte header");
  const n = new DataView(t.buffer, t.byteOffset, t.byteLength), r = n.getUint32(0, !1), i = n.getUint32(4, !1), s = n.getUint32(8, !1), l = n.getUint32(12, !1), a = n.getUint32(16, !1), o = t.subarray(20), c = Ua[i] || "application/octet-stream";
  return {
    gid: r,
    codec: i,
    width: s,
    height: l,
    channels: a,
    dataUrl: `data:${c};base64,${ws(o)}`
  };
}
function ws(e) {
  if (typeof Buffer < "u") return Buffer.from(e).toString("base64");
  let t = "";
  const n = 32768;
  for (let r = 0; r < e.length; r += n)
    t += String.fromCharCode.apply(null, e.subarray(r, r + n));
  return btoa(t);
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
  _deliverPreview(t) {
    if (!(typeof window < "u" && typeof Image < "u" && !/jsdom/i.test(typeof navigator < "u" && navigator.userAgent || ""))) {
      this._emit("preview", t);
      return;
    }
    const r = new Image();
    let i = !1;
    const s = () => {
      i || (i = !0, this._emit("preview", t));
    };
    r.onload = () => {
      t.image = r, s();
    }, r.onerror = s, r.src = t.dataUrl;
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
const Xa = {
  slider: "xi-slider",
  number: "xi-number",
  toggle: "xi-toggle",
  radio: "xi-radio",
  dropdown: "xi-dropdown"
};
function Ga(e, { section: t = "Config", tag: n = "control" } = {}) {
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
async function vo(e, t) {
  const { client: n, instance: r, sectionFilter: i } = t, s = e.ownerDocument || globalThis.document, l = await n.getInstanceDef(r) || {}, a = { ...l }, o = t.descriptor && t.descriptor.length ? t.descriptor : Ga(l), c = [];
  e.innerHTML = "";
  for (const h of o) {
    if (i && !i(h)) continue;
    const b = s.createElement("section");
    if (b.className = "xi-section", b.dataset.tag = h.tag || "control", h.section) {
      const d = s.createElement("h3");
      d.className = "xi-section-title", d.textContent = h.section, b.appendChild(d);
    }
    for (const d of h.controls || []) {
      const v = Xa[d.type] || "xi-number", g = s.createElement(v);
      d.label && g.setAttribute("label", d.label);
      for (const f of ["min", "max", "step"]) d[f] != null && g.setAttribute(f, String(d[f]));
      const p = s.createElement("div");
      p.className = "xi-control", p.appendChild(g), b.appendChild(p), d.options != null && (g.options = d.options), d.key in a && (g.value = a[d.key]), g.addEventListener("change", async (f) => {
        a[d.key] = f.detail.value;
        try {
          await n.setInstanceDef(r, { ...a });
        } catch {
        }
        e.dispatchEvent(new CustomEvent("xi-change", { detail: { key: d.key, value: f.detail.value }, bubbles: !0 }));
      }), c.push({ el: g, key: d.key });
    }
    e.appendChild(b);
  }
  return {
    // Re-read the def from the backend and push values back into the controls.
    async refresh() {
      const h = await n.getInstanceDef(r) || {};
      Object.assign(a, h);
      for (const { el: b, key: d } of c) d in a && (b.value = a[d]);
    },
    destroy() {
      e.innerHTML = "";
    }
  };
}
function po(e) {
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
function go(e, { client: t, items: n, columns: r = 3 }) {
  const i = e.ownerDocument || globalThis.document;
  e.innerHTML = "";
  const s = i.createElement("div");
  s.className = "xi-monitor", s.style.display = "grid", s.style.gap = "0.75rem", s.style.gridTemplateColumns = `repeat(${r}, minmax(0, 1fr))`, e.appendChild(s);
  const l = /* @__PURE__ */ new Map(), a = /* @__PURE__ */ new Map(), o = /* @__PURE__ */ new Map();
  for (const v of n) {
    const g = i.createElement("div");
    g.className = "xi-tile", g.dataset.key = v.key;
    const p = i.createElement("div");
    p.className = "xi-tile-label", p.textContent = v.label, g.appendChild(p);
    let f;
    v.type === "trace" ? (f = i.createElement("xi-trace"), f.setAttribute("key", v.key)) : v.type === "image" ? (f = i.createElement("xi-image-viewer"), f.style.height = "180px") : (f = i.createElement("div"), f.className = "xi-value", f.textContent = "—"), g.appendChild(f), s.appendChild(g), l.set(v.key, { type: v.type, el: f });
  }
  const c = (v) => {
    const g = v.items || {};
    a.clear(), o.clear();
    for (const [p, f] of l) {
      const m = g[p];
      if (m)
        if (f.type === "trace") f.el.update(g);
        else if (f.type === "image") {
          if (m.gid != null) {
            const y = m.src != null ? m.src : m.gid;
            o.set(m.gid, y);
            let _ = a.get(y);
            _ || a.set(y, _ = /* @__PURE__ */ new Set()), _.add(f.el);
          }
        } else f.el.textContent = Ka(m.value);
    }
  }, h = (v) => {
    const g = o.has(v.gid) ? o.get(v.gid) : v.gid, p = a.get(g), f = v.image || v.dataUrl;
    if (p) for (const m of p) m.setFrame(f);
  }, b = t.onVars(c), d = t.onPreview(h);
  return { destroy() {
    b(), d(), e.innerHTML = "";
  } };
}
function Ka(e) {
  return e == null ? "—" : typeof e == "number" ? Number.isInteger(e) ? String(e) : e.toFixed(3) : typeof e == "boolean" ? e ? "true" : "false" : String(e);
}
const Ja = `
  :host { display:block; height:100%; box-sizing:border-box; background:#1e1e1e;
          border:1px solid #333; border-radius:6px; color:#ddd; overflow:hidden;
          font:13px/1.4 system-ui,sans-serif; }
  .hd { padding:4px 8px; font-size:11px; letter-spacing:.04em; text-transform:uppercase;
        color:#888; border-bottom:1px solid #2a2a2a; }
  .body { padding:8px; height:calc(100% - 24px); box-sizing:border-box; }
`;
function It(e, t) {
  return e.attachShadow({ mode: "open" }), e.shadowRoot.innerHTML = `<style>${Ja}</style>
    <div class="hd">${t || ""}</div><div class="body"></div>`, e.shadowRoot.querySelector(".body");
}
const Xn = (e, t) => e.config && e.config.title || e.binding && e.binding.var || t, Gn = (e, t) => t && e.vars[t.var] ? e.vars[t.var].value : void 0;
function xs(e) {
  return e == null ? { kind: "none", label: "—", color: "#bbb" } : e <= -99e4 ? { kind: "sys", label: "SYS", color: "#c084fc" } : e > 0 ? { kind: "ok", label: e > 1 ? `OK${e}` : "OK", color: "#3ad17a" } : e < 0 ? { kind: "ng", label: e < -1 ? `NG${-e}` : "NG", color: "#ff5b5b" } : { kind: "na", label: "NA", color: "#ffb454" };
}
const Es = (e) => !e || e.result === !0 || !e.var;
class Za extends HTMLElement {
  connectedCallback() {
    this.body = It(this, Xn(this, "Verdict")), this.body.style.cssText = "display:flex;flex-direction:column;align-items:center;justify-content:center;gap:6px;text-align:center", this.big = document.createElement("div"), this.big.style.cssText = "font-weight:800;font-size:clamp(20px,7vw,72px);line-height:1", this.sub = document.createElement("div"), this.sub.style.cssText = "font-size:12px;color:#888;max-width:96%;overflow:hidden;text-overflow:ellipsis;white-space:nowrap", this.body.append(this.big, this.sub);
  }
  feed(t) {
    const n = this.binding || {};
    if (Es(n)) {
      const l = t.result, a = xs(l ? l.code : null);
      this.big.textContent = a.label, this.big.style.color = a.color, this.sub.textContent = l && l.msg ? l.msg : "";
      return;
    }
    const r = Gn(t, n), i = r === !0 || r === "OK" || r === "ok" || r === "PASS", s = r === !1 || r === "NG" || r === "ng" || r === "FAIL";
    this.big.textContent = r === void 0 ? "—" : i ? "OK" : s ? "NG" : String(r), this.big.style.color = i ? "#3ad17a" : s ? "#ff5b5b" : "#ccc", this.sub.textContent = "";
  }
}
class Qa extends HTMLElement {
  connectedCallback() {
    this.body = It(this, Xn(this, "Value")), this.body.style.cssText = "display:flex;align-items:center;justify-content:center;font-size:clamp(16px,5vw,40px);font-weight:600";
  }
  feed(t) {
    var r;
    const n = Gn(t, this.binding);
    this.body.textContent = n === void 0 ? "—" : typeof n == "number" ? +n.toFixed(((r = this.config) == null ? void 0 : r.decimals) ?? 3) : String(n);
  }
}
class eo extends HTMLElement {
  connectedCallback() {
    this.body = It(this, Xn(this, "Image")), this.body.style.cssText = "padding:0", this.viewer = document.createElement("xi-image-viewer"), this.viewer.style.cssText = "width:100%;height:100%;display:block", this.body.appendChild(this.viewer);
  }
  feed(t) {
    const n = this.binding && t.vars[this.binding.var], r = n ? n.src != null ? n.src : n.gid : void 0, i = n && r != null ? t.images[r] : void 0;
    i && i !== this._u && typeof this.viewer.setFrame == "function" && (this.viewer.setFrame(i), this._u = i);
  }
}
class to extends HTMLElement {
  connectedCallback() {
    this.body = It(this, Xn(this, "SPC")), this.buf = [], this.last = -1, this.cv = document.createElement("canvas"), this.cv.style.cssText = "width:100%;height:100%", this.body.appendChild(this.cv);
  }
  feed(t) {
    var n;
    if (t.run_id !== this.last) {
      this.last = t.run_id;
      const r = Gn(t, this.binding);
      if (typeof r == "number") {
        this.buf.push(r);
        const i = ((n = this.config) == null ? void 0 : n.window) || 100;
        this.buf.length > i && this.buf.shift();
      }
    }
    this.draw();
  }
  draw() {
    var d, v, g;
    const t = this.cv, n = t.getBoundingClientRect();
    if (!n.width) return;
    t.width = n.width, t.height = n.height;
    const r = t.getContext("2d");
    if (r.clearRect(0, 0, t.width, t.height), !this.buf.length) return;
    const i = ((d = this.config) == null ? void 0 : d.mean) ?? this.buf.reduce((p, f) => p + f, 0) / this.buf.length, s = (v = this.config) == null ? void 0 : v.ucl, l = (g = this.config) == null ? void 0 : g.lcl;
    let a = Math.min(...this.buf), o = Math.max(...this.buf);
    s != null && (o = Math.max(o, s)), l != null && (a = Math.min(a, l));
    const c = (o - a) * 0.1 || 1;
    a -= c, o += c;
    const h = (p) => t.height - (p - a) / (o - a) * t.height, b = (p, f, m) => {
      p != null && (r.strokeStyle = f, r.setLineDash(m || []), r.beginPath(), r.moveTo(0, h(p)), r.lineTo(t.width, h(p)), r.stroke(), r.setLineDash([]));
    };
    b(i, "#666"), b(s, "#ff5b5b", [4, 3]), b(l, "#ff5b5b", [4, 3]), r.strokeStyle = "#4aa0f0", r.lineWidth = 1.5, r.beginPath(), this.buf.forEach((p, f) => {
      const m = f / Math.max(1, this.buf.length - 1) * t.width;
      f ? r.lineTo(m, h(p)) : r.moveTo(m, h(p));
    }), r.stroke();
  }
}
class no extends HTMLElement {
  connectedCallback() {
    var t;
    this.body = It(this, ((t = this.config) == null ? void 0 : t.title) || "Throughput"), this.buf = [], this.last = -1, this.body.style.cssText = "display:flex;flex-direction:column;align-items:center;justify-content:center;gap:4px", this.big = document.createElement("div"), this.big.style.cssText = "font-size:clamp(18px,6vw,44px);font-weight:700;color:#9ad", this.sub = document.createElement("div"), this.sub.style.cssText = "font-size:12px;color:#888", this.body.append(this.big, this.sub);
  }
  feed(t) {
    if (t.run_id !== this.last && t.run_ms != null && (this.last = t.run_id, this.buf.push(t.run_ms), this.buf.length > 30 && this.buf.shift()), this.buf.length) {
      const n = this.buf.reduce((r, i) => r + i, 0) / this.buf.length;
      this.big.textContent = `${(6e4 / n).toFixed(0)} /min`, this.sub.textContent = `cycle ${n.toFixed(1)} ms`;
    }
  }
}
class ro extends HTMLElement {
  connectedCallback() {
    var t;
    this.body = It(this, ((t = this.config) == null ? void 0 : t.title) || "Yield"), this.ok = 0, this.ng = 0, this.last = -1, this.body.style.cssText = "display:flex;flex-direction:column;align-items:center;justify-content:center;gap:4px", this.big = document.createElement("div"), this.big.style.cssText = "font-size:clamp(18px,6vw,44px);font-weight:700", this.sub = document.createElement("div"), this.sub.style.cssText = "font-size:12px;color:#888", this.body.append(this.big, this.sub);
  }
  feed(t) {
    var s;
    const n = this.binding || {};
    if (Es(n)) {
      const l = t.result;
      if (l && l.run_id != null && l.run_id !== this.last) {
        this.last = l.run_id;
        const a = xs(l.code);
        a.kind === "ok" ? this.ok++ : a.kind === "ng" ? this.ng++ : a.kind === "na" && (this.na = (this.na || 0) + 1);
      }
    } else if (t.run_id !== this.last) {
      this.last = t.run_id;
      const l = Gn(t, n);
      l !== void 0 && (l === !0 || l === "OK" || l === "ok" || l === "PASS" ? this.ok++ : this.ng++);
    }
    const r = this.ok + this.ng, i = r ? 100 * this.ok / r : 0;
    this.big.textContent = `${i.toFixed(1)}%`, this.big.style.color = i >= (((s = this.config) == null ? void 0 : s.warn) ?? 95) ? "#3ad17a" : "#ffb454", this.sub.textContent = `OK ${this.ok} / NG ${this.ng}` + (this.na ? ` · NA ${this.na}` : "");
  }
}
class io extends HTMLElement {
  connectedCallback() {
    var t;
    this.body = It(this, ((t = this.config) == null ? void 0 : t.title) || "Dispatch groups"), this.body.style.cssText = "display:flex;flex-direction:column;gap:10px;padding:10px;overflow:auto", this.peak = {}, this.rows = {};
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
const ks = {
  verdict: Za,
  value: Qa,
  image: eo,
  spc: to,
  throughput: no,
  yield: ro,
  groups: io
};
for (const [e, t] of Object.entries(ks)) customElements.define(`xi-card-${e}`, t);
const Fr = (e) => !!(e && e.card), Rt = (e) => !!(e && (e.dir === "row" || e.dir === "col") && Array.isArray(e.children) && e.children.length >= 1), Ye = (e) => !!(e && Array.isArray(e.tabs) && e.tabs.length >= 1 && e.tabs.every((t) => t && t.child)), wn = () => ({ type: "value", bind: {}, config: { title: "(empty)" } });
function Wr(e) {
  const t = e.children.length;
  return (Array.isArray(e.weights) && e.weights.length === t ? e.weights.slice() : Array(t).fill(1)).map((r) => typeof r == "number" && r > 0 ? r : 1);
}
function so(e) {
  const t = Wr(e), n = t.reduce((r, i) => r + i, 0) || 1;
  return t.map((r) => r / n);
}
function $s(e, t) {
  return Ye(e) ? e.tabs[t].child : e.children[t];
}
function lo(e, t, n) {
  if (Ye(e)) {
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
  Rt(e) ? e.children.forEach((r, i) => mr(r, t, [...n, i])) : Ye(e) && e.tabs.forEach((r, i) => mr(r.child, t, [...n, i]));
}
function mo(e) {
  let t = 0;
  return mr(e, () => t++), t;
}
function ao(e, t) {
  let n = e;
  for (const r of t)
    if (Rt(n) || Ye(n)) n = $s(n, r);
    else return;
  return n;
}
function $e(e, t, n) {
  if (t.length === 0) return n(e);
  const [r, ...i] = t;
  return lo(e, r, $e($s(e, r), i, n));
}
function _o(e, t, n, r = wn()) {
  return $e(e, t, (i) => ({ dir: n === "col" ? "col" : "row", children: [i, { card: r }], weights: [1, 1] }));
}
function bo(e, t, n, r = wn()) {
  if (n = n === "col" ? "col" : "row", t.length === 0) return { dir: n, children: [e, { card: r }], weights: [1, 1] };
  const i = t.slice(0, -1), s = t[t.length - 1], l = ao(e, i);
  return Rt(l) && l.dir === n ? $e(e, i, (a) => {
    const o = a.children.slice();
    o.splice(s + 1, 0, { card: r });
    const c = Wr(a);
    return c.splice(s + 1, 0, c[s]), { ...a, children: o, weights: c };
  }) : $e(e, t, (a) => ({ dir: n, children: [a, { card: r }], weights: [1, 1] }));
}
function yo(e, t) {
  if (t.length === 0) return { card: wn() };
  const n = t.slice(0, -1), r = t[t.length - 1];
  return $e(e, n, (i) => {
    if (!Rt(i)) return i;
    const s = i.children.filter((a, o) => o !== r), l = Wr(i).filter((a, o) => o !== r);
    return s.length === 1 ? s[0] : { ...i, children: s, weights: l };
  });
}
function wo(e, t, n) {
  return $e(e, t, () => ({ card: n }));
}
function xo(e, t, n) {
  return $e(e, t, (r) => Rt(r) ? { ...r, weights: n.slice() } : r);
}
function Eo(e, t) {
  return $e(e, t, (n) => ({ tabs: [{ name: "Page 1", child: n }, { name: "Page 2", child: { card: wn() } }], active: 0 }));
}
function ko(e, t, n, r = { card: wn() }) {
  return $e(e, t, (i) => Ye(i) ? { ...i, tabs: [...i.tabs, { name: n || `Page ${i.tabs.length + 1}`, child: r }], active: i.tabs.length } : i);
}
function $o(e, t, n) {
  return $e(e, t, (r) => {
    if (!Ye(r)) return r;
    const i = r.tabs.filter((s, l) => l !== n);
    return i.length === 1 ? i[0].child : { ...r, tabs: i, active: Math.max(0, Math.min(r.active || 0, i.length - 1)) };
  });
}
function To(e, t, n, r) {
  return $e(e, t, (i) => Ye(i) ? { ...i, tabs: i.tabs.map((s, l) => l === n ? { ...s, name: r } : s) } : i);
}
function Co(e, t, n) {
  return $e(e, t, (r) => Ye(r) ? { ...r, active: n } : r);
}
function si(e, t = "root") {
  return Fr(e) ? e.card.type ? [] : [`${t}: leaf has no card.type`] : Rt(e) ? e.children.flatMap((n, r) => si(n, `${t}.${r}`)) : Ye(e) ? e.tabs.flatMap((n, r) => si(n.child, `${t}.${n.name || r}`)) : [`${t}: node is not a leaf {card}, split {dir,children}, or tabs {tabs}`];
}
function So(e, { client: t, dashboard: n, pollStatsMs: r = 200 }) {
  const i = e.ownerDocument || globalThis.document, s = globalThis.requestAnimationFrame || ((f) => setTimeout(f, 16)), l = { run_id: -1, vars: {}, images: {}, run_ms: null, status: null, result: null, groups: [] };
  let a = [], o = 0;
  function c() {
    o || (o = s(() => {
      o = 0;
      for (const f of a)
        try {
          f.feed(l);
        } catch {
        }
    }));
  }
  function h(f) {
    const m = ks[f.type], y = i.createElement(m ? `xi-card-${f.type}` : "div");
    return m || (y.textContent = `unknown card: ${f.type}`, y.style.cssText = "color:#f88;padding:8px"), y.binding = f.bind || {}, y.config = f.config || {}, y.style.minWidth = "0", y.style.minHeight = "0", y.style.overflow = "hidden", m && a.push(y), y;
  }
  function b(f) {
    let m = Math.min(f.active || 0, f.tabs.length - 1);
    const y = i.createElement("div");
    y.style.cssText = "display:flex;flex-direction:column;min-width:0;min-height:0;width:100%;height:100%";
    const _ = i.createElement("div");
    _.style.cssText = "display:flex;gap:2px;flex:0 0 auto;align-items:center;padding:2px 2px 0;overflow:auto";
    const E = i.createElement("div");
    E.style.cssText = "flex:1 1 0;min-width:0;min-height:0;position:relative";
    const w = [], k = [], I = () => {
      w.forEach((B, P) => {
        const j = P === m;
        B.style.background = j ? "#1e1e1e" : "#121212", B.style.color = j ? "#ddd" : "#888";
      }), k.forEach((B, P) => {
        B.style.display = P === m ? "" : "none";
      });
    };
    return f.tabs.forEach((B, P) => {
      const j = i.createElement("div");
      j.style.cssText = "padding:4px 11px;cursor:pointer;border-radius:6px 6px 0 0;border:1px solid #333;border-bottom:none;font:12px system-ui,sans-serif", j.textContent = B.name || `Page ${P + 1}`, j.onclick = () => {
        m = P, I();
      }, w.push(j), _.appendChild(j);
      const Y = d(B.child);
      Y.style.cssText += ";position:absolute;inset:0;min-width:0;min-height:0;border:1px solid #333;border-radius:0 6px 6px 6px;overflow:hidden", k.push(Y), E.appendChild(Y);
    }), I(), y.append(_, E), y;
  }
  function d(f) {
    if (Fr(f)) return h(f.card);
    if (Ye(f)) return b(f);
    if (!Rt(f)) {
      const E = i.createElement("div");
      return E.textContent = "bad layout node", E.style.color = "#f88", E;
    }
    const m = f.dir === "col", y = i.createElement("div");
    y.style.cssText = `display:flex;flex-direction:${m ? "column" : "row"};min-width:0;min-height:0;width:100%;height:100%`;
    const _ = so(f);
    return f.children.forEach((E, w) => {
      const k = d(E);
      k.style.flex = `${_[w]} 1 0`, k.style.minWidth = "0", k.style.minHeight = "0", y.appendChild(k);
    }), y;
  }
  function v() {
    a = [], e.replaceChildren(), e.style.cssText += ";display:flex;min-width:0;min-height:0";
    const f = n && n.layout;
    if (!f) return;
    const m = d(f);
    m.style.flex = "1 1 0", m.style.minWidth = "0", m.style.minHeight = "0", e.appendChild(m), c();
  }
  const g = [
    t.onVars((f) => {
      l.run_id = f.run_id, l.vars = f.items;
      const m = {};
      for (const y of Object.keys(f.items || {})) {
        const _ = f.items[y];
        _ && _.gid != null && (m[_.gid] = _.src != null ? _.src : _.gid);
      }
      l.gidToCanon = m, c();
    }),
    t.onPreview((f) => {
      const m = l.gidToCanon && f.gid in l.gidToCanon ? l.gidToCanon[f.gid] : f.gid;
      l.images[m] = f.image || f.dataUrl, c();
    }),
    t.onEvent((f) => {
      f.name === "run_finished" && f.data && typeof f.data.ms == "number" ? l.run_ms = f.data.ms : f.name === "run_result" && f.data ? (l.result = f.data, c()) : (f.name === "safe_state" || f.name === "status") && (l.status = f.data, c());
    })
  ], p = setInterval(() => {
    t.cmd("dispatch_stats").then((f) => {
      f && Array.isArray(f.groups) && (l.groups = f.groups, c());
    }).catch(() => {
    });
  }, r);
  return v(), {
    setDashboard(f) {
      n = f, v();
    },
    state: l,
    destroy() {
      g.forEach((f) => f()), clearInterval(p), e.replaceChildren();
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
  gr as TOOLS,
  Mo as XI_COMPONENTS,
  ho as XiClient,
  bo as addSibling,
  ko as addTab,
  ws as bytesToBase64,
  po as collectStatusItems,
  mo as countLeaves,
  ys as decodePreviewFrame,
  mr as eachLeaf,
  wn as emptyCard,
  ao as getNode,
  Ga as inferDescriptor,
  Fr as isLeaf,
  Rt as isSplit,
  Ye as isTabs,
  ii as makeTool,
  So as mountDashboard,
  go as mountMonitor,
  vo as mountPanel,
  bs as parseVars,
  co as protocol,
  uo as registerTool,
  yo as removePane,
  $o as removeTab,
  To as renameTab,
  Co as setActive,
  wo as setCard,
  xo as setWeights,
  _o as splitLeaf,
  si as validate,
  so as weightsOf,
  Eo as wrapInTabs
};
