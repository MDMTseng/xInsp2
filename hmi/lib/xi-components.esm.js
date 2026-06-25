var Ts = Object.defineProperty;
var qr = (e) => {
  throw TypeError(e);
};
var Ss = (e, t, n) => t in e ? Ts(e, t, { enumerable: !0, configurable: !0, writable: !0, value: n }) : e[t] = n;
var F = (e, t, n) => Ss(e, typeof t != "symbol" ? t + "" : t, n), Gn = (e, t, n) => t.has(e) || qr("Cannot " + n);
var u = (e, t, n) => (Gn(e, t, "read from private field"), n ? n.call(e) : t.get(e)), $ = (e, t, n) => t.has(e) ? qr("Cannot add the same private member more than once") : t instanceof WeakSet ? t.add(e) : t.set(e, n), E = (e, t, n, r) => (Gn(e, t, "write to private field"), r ? r.call(e, n) : t.set(e, n), n), M = (e, t, n) => (Gn(e, t, "access private method"), n);
var li;
typeof window < "u" && ((li = window.__svelte ?? (window.__svelte = {})).v ?? (li.v = /* @__PURE__ */ new Set())).add("5");
const Cs = 1, Ms = 2, fi = 4, As = 8, Ns = 16, Os = 1, Rs = 4, Is = 8, Ps = 16, Ds = 2, ui = "[", mr = "[!", Vr = "[?", _r = "]", Vt = {}, B = Symbol("uninitialized"), Ls = "http://www.w3.org/1999/xhtml", ci = !1;
var br = Array.isArray, js = Array.prototype.indexOf, Mn = Array.prototype.includes, Wn = Array.from, An = Object.keys, Nn = Object.defineProperty, xt = Object.getOwnPropertyDescriptor, Hs = Object.getOwnPropertyDescriptors, Fs = Object.prototype, Ws = Array.prototype, di = Object.getPrototypeOf, Br = Object.isExtensible;
const Ys = () => {
};
function zs(e) {
  for (var t = 0; t < e.length; t++)
    e[t]();
}
function hi() {
  var e, t, n = new Promise((r, i) => {
    e = r, t = i;
  });
  return { promise: n, resolve: e, reject: t };
}
const Z = 2, Bt = 4, Yn = 8, vi = 1 << 24, Ne = 16, Re = 32, et = 64, er = 128, xe = 512, U = 1024, X = 2048, We = 4096, re = 8192, ve = 16384, Mt = 32768, tr = 1 << 25, Ut = 65536, On = 1 << 17, qs = 1 << 18, At = 1 << 19, Vs = 1 << 20, je = 1 << 25, St = 65536, Rn = 1 << 21, Lt = 1 << 22, ft = 1 << 23, Et = Symbol("$state"), pi = Symbol("legacy props"), Bs = Symbol(""), En = Symbol("attributes"), Us = Symbol("class"), Xs = Symbol("style"), Zt = Symbol("text"), gi = Symbol("form reset"), zn = new class extends Error {
  constructor() {
    super(...arguments);
    F(this, "name", "StaleReactionError");
    F(this, "message", "The reaction that called `getAbortSignal()` was re-run or destroyed");
  }
}();
var ai;
const mi = (
  // We gotta write it like this because after downleveling the pure comment may end up in the wrong location
  !!((ai = globalThis.document) != null && ai.contentType) && /* @__PURE__ */ globalThis.document.contentType.includes("xml")
), yr = 3, gn = 8;
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
let L;
function pe(e) {
  if (e === null)
    throw qn(), Vt;
  return L = e;
}
function Vn() {
  return pe(/* @__PURE__ */ it(L));
}
function z(e) {
  if (D) {
    if (/* @__PURE__ */ it(L) !== null)
      throw qn(), Vt;
    L = e;
  }
}
function ul(e = 1) {
  if (D) {
    for (var t = e, n = L; t--; )
      n = /** @type {TemplateNode} */
      /* @__PURE__ */ it(n);
    L = n;
  }
}
function In(e = !0) {
  for (var t = 0, n = L; ; ) {
    if (n.nodeType === gn) {
      var r = (
        /** @type {Comment} */
        n.data
      );
      if (r === _r) {
        if (t === 0) return n;
        t -= 1;
      } else (r === ui || r === mr || // "[1", "[2", etc. for if blocks
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
  if (!e || e.nodeType !== gn)
    throw qn(), Vt;
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
let dl = !1, ie = null;
function Xt(e) {
  ie = e;
}
function nt(e, t = !1, n) {
  ie = {
    p: ie,
    i: !1,
    c: null,
    e: null,
    s: e,
    x: null,
    r: (
      /** @type {Effect} */
      T
    ),
    l: null
  };
}
function rt(e) {
  var t = (
    /** @type {ComponentContext} */
    ie
  ), n = t.e;
  if (n !== null) {
    t.e = null;
    for (var r of n)
      Xi(r);
  }
  return e !== void 0 && (t.x = e), t.i = !0, ie = t.p, e ?? /** @type {T} */
  {};
}
function wi() {
  return !0;
}
let ht = [];
function xi() {
  var e = ht;
  ht = [], zs(e);
}
function Ze(e) {
  if (ht.length === 0 && !sn) {
    var t = ht;
    queueMicrotask(() => {
      t === ht && xi();
    });
  }
  ht.push(e);
}
function hl() {
  for (; ht.length > 0; )
    xi();
}
function Ei(e) {
  var t = T;
  if (t === null)
    return C.f |= ft, e;
  if ((t.f & Mt) === 0 && (t.f & Bt) === 0)
    throw e;
  ot(e, t);
}
function ot(e, t) {
  if (!(t !== null && (t.f & ve) !== 0)) {
    for (; t !== null; ) {
      if ((t.f & er) !== 0) {
        if ((t.f & Mt) === 0)
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
function Y(e, t) {
  e.f = e.f & vl | t;
}
function wr(e) {
  (e.f & xe) !== 0 || e.deps === null ? Y(e, U) : Y(e, We);
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
  (e.f & X) !== 0 ? t.add(e) : (e.f & We) !== 0 && n.add(e), ki(e.deps), Y(e, U);
}
let wn = !1;
function pl(e) {
  var t = wn;
  try {
    return wn = !1, [e(), wn];
  } finally {
    wn = t;
  }
}
function gl(e) {
  let t = 0, n = Ct(0), r;
  return () => {
    Sr() && (A(n), Ar(() => (t === 0 && (r = Ir(() => e(() => ln(n)))), t += 1, () => {
      Ze(() => {
        t -= 1, t === 0 && (r == null || r(), r = void 0, ln(n));
      });
    })));
  };
}
var ml = Ut | At;
function _l(e, t, n, r) {
  new bl(e, t, n, r);
}
var ce, un, _e, mt, ae, be, ne, de, Be, _t, lt, jt, cn, dn, Ue, jn, H, Ti, Si, Ci, nr, kn, $n, rr, ir;
class bl {
  /**
   * @param {TemplateNode} node
   * @param {BoundaryProps} props
   * @param {((anchor: Node) => void)} children
   * @param {((error: unknown) => unknown) | undefined} [transform_error]
   */
  constructor(t, n, r, i) {
    $(this, H);
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
    $(this, ce);
    /** @type {TemplateNode | null} */
    $(this, un, D ? L : null);
    /** @type {BoundaryProps} */
    $(this, _e);
    /** @type {((anchor: Node) => void)} */
    $(this, mt);
    /** @type {Effect} */
    $(this, ae);
    /** @type {Effect | null} */
    $(this, be, null);
    /** @type {Effect | null} */
    $(this, ne, null);
    /** @type {Effect | null} */
    $(this, de, null);
    /** @type {DocumentFragment | null} */
    $(this, Be, null);
    $(this, _t, 0);
    $(this, lt, 0);
    $(this, jt, !1);
    /** @type {Set<Effect>} */
    $(this, cn, /* @__PURE__ */ new Set());
    /** @type {Set<Effect>} */
    $(this, dn, /* @__PURE__ */ new Set());
    /**
     * A source containing the number of pending async deriveds/expressions.
     * Only created if `$effect.pending()` is used inside the boundary,
     * otherwise updating the source results in needless `Batch.ensure()`
     * calls followed by no-op flushes
     * @type {Source<number> | null}
     */
    $(this, Ue, null);
    $(this, jn, gl(() => (E(this, Ue, Ct(u(this, _t))), () => {
      E(this, Ue, null);
    })));
    var s;
    E(this, ce, t), E(this, _e, n), E(this, mt, (l) => {
      var a = (
        /** @type {Effect} */
        T
      );
      a.b = this, a.f |= er, r(l);
    }), this.parent = /** @type {Effect} */
    T.b, this.transform_error = i ?? ((s = this.parent) == null ? void 0 : s.transform_error) ?? ((l) => l), E(this, ae, Nr(() => {
      if (D) {
        const l = (
          /** @type {Comment} */
          u(this, un)
        );
        Vn();
        const a = l.data === mr;
        if (l.data.startsWith(Vr)) {
          const c = JSON.parse(l.data.slice(Vr.length));
          M(this, H, Si).call(this, c);
        } else a ? M(this, H, Ci).call(this) : M(this, H, Ti).call(this);
      } else
        M(this, H, nr).call(this);
    }, ml)), D && E(this, ce, L);
  }
  /**
   * Defer an effect inside a pending boundary until the boundary resolves
   * @param {Effect} effect
   */
  defer_effect(t) {
    $i(t, u(this, cn), u(this, dn));
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
    M(this, H, rr).call(this, t, n), E(this, _t, u(this, _t) + t), !(!u(this, Ue) || u(this, jt)) && (E(this, jt, !0), Ze(() => {
      E(this, jt, !1), u(this, Ue) && Gt(u(this, Ue), u(this, _t));
    }));
  }
  get_effect_pending() {
    return u(this, jn).call(this), A(
      /** @type {Source<number>} */
      u(this, Ue)
    );
  }
  /** @param {unknown} error */
  error(t) {
    if (!u(this, _e).onerror && !u(this, _e).failed)
      throw t;
    k != null && k.is_fork ? (u(this, be) && k.skip_effect(u(this, be)), u(this, ne) && k.skip_effect(u(this, ne)), u(this, de) && k.skip_effect(u(this, de)), k.oncommit(() => {
      M(this, H, ir).call(this, t);
    })) : M(this, H, ir).call(this, t);
  }
}
ce = new WeakMap(), un = new WeakMap(), _e = new WeakMap(), mt = new WeakMap(), ae = new WeakMap(), be = new WeakMap(), ne = new WeakMap(), de = new WeakMap(), Be = new WeakMap(), _t = new WeakMap(), lt = new WeakMap(), jt = new WeakMap(), cn = new WeakMap(), dn = new WeakMap(), Ue = new WeakMap(), jn = new WeakMap(), H = new WeakSet(), Ti = function() {
  try {
    E(this, be, we(() => u(this, mt).call(this, u(this, ce))));
  } catch (t) {
    this.error(t);
  }
}, /**
 * @param {unknown} error The deserialized error from the server's hydration comment
 */
Si = function(t) {
  const n = u(this, _e).failed;
  n && E(this, de, we(() => {
    n(
      u(this, ce),
      () => t,
      () => () => {
      }
    );
  }));
}, Ci = function() {
  const t = u(this, _e).pending;
  t && (this.is_pending = !0, E(this, ne, we(() => t(u(this, ce)))), Ze(() => {
    var n = E(this, Be, document.createDocumentFragment()), r = He();
    n.append(r), E(this, be, M(this, H, $n).call(this, () => we(() => u(this, mt).call(this, r)))), u(this, lt) === 0 && (u(this, ce).before(n), E(this, Be, null), $t(
      /** @type {Effect} */
      u(this, ne),
      () => {
        E(this, ne, null);
      }
    ), M(this, H, kn).call(
      this,
      /** @type {Batch} */
      k
    ));
  }));
}, nr = function() {
  try {
    if (this.is_pending = this.has_pending_snippet(), E(this, lt, 0), E(this, _t, 0), E(this, be, we(() => {
      u(this, mt).call(this, u(this, ce));
    })), u(this, lt) > 0) {
      var t = E(this, Be, document.createDocumentFragment());
      Rr(u(this, be), t);
      const n = (
        /** @type {(anchor: Node) => void} */
        u(this, _e).pending
      );
      E(this, ne, we(() => n(u(this, ce))));
    } else
      M(this, H, kn).call(
        this,
        /** @type {Batch} */
        k
      );
  } catch (n) {
    this.error(n);
  }
}, /**
 * @param {Batch} batch
 */
kn = function(t) {
  this.is_pending = !1, t.transfer_effects(u(this, cn), u(this, dn));
}, /**
 * @template T
 * @param {() => T} fn
 */
$n = function(t) {
  var n = T, r = C, i = ie;
  Ye(u(this, ae)), ke(u(this, ae)), Xt(u(this, ae).ctx);
  try {
    return ut.ensure(), t();
  } catch (s) {
    return Ei(s), null;
  } finally {
    Ye(n), ke(r), Xt(i);
  }
}, /**
 * Updates the pending count associated with the currently visible pending snippet,
 * if any, such that we can replace the snippet with content once work is done
 * @param {1 | -1} d
 * @param {Batch} batch
 */
rr = function(t, n) {
  var r;
  if (!this.has_pending_snippet()) {
    this.parent && M(r = this.parent, H, rr).call(r, t, n);
    return;
  }
  E(this, lt, u(this, lt) + t), u(this, lt) === 0 && (M(this, H, kn).call(this, n), u(this, ne) && $t(u(this, ne), () => {
    E(this, ne, null);
  }), u(this, Be) && (u(this, ce).before(u(this, Be)), E(this, Be, null)));
}, /**
 * @param {unknown} error
 */
ir = function(t) {
  u(this, be) && (se(u(this, be)), E(this, be, null)), u(this, ne) && (se(u(this, ne)), E(this, ne, null)), u(this, de) && (se(u(this, de)), E(this, de, null)), D && (pe(
    /** @type {TemplateNode} */
    u(this, un)
  ), ul(), pe(In()));
  var n = u(this, _e).onerror;
  let r = u(this, _e).failed;
  var i = !1, s = !1;
  const l = () => {
    if (i) {
      fl();
      return;
    }
    i = !0, s && ll(), u(this, de) !== null && $t(u(this, de), () => {
      E(this, de, null);
    }), M(this, H, $n).call(this, () => {
      M(this, H, nr).call(this);
    });
  }, a = (o) => {
    try {
      s = !0, n == null || n(o, l), s = !1;
    } catch (c) {
      ot(c, u(this, ae) && u(this, ae).parent);
    }
    r && E(this, de, M(this, H, $n).call(this, () => {
      try {
        return we(() => {
          var c = (
            /** @type {Effect} */
            T
          );
          c.b = this, c.f |= er, r(
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
  const i = an;
  var s = e.filter((p) => !p.settled), l = t.map(i);
  if (n.length === 0 && s.length === 0) {
    r(l);
    return;
  }
  var a = (
    /** @type {Effect} */
    T
  ), o = wl(), c = s.length === 1 ? s[0].promise : s.length > 1 ? Promise.all(s.map((p) => p.promise)) : null;
  function v(p) {
    if ((a.f & ve) === 0) {
      o();
      try {
        r([...l, ...p]);
      } catch (m) {
        ot(m, a);
      }
      Pn();
    }
  }
  var b = Mi();
  if (n.length === 0) {
    c.then(() => v([])).finally(b);
    return;
  }
  function d() {
    Promise.all(n.map((p) => /* @__PURE__ */ xl(p))).then(v).catch((p) => ot(p, a)).finally(b);
  }
  c ? c.then(() => {
    o(), d(), Pn();
  }) : d();
}
function wl() {
  var e = (
    /** @type {Effect} */
    T
  ), t = C, n = ie, r = (
    /** @type {Batch} */
    k
  );
  return function(s = !0) {
    Ye(e), ke(t), Xt(n), s && (e.f & ve) === 0 && (r == null || r.activate(), r == null || r.apply());
  };
}
function Pn(e = !0) {
  Ye(null), ke(null), Xt(null), e && (k == null || k.deactivate());
}
function Mi() {
  var e = (
    /** @type {Effect} */
    T
  ), t = e.b, n = (
    /** @type {Batch} */
    k
  ), r = !!(t != null && t.is_rendered());
  return t == null || t.update_pending_count(1, n), n.increment(r, e), () => {
    t == null || t.update_pending_count(-1, n), n.decrement(r, e);
  };
}
// @__NO_SIDE_EFFECTS__
function an(e) {
  var t = Z | X;
  return T !== null && (T.f |= At), {
    ctx: ie,
    deps: null,
    effects: null,
    equals: bi,
    f: t,
    fn: e,
    reactions: null,
    rv: 0,
    v: (
      /** @type {V} */
      B
    ),
    wv: 0,
    parent: T,
    ac: null
  };
}
const Qt = Symbol("obsolete");
// @__NO_SIDE_EFFECTS__
function xl(e, t, n) {
  let r = (
    /** @type {Effect | null} */
    T
  );
  r === null && Gs();
  var i = (
    /** @type {Promise<V>} */
    /** @type {unknown} */
    void 0
  ), s = Ct(
    /** @type {V} */
    B
  ), l = !C, a = /* @__PURE__ */ new Set();
  return Pl(() => {
    var p, m;
    var o = (
      /** @type {Effect} */
      T
    ), c = hi();
    i = c.promise;
    try {
      Promise.resolve(e()).then(c.resolve, (g) => {
        g !== zn && c.reject(g);
      }).finally(Pn);
    } catch (g) {
      c.reject(g), Pn();
    }
    var v = (
      /** @type {Batch} */
      k
    );
    if (l) {
      if ((o.f & Mt) !== 0)
        var b = Mi();
      if (
        // boundary can be null if the async derived is inside an $effect.root not connected to the component render tree
        (p = r.b) != null && p.is_rendered()
      )
        (m = v.async_deriveds.get(o)) == null || m.reject(Qt);
      else
        for (const g of a.values())
          g.reject(Qt);
      a.add(c), v.async_deriveds.set(o, c);
    }
    const d = (g, f = void 0) => {
      b == null || b(), a.delete(c), f !== Qt && (v.activate(), f ? (s.f |= ft, Gt(s, f)) : ((s.f & ft) !== 0 && (s.f ^= ft), Gt(s, g)), v.deactivate());
    };
    c.promise.then(d, (g) => d(null, g || "unknown"));
  }), Cr(() => {
    for (const o of a)
      o.reject(Qt);
  }), new Promise((o) => {
    function c(v) {
      function b() {
        v === i ? o(s) : c(i);
      }
      v.then(b, b);
    }
    c(i);
  });
}
// @__NO_SIDE_EFFECTS__
function Ai(e) {
  const t = /* @__PURE__ */ an(e);
  return es(t), t;
}
// @__NO_SIDE_EFFECTS__
function Ni(e) {
  const t = /* @__PURE__ */ an(e);
  return t.equals = yi, t;
}
function El(e) {
  var t = e.effects;
  if (t !== null) {
    e.effects = null;
    for (var n = 0; n < t.length; n += 1)
      se(
        /** @type {Effect} */
        t[n]
      );
  }
}
function xr(e) {
  var t, n = T, r = e.parent;
  if (!tt && r !== null && e.v !== B && // if it was never evaluated before, it's guaranteed to fail downstream, so we try to execute instead
  (r.f & (ve | re)) !== 0)
    return al(), e.v;
  Ye(r);
  try {
    e.f &= ~St, El(e), t = is(e);
  } finally {
    Ye(n);
  }
  return t;
}
function Oi(e) {
  var t = xr(e);
  if (!e.equals(t) && (e.wv = ns(), (!(k != null && k.is_fork) || e.deps === null) && (k !== null ? (k.capture(e, t, !0), rn == null || rn.capture(e, t, !0)) : e.v = t, e.deps === null))) {
    Y(e, U);
    return;
  }
  tt || (J !== null ? (Sr() || k != null && k.is_fork) && J.set(e, t) : wr(e));
}
function kl(e) {
  var t, n;
  if (e.effects !== null)
    for (const r of e.effects)
      (r.teardown || r.ac) && ((t = r.teardown) == null || t.call(r), (n = r.ac) == null || n.abort(zn), r.fn !== null && (r.teardown = Ys), r.ac = null, fn(r, 0), Or(r));
}
function Ri(e) {
  if (e.effects !== null)
    for (const t of e.effects)
      t.teardown && t.fn !== null && Kt(t);
}
let Kn = null, It = null, k = null, rn = null, J = null, sr = null, sn = !1, Jn = !1, Dt = null, Tn = null;
var Ur = 0;
let $l = 1;
var Ht, at, bt, Ft, Wt, Yt, Xe, zt, oe, hn, Ge, Ce, De, qt, yt, P, lr, en, ar, Ii, Pi, Pt, Tl, tn;
const Hn = class Hn {
  constructor() {
    $(this, P);
    F(this, "id", $l++);
    /** True as soon as `#process` was called */
    $(this, Ht, !1);
    F(this, "linked", !0);
    /** @type {Batch | null} */
    $(this, at, null);
    /** @type {Batch | null} */
    $(this, bt, null);
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
    $(this, Ft, /* @__PURE__ */ new Set());
    /**
     * If a fork is discarded, we need to destroy any effects that are no longer needed
     * @type {Set<(batch: Batch) => void>}
     */
    $(this, Wt, /* @__PURE__ */ new Set());
    /**
     * The number of async effects that are currently in flight
     */
    $(this, Yt, 0);
    /**
     * Async effects that are currently in flight, _not_ inside a pending boundary
     * @type {Map<Effect, number>}
     */
    $(this, Xe, /* @__PURE__ */ new Map());
    /**
     * A deferred that resolves when the batch is committed, used with `settled()`
     * TODO replace with Promise.withResolvers once supported widely enough
     * @type {{ promise: Promise<void>, resolve: (value?: any) => void, reject: (reason: unknown) => void } | null}
     */
    $(this, zt, null);
    /**
     * The root effects that need to be flushed
     * @type {Effect[]}
     */
    $(this, oe, []);
    /**
     * Effects created while this batch was active.
     * @type {Effect[]}
     */
    $(this, hn, []);
    /**
     * Deferred effects (which run after async work has completed) that are DIRTY
     * @type {Set<Effect>}
     */
    $(this, Ge, /* @__PURE__ */ new Set());
    /**
     * Deferred effects that are MAYBE_DIRTY
     * @type {Set<Effect>}
     */
    $(this, Ce, /* @__PURE__ */ new Set());
    /**
     * A map of branches that still exist, but will be destroyed when this batch
     * is committed — we skip over these during `process`.
     * The value contains child effects that were dirty/maybe_dirty before being reset,
     * so they can be rescheduled if the branch survives.
     * @type {Map<Effect, { d: Effect[], m: Effect[] }>}
     */
    $(this, De, /* @__PURE__ */ new Map());
    /**
     * Inverse of #skipped_branches which we need to tell prior batches to unskip them when committing
     * @type {Set<Effect>}
     */
    $(this, qt, /* @__PURE__ */ new Set());
    F(this, "is_fork", !1);
    $(this, yt, !1);
    It === null ? Kn = It = this : (E(It, bt, this), E(this, at, It)), It = this;
  }
  /**
   * Add an effect to the #skipped_branches map and reset its children
   * @param {Effect} effect
   */
  skip_effect(t) {
    u(this, De).has(t) || u(this, De).set(t, { d: [], m: [] }), u(this, qt).delete(t);
  }
  /**
   * Remove an effect from the #skipped_branches map and reschedule
   * any tracked dirty/maybe_dirty child effects
   * @param {Effect} effect
   * @param {(e: Effect) => void} callback
   */
  unskip_effect(t, n = (r) => this.schedule(r)) {
    var r = u(this, De).get(t);
    if (r) {
      u(this, De).delete(t);
      for (var i of r.d)
        Y(i, X), n(i);
      for (i of r.m)
        Y(i, We), n(i);
    }
    u(this, qt).add(t);
  }
  /**
   * Associate a change to a given source with the current
   * batch, noting its previous and current values
   * @param {Value} source
   * @param {any} value
   * @param {boolean} [is_derived]
   */
  capture(t, n, r = !1) {
    t.v !== B && !this.previous.has(t) && this.previous.set(t, t.v), (t.f & ft) === 0 && (this.current.set(t, [n, r]), J == null || J.set(t, n)), this.is_fork || (t.v = n);
  }
  activate() {
    k = this;
  }
  deactivate() {
    k = null, J = null;
  }
  flush() {
    try {
      Jn = !0, k = this, M(this, P, en).call(this);
    } finally {
      Ur = 0, sr = null, Dt = null, Tn = null, Jn = !1, k = null, J = null, kt.clear();
    }
  }
  discard() {
    var t;
    for (const n of u(this, Wt)) n(this);
    u(this, Wt).clear();
    for (const n of this.async_deriveds.values())
      n.reject(Qt);
    M(this, P, tn).call(this), (t = u(this, zt)) == null || t.resolve();
  }
  /**
   * @param {Effect} effect
   */
  register_created_effect(t) {
    u(this, hn).push(t);
  }
  /**
   * @param {boolean} blocking
   * @param {Effect} effect
   */
  increment(t, n) {
    if (E(this, Yt, u(this, Yt) + 1), t) {
      let r = u(this, Xe).get(n) ?? 0;
      u(this, Xe).set(n, r + 1);
    }
  }
  /**
   * @param {boolean} blocking
   * @param {Effect} effect
   */
  decrement(t, n) {
    if (E(this, Yt, u(this, Yt) - 1), t) {
      let r = u(this, Xe).get(n) ?? 0;
      r === 1 ? u(this, Xe).delete(n) : u(this, Xe).set(n, r - 1);
    }
    u(this, yt) || (E(this, yt, !0), Ze(() => {
      E(this, yt, !1), this.linked && this.flush();
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
    u(this, Ft).add(t);
  }
  /** @param {(batch: Batch) => void} fn */
  ondiscard(t) {
    u(this, Wt).add(t);
  }
  settled() {
    return (u(this, zt) ?? E(this, zt, hi())).promise;
  }
  static ensure() {
    if (k === null) {
      const t = k = new Hn();
      !Jn && !sn && Ze(() => {
        u(t, Ht) || t.flush();
      });
    }
    return k;
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
    if (sr = t, (i = t.b) != null && i.is_pending && (t.f & (Bt | Yn | vi)) !== 0 && (t.f & Mt) === 0) {
      t.b.defer_effect(t);
      return;
    }
    for (var n = t; n.parent !== null; ) {
      n = n.parent;
      var r = n.f;
      if (Dt !== null && n === T && (C === null || (C.f & Z) === 0))
        return;
      if ((r & (et | Re)) !== 0) {
        if ((r & U) === 0)
          return;
        n.f ^= U;
      }
    }
    u(this, oe).push(n);
  }
};
Ht = new WeakMap(), at = new WeakMap(), bt = new WeakMap(), Ft = new WeakMap(), Wt = new WeakMap(), Yt = new WeakMap(), Xe = new WeakMap(), zt = new WeakMap(), oe = new WeakMap(), hn = new WeakMap(), Ge = new WeakMap(), Ce = new WeakMap(), De = new WeakMap(), qt = new WeakMap(), yt = new WeakMap(), P = new WeakSet(), lr = function() {
  if (this.is_fork) return !0;
  for (const r of u(this, Xe).keys()) {
    for (var t = r, n = !1; t.parent !== null; ) {
      if (u(this, De).has(t)) {
        n = !0;
        break;
      }
      t = t.parent;
    }
    if (!n)
      return !0;
  }
  return !1;
}, en = function() {
  var o, c, v, b;
  E(this, Ht, !0), Ur++ > 1e3 && (M(this, P, tn).call(this), Sl());
  for (const d of u(this, Ge))
    u(this, Ce).delete(d), Y(d, X), this.schedule(d);
  for (const d of u(this, Ce))
    Y(d, We), this.schedule(d);
  const t = u(this, oe);
  E(this, oe, []), this.apply();
  var n = Dt = [], r = [], i = Tn = [];
  for (const d of t)
    try {
      M(this, P, ar).call(this, d, n, r);
    } catch (p) {
      throw ji(d), M(this, P, lr).call(this) || this.discard(), p;
    }
  if (k = null, i.length > 0) {
    var s = Hn.ensure();
    for (const d of i)
      s.schedule(d);
  }
  if (Dt = null, Tn = null, M(this, P, lr).call(this)) {
    M(this, P, Pt).call(this, r), M(this, P, Pt).call(this, n);
    for (const [d, p] of u(this, De))
      Li(d, p);
    i.length > 0 && /** @type {unknown} */
    M(o = k, P, en).call(o);
    return;
  }
  const l = M(this, P, Ii).call(this);
  if (l) {
    M(this, P, Pt).call(this, r), M(this, P, Pt).call(this, n), M(c = l, P, Pi).call(c, this);
    return;
  }
  u(this, Ge).clear(), u(this, Ce).clear();
  for (const d of u(this, Ft)) d(this);
  u(this, Ft).clear(), rn = this, Xr(r), Xr(n), rn = null, (v = u(this, zt)) == null || v.resolve();
  var a = (
    /** @type {Batch | null} */
    /** @type {unknown} */
    k
  );
  if (u(this, Yt) === 0 && (u(this, oe).length === 0 || a !== null) && M(this, P, tn).call(this), u(this, oe).length > 0)
    if (a !== null) {
      const d = a;
      u(d, oe).push(...u(this, oe).filter((p) => !u(d, oe).includes(p)));
    } else
      a = this;
  a !== null && M(b = a, P, en).call(b);
}, /**
 * Traverse the effect tree, executing effects or stashing
 * them for later execution as appropriate
 * @param {Effect} root
 * @param {Effect[]} effects
 * @param {Effect[]} render_effects
 */
ar = function(t, n, r) {
  t.f ^= U;
  for (var i = t.first; i !== null; ) {
    var s = i.f, l = (s & (Re | et)) !== 0, a = l && (s & U) !== 0, o = a || (s & re) !== 0 || u(this, De).has(i);
    if (!o && i.fn !== null) {
      l ? i.f ^= U : (s & Bt) !== 0 ? n.push(i) : mn(i) && ((s & Ne) !== 0 && u(this, Ce).add(i), Kt(i));
      var c = i.first;
      if (c !== null) {
        i = c;
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
}, Ii = function() {
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
Pi = function(t) {
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
          l & (Lt | Ne) && !this.async_deriveds.has(a) && (u(this, Ce).delete(a), Y(a, X), this.schedule(a));
        }
      }
  };
  for (const i of this.current.keys())
    n(i);
  this.oncommit(() => t.discard()), M(r = t, P, tn).call(r), k = this, M(this, P, en).call(this);
}, /**
 * @param {Effect[]} effects
 */
Pt = function(t) {
  for (var n = 0; n < t.length; n += 1)
    $i(t[n], u(this, Ge), u(this, Ce));
}, Tl = function() {
  var b;
  for (let d = Kn; d !== null; d = u(d, bt)) {
    var t = d.id < this.id, n = [];
    for (const [p, [m, g]] of this.current) {
      if (d.current.has(p)) {
        var r = (
          /** @type {[any, boolean]} */
          d.current.get(p)[0]
        );
        if (t && m !== r)
          d.current.set(p, [m, g]);
        else
          continue;
      }
      n.push(p);
    }
    if (t)
      for (const [p, m] of this.async_deriveds) {
        const g = d.async_deriveds.get(p);
        g && m.promise.then(g.resolve).catch(g.reject);
      }
    var i = [...d.current.keys()].filter(
      (p) => !/** @type {[any, boolean]} */
      d.current.get(p)[1]
    );
    if (!(!u(d, Ht) || i.length === 0)) {
      var s = i.filter((p) => !this.current.has(p));
      if (s.length === 0)
        t && d.discard();
      else if (n.length > 0) {
        if (t)
          for (const p of u(this, qt))
            d.unskip_effect(p, (m) => {
              var g;
              (m.f & (Ne | Lt)) !== 0 ? d.schedule(m) : M(g = d, P, Pt).call(g, [m]);
            });
        d.activate();
        var l = /* @__PURE__ */ new Set(), a = /* @__PURE__ */ new Map();
        for (var o of n)
          Di(o, s, l, a);
        a = /* @__PURE__ */ new Map();
        var c = [...d.current].filter(([p, m]) => {
          const g = this.current.get(p);
          return g ? g[0] !== m[0] || g[1] !== m[1] : !0;
        }).map(([p]) => p);
        if (c.length > 0)
          for (const p of u(this, hn))
            (p.f & (ve | re | On)) === 0 && Er(p, c, a) && ((p.f & (Lt | Ne)) !== 0 ? (Y(p, X), d.schedule(p)) : u(d, Ge).add(p));
        if (u(d, oe).length > 0 && !u(d, yt)) {
          d.apply();
          for (var v of u(d, oe))
            M(b = d, P, ar).call(b, v, [], []);
          E(d, oe, []);
        }
        d.deactivate();
      }
    }
  }
}, tn = function() {
  if (this.linked) {
    var t = u(this, at), n = u(this, bt);
    t === null ? Kn = n : E(t, bt, n), n === null ? It = t : E(n, at, t), this.linked = !1;
  }
};
let ut = Hn;
function R(e) {
  var t = sn;
  sn = !0;
  try {
    for (var n; ; ) {
      if (hl(), k === null)
        return (
          /** @type {T} */
          n
        );
      k.flush();
    }
  } finally {
    sn = t;
  }
}
function Sl() {
  try {
    el();
  } catch (e) {
    ot(e, sr);
  }
}
let Se = null;
function Xr(e) {
  var t = e.length;
  if (t !== 0) {
    for (var n = 0; n < t; ) {
      var r = e[n++];
      if ((r.f & (ve | re)) === 0 && mn(r) && (Se = /* @__PURE__ */ new Set(), Kt(r), r.deps === null && r.first === null && r.nodes === null && r.teardown === null && r.ac === null && Ji(r), (Se == null ? void 0 : Se.size) > 0)) {
        kt.clear();
        for (const i of Se) {
          if ((i.f & (ve | re)) !== 0) continue;
          const s = [i];
          let l = i.parent;
          for (; l !== null; )
            Se.has(l) && (Se.delete(l), s.push(l)), l = l.parent;
          for (let a = s.length - 1; a >= 0; a--) {
            const o = s[a];
            (o.f & (ve | re)) === 0 && Kt(o);
          }
        }
        Se.clear();
      }
    }
    Se = null;
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
      ) : (s & (Lt | Ne)) !== 0 && (s & X) === 0 && Er(i, t, r) && (Y(i, X), kr(
        /** @type {Effect} */
        i
      ));
    }
}
function Er(e, t, n) {
  const r = n.get(e);
  if (r !== void 0) return r;
  if (e.deps !== null)
    for (const i of e.deps) {
      if (Mn.call(t, i))
        return !0;
      if ((i.f & Z) !== 0 && Er(
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
function kr(e) {
  k.schedule(e);
}
function Li(e, t) {
  if (!((e.f & Re) !== 0 && (e.f & U) !== 0)) {
    (e.f & X) !== 0 ? t.d.push(e) : (e.f & We) !== 0 && t.m.push(e), Y(e, U);
    for (var n = e.first; n !== null; )
      Li(n, t), n = n.next;
  }
}
function ji(e) {
  Y(e, U);
  for (var t = e.first; t !== null; )
    ji(t), t = t.next;
}
let Dn = /* @__PURE__ */ new Set();
const kt = /* @__PURE__ */ new Map();
let Hi = !1;
function Ct(e, t) {
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
function Pe(e, t) {
  const n = Ct(e);
  return es(n), n;
}
// @__NO_SIDE_EFFECTS__
function Fi(e, t = !1, n = !0) {
  const r = Ct(e);
  return t || (r.equals = yi), r;
}
function Ae(e, t, n = !1) {
  C !== null && // since we are untracking the function inside `$inspect.with` we need to add this check
  // to ensure we error if state is set inside an inspect effect
  (!Oe || (C.f & On) !== 0) && wi() && (C.f & (Z | Ne | Lt | On)) !== 0 && (Fe === null || !Fe.has(e)) && sl();
  let r = n ? vt(t) : t;
  return Gt(e, r, Tn);
}
function Gt(e, t, n = null) {
  if (!e.equals(t)) {
    kt.set(e, tt ? t : e.v);
    var r = ut.ensure();
    if (r.capture(e, t), (e.f & Z) !== 0) {
      const i = (
        /** @type {Derived} */
        e
      );
      (e.f & X) !== 0 && xr(i), J === null && wr(i);
    }
    e.wv = ns(), Wi(e, X, n), T !== null && (T.f & U) !== 0 && (T.f & (Re | et)) === 0 && (me === null ? jl([e]) : me.push(e)), !r.is_fork && Dn.size > 0 && !Hi && Cl();
  }
  return t;
}
function Cl() {
  Hi = !1;
  for (const e of Dn) {
    (e.f & U) !== 0 && Y(e, We);
    let t;
    try {
      t = mn(e);
    } catch {
      t = !0;
    }
    t && Kt(e);
  }
  Dn.clear();
}
function ln(e) {
  Ae(e, e.v + 1);
}
function Wi(e, t, n) {
  var r = e.reactions;
  if (r !== null)
    for (var i = r.length, s = 0; s < i; s++) {
      var l = r[s], a = l.f, o = (a & X) === 0;
      if (o && Y(l, t), (a & On) !== 0)
        Dn.add(
          /** @type {Effect} */
          l
        );
      else if ((a & Z) !== 0) {
        var c = (
          /** @type {Derived} */
          l
        );
        J == null || J.delete(c), (a & St) === 0 && (a & xe && (T === null || (T.f & Rn) === 0) && (l.f |= St), Wi(c, We, n));
      } else if (o) {
        var v = (
          /** @type {Effect} */
          l
        );
        (a & Ne) !== 0 && Se !== null && Se.add(v), n !== null ? n.push(v) : kr(v);
      }
    }
}
function vt(e) {
  if (typeof e != "object" || e === null || Et in e)
    return e;
  const t = di(e);
  if (t !== Fs && t !== Ws)
    return e;
  var n = /* @__PURE__ */ new Map(), r = br(e), i = /* @__PURE__ */ Pe(0), s = Tt, l = (a) => {
    if (Tt === s)
      return a();
    var o = C, c = Tt;
    ke(null), Qr(s);
    var v = a();
    return ke(o), Qr(c), v;
  };
  return r && n.set("length", /* @__PURE__ */ Pe(
    /** @type {any[]} */
    e.length
  )), new Proxy(
    /** @type {any} */
    e,
    {
      defineProperty(a, o, c) {
        (!("value" in c) || c.configurable === !1 || c.enumerable === !1 || c.writable === !1) && rl();
        var v = n.get(o);
        return v === void 0 ? l(() => {
          var b = /* @__PURE__ */ Pe(c.value);
          return n.set(o, b), b;
        }) : Ae(v, c.value, !0), !0;
      },
      deleteProperty(a, o) {
        var c = n.get(o);
        if (c === void 0) {
          if (o in a) {
            const v = l(() => /* @__PURE__ */ Pe(B));
            n.set(o, v), ln(i);
          }
        } else
          Ae(c, B), ln(i);
        return !0;
      },
      get(a, o, c) {
        var p;
        if (o === Et)
          return e;
        var v = n.get(o), b = o in a;
        if (v === void 0 && (!b || (p = xt(a, o)) != null && p.writable) && (v = l(() => {
          var m = vt(b ? a[o] : B), g = /* @__PURE__ */ Pe(m);
          return g;
        }), n.set(o, v)), v !== void 0) {
          var d = A(v);
          return d === B ? void 0 : d;
        }
        return Reflect.get(a, o, c);
      },
      getOwnPropertyDescriptor(a, o) {
        var c = Reflect.getOwnPropertyDescriptor(a, o);
        if (c && "value" in c) {
          var v = n.get(o);
          v && (c.value = A(v));
        } else if (c === void 0) {
          var b = n.get(o), d = b == null ? void 0 : b.v;
          if (b !== void 0 && d !== B)
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
        if (o === Et)
          return !0;
        var c = n.get(o), v = c !== void 0 && c.v !== B || Reflect.has(a, o);
        if (c !== void 0 || T !== null && (!v || (d = xt(a, o)) != null && d.writable)) {
          c === void 0 && (c = l(() => {
            var p = v ? vt(a[o]) : B, m = /* @__PURE__ */ Pe(p);
            return m;
          }), n.set(o, c));
          var b = A(c);
          if (b === B)
            return !1;
        }
        return v;
      },
      set(a, o, c, v) {
        var h;
        var b = n.get(o), d = o in a;
        if (r && o === "length")
          for (var p = c; p < /** @type {Source<number>} */
          b.v; p += 1) {
            var m = n.get(p + "");
            m !== void 0 ? Ae(m, B) : p in a && (m = l(() => /* @__PURE__ */ Pe(B)), n.set(p + "", m));
          }
        if (b === void 0)
          (!d || (h = xt(a, o)) != null && h.writable) && (b = l(() => /* @__PURE__ */ Pe(void 0)), Ae(b, vt(c)), n.set(o, b));
        else {
          d = b.v !== B;
          var g = l(() => vt(c));
          Ae(b, g);
        }
        var f = Reflect.getOwnPropertyDescriptor(a, o);
        if (f != null && f.set && f.set.call(v, c), !d) {
          if (r && typeof o == "string") {
            var _ = (
              /** @type {Source<number>} */
              n.get("length")
            ), y = Number(o);
            Number.isInteger(y) && y >= _.v && Ae(_, y + 1);
          }
          ln(i);
        }
        return !0;
      },
      ownKeys(a) {
        A(i);
        var o = Reflect.ownKeys(a).filter((b) => {
          var d = n.get(b);
          return d === void 0 || d.v !== B;
        });
        for (var [c, v] of n)
          v.v !== B && !(c in a) && o.push(c);
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
    if (e !== null && typeof e == "object" && Et in e)
      return e[Et];
  } catch {
  }
  return e;
}
function Ml(e, t) {
  return Object.is(Gr(e), Gr(t));
}
var Kr, Yi, zi, qi;
function or() {
  if (Kr === void 0) {
    Kr = window, Yi = /Firefox/.test(navigator.userAgent);
    var e = Element.prototype, t = Node.prototype, n = Text.prototype;
    zi = xt(t, "firstChild").get, qi = xt(t, "nextSibling").get, Br(e) && (e[Us] = void 0, e[En] = null, e[Xs] = void 0, e.__e = void 0), Br(n) && (n[Zt] = void 0);
  }
}
function He(e = "") {
  return document.createTextNode(e);
}
// @__NO_SIDE_EFFECTS__
function on(e) {
  return (
    /** @type {TemplateNode | null} */
    zi.call(e)
  );
}
// @__NO_SIDE_EFFECTS__
function it(e) {
  return (
    /** @type {TemplateNode | null} */
    qi.call(e)
  );
}
function V(e, t) {
  if (!D)
    return /* @__PURE__ */ on(e);
  var n = /* @__PURE__ */ on(L);
  if (n === null)
    n = L.appendChild(He());
  else if (t && n.nodeType !== yr) {
    var r = He();
    return n == null || n.before(r), pe(r), r;
  }
  return t && Ui(
    /** @type {Text} */
    n
  ), pe(n), n;
}
function Ee(e, t = 1, n = !1) {
  let r = D ? L : e;
  for (var i; t--; )
    i = r, r = /** @type {TemplateNode} */
    /* @__PURE__ */ it(r);
  if (!D)
    return r;
  if (n) {
    if ((r == null ? void 0 : r.nodeType) !== yr) {
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
function Vi(e) {
  e.textContent = "";
}
function Bi() {
  return !1;
}
function $r(e, t, n) {
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
  for (; t !== null && t.nodeType === yr; )
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
function Tr(e) {
  var t = C, n = T;
  ke(null), Ye(null);
  try {
    return e();
  } finally {
    ke(t), Ye(n);
  }
}
function Nl(e) {
  T === null && (C === null && Qs(), Zs()), tt && Js();
}
function Ol(e, t) {
  var n = t.last;
  n === null ? t.last = t.first = e : (n.next = e, e.prev = n, t.last = e);
}
function ze(e, t) {
  var n = T;
  n !== null && (n.f & re) !== 0 && (e |= re);
  var r = {
    ctx: ie,
    deps: null,
    nodes: null,
    f: e | X | xe,
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
  k == null || k.register_created_effect(r);
  var i = r;
  if ((e & Bt) !== 0)
    Dt !== null ? Dt.push(r) : ut.ensure().schedule(r);
  else if (t !== null) {
    try {
      Kt(r);
    } catch (l) {
      throw se(r), l;
    }
    i.deps === null && i.teardown === null && i.nodes === null && i.first === i.last && // either `null`, or a singular child
    (i.f & At) === 0 && (i = i.first, (e & Ne) !== 0 && (e & Ut) !== 0 && i !== null && (i.f |= Ut));
  }
  if (i !== null && (i.parent = n, n !== null && Ol(i, n), C !== null && (C.f & Z) !== 0 && (e & et) === 0)) {
    var s = (
      /** @type {Derived} */
      C
    );
    (s.effects ?? (s.effects = [])).push(i);
  }
  return r;
}
function Sr() {
  return C !== null && !Oe;
}
function Cr(e) {
  const t = ze(Yn, null);
  return Y(t, U), t.teardown = e, t;
}
function Mr(e) {
  Nl();
  var t = (
    /** @type {Effect} */
    T.f
  ), n = !C && (t & Re) !== 0 && ie !== null && !ie.i;
  if (n) {
    var r = (
      /** @type {ComponentContext} */
      ie
    );
    (r.e ?? (r.e = [])).push(e);
  } else
    return Xi(e);
}
function Xi(e) {
  return ze(Bt | Vs, e);
}
function Rl(e) {
  ut.ensure();
  const t = ze(et | At, e);
  return () => {
    se(t);
  };
}
function Il(e) {
  ut.ensure();
  const t = ze(et | At, e);
  return (n = {}) => new Promise((r) => {
    n.outro ? $t(t, () => {
      se(t), r(void 0);
    }) : (se(t), r(void 0));
  });
}
function Gi(e) {
  return ze(Bt, e);
}
function Pl(e) {
  return ze(Lt | At, e);
}
function Ar(e, t = 0) {
  return ze(Yn | t, e);
}
function ge(e, t = [], n = [], r = []) {
  yl(r, t, n, (i) => {
    ze(Yn, () => {
      e(...i.map(A));
    });
  });
}
function Nr(e, t = 0) {
  var n = ze(Ne | t, e);
  return n;
}
function we(e) {
  return ze(Re | At, e);
}
function Ki(e) {
  var t = e.teardown;
  if (t !== null) {
    const n = tt, r = C;
    Zr(!0), ke(null);
    try {
      t.call(null);
    } finally {
      Zr(n), ke(r);
    }
  }
}
function Or(e, t = !1) {
  var n = e.first;
  for (e.first = e.last = null; n !== null; ) {
    const i = n.ac;
    i !== null && Tr(() => {
      i.abort(zn);
    });
    var r = n.next;
    (n.f & et) !== 0 ? n.parent = null : se(n, t), n = r;
  }
}
function Dl(e) {
  for (var t = e.first; t !== null; ) {
    var n = t.next;
    (t.f & Re) === 0 && se(t), t = n;
  }
}
function se(e, t = !0) {
  var n = !1;
  (t || (e.f & qs) !== 0) && e.nodes !== null && e.nodes.end !== null && (Ll(
    e.nodes.start,
    /** @type {TemplateNode} */
    e.nodes.end
  ), n = !0), e.f |= tr, Or(e, t && !n), fn(e, 0);
  var r = e.nodes && e.nodes.t;
  if (r !== null)
    for (const s of r)
      s.stop();
  Ki(e), e.f ^= tr, e.f |= ve;
  var i = e.parent;
  i !== null && i.first !== null && Ji(e), e.next = e.prev = e.teardown = e.ctx = e.deps = e.fn = e.nodes = e.ac = e.b = null;
}
function Ll(e, t) {
  for (; e !== null; ) {
    var n = e === t ? null : /* @__PURE__ */ it(e);
    e.remove(), e = n;
  }
}
function Ji(e) {
  var t = e.parent, n = e.prev, r = e.next;
  n !== null && (n.next = r), r !== null && (r.prev = n), t !== null && (t.first === e && (t.first = r), t.last === e && (t.last = n));
}
function $t(e, t, n = !0) {
  var r = [];
  Zi(e, r, !0);
  var i = () => {
    n && se(e), t && t();
  }, s = r.length;
  if (s > 0) {
    var l = () => --s || i();
    for (var a of r)
      a.out(l);
  } else
    i();
}
function Zi(e, t, n) {
  if ((e.f & re) === 0) {
    e.f ^= re;
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
        (i.f & Re) !== 0 && (e.f & Ne) !== 0;
        Zi(i, t, l ? n : !1);
      }
      i = s;
    }
  }
}
function Ln(e) {
  Qi(e, !0);
}
function Qi(e, t) {
  if ((e.f & re) !== 0) {
    e.f ^= re, (e.f & U) === 0 && (Y(e, X), ut.ensure().schedule(e));
    for (var n = e.first; n !== null; ) {
      var r = n.next, i = (n.f & Ut) !== 0 || (n.f & Re) !== 0;
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
let C = null, Oe = !1;
function ke(e) {
  C = e;
}
let T = null;
function Ye(e) {
  T = e;
}
let Fe = null;
function es(e) {
  C !== null && (Fe ?? (Fe = /* @__PURE__ */ new Set())).add(e);
}
let fe = null, ue = 0, me = null;
function jl(e) {
  me = e;
}
let ts = 1, pt = 0, Tt = pt;
function Qr(e) {
  Tt = e;
}
function ns() {
  return ++ts;
}
function mn(e) {
  var t = e.f;
  if ((t & X) !== 0)
    return !0;
  if (t & Z && (e.f &= ~St), (t & We) !== 0) {
    for (var n = (
      /** @type {Value[]} */
      e.deps
    ), r = n.length, i = 0; i < r; i++) {
      var s = n[i];
      if (mn(
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
    J === null && Y(e, U);
  }
  return !1;
}
function rs(e, t, n = !0) {
  var r = e.reactions;
  if (r !== null && !(Fe !== null && Fe.has(e)))
    for (var i = 0; i < r.length; i++) {
      var s = r[i];
      (s.f & Z) !== 0 ? rs(
        /** @type {Derived} */
        s,
        t,
        !1
      ) : t === s && (n ? Y(s, X) : (s.f & U) !== 0 && Y(s, We), kr(
        /** @type {Effect} */
        s
      ));
    }
}
function is(e) {
  var g;
  var t = fe, n = ue, r = me, i = C, s = Fe, l = ie, a = Oe, o = Tt, c = e.f;
  fe = /** @type {null | Value[]} */
  null, ue = 0, me = null, C = (c & (Re | et)) === 0 ? e : null, Fe = null, Xt(e.ctx), Oe = !1, Tt = ++pt, e.ac !== null && (Tr(() => {
    e.ac.abort(zn);
  }), e.ac = null);
  try {
    e.f |= Rn;
    var v = (
      /** @type {Function} */
      e.fn
    ), b = v();
    e.f |= Mt;
    var d = e.deps, p = k == null ? void 0 : k.is_fork;
    if (fe !== null) {
      var m;
      if (p || fn(e, ue), d !== null && ue > 0)
        for (d.length = ue + fe.length, m = 0; m < fe.length; m++)
          d[ue + m] = fe[m];
      else
        e.deps = d = fe;
      if (Sr() && (e.f & xe) !== 0)
        for (m = ue; m < d.length; m++)
          ((g = d[m]).reactions ?? (g.reactions = [])).push(e);
    } else !p && d !== null && ue < d.length && (fn(e, ue), d.length = ue);
    if (wi() && me !== null && !Oe && d !== null && (e.f & (Z | We | X)) === 0)
      for (m = 0; m < /** @type {Source[]} */
      me.length; m++)
        rs(
          me[m],
          /** @type {Effect} */
          e
        );
    if (i !== null && i !== e) {
      if (pt++, i.deps !== null)
        for (let f = 0; f < n; f += 1)
          i.deps[f].rv = pt;
      if (t !== null)
        for (const f of t)
          f.rv = pt;
      me !== null && (r === null ? r = me : r.push(.../** @type {Source[]} */
      me));
    }
    return (e.f & ft) !== 0 && (e.f ^= ft), b;
  } catch (f) {
    return Ei(f);
  } finally {
    e.f ^= Rn, fe = t, ue = n, me = r, C = i, Fe = s, Xt(l), Oe = a, Tt = o;
  }
}
function Hl(e, t) {
  let n = t.reactions;
  if (n !== null) {
    var r = js.call(n, e);
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
    (s.f & xe) !== 0 && (s.f ^= xe, s.f &= ~St), s.v !== B && wr(s), kl(s), fn(s, 0);
  }
}
function fn(e, t) {
  var n = e.deps;
  if (n !== null)
    for (var r = t; r < n.length; r++)
      Hl(e, n[r]);
}
function Kt(e) {
  var t = e.f;
  if ((t & ve) === 0) {
    Y(e, U);
    var n = T, r = Sn;
    T = e, Sn = !0;
    try {
      (t & (Ne | vi)) !== 0 ? Dl(e) : Or(e), Ki(e);
      var i = is(e);
      e.teardown = typeof i == "function" ? i : null, e.wv = ts;
      var s;
      ci && dl && (e.f & X) !== 0 && e.deps;
    } finally {
      Sn = r, T = n;
    }
  }
}
function A(e) {
  var t = e.f, n = (t & Z) !== 0;
  if (C !== null && !Oe) {
    var r = T !== null && (T.f & ve) !== 0;
    if (!r && (Fe === null || !Fe.has(e))) {
      var i = C.deps;
      if ((C.f & Rn) !== 0)
        e.rv < pt && (e.rv = pt, fe === null && i !== null && i[ue] === e ? ue++ : fe === null ? fe = [e] : fe.push(e));
      else {
        C.deps ?? (C.deps = []), Mn.call(C.deps, e) || C.deps.push(e);
        var s = e.reactions;
        s === null ? e.reactions = [C] : Mn.call(s, C) || s.push(C);
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
      return ((l.f & U) === 0 && l.reactions !== null || ls(l)) && (a = xr(l)), kt.set(l, a), a;
    }
    var o = (l.f & xe) === 0 && !Oe && C !== null && (Sn || (C.f & xe) !== 0), c = (l.f & Mt) === 0;
    mn(l) && (o && (l.f |= xe), Oi(l)), o && !c && (Ri(l), ss(l));
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
      (t.reactions ?? (t.reactions = [])).push(e), (t.f & Z) !== 0 && (t.f & xe) === 0 && (Ri(
        /** @type {Derived} */
        t
      ), ss(
        /** @type {Derived} */
        t
      ));
}
function ls(e) {
  if (e.v === B) return !0;
  if (e.deps === null) return !1;
  for (const t of e.deps)
    if (kt.has(t) || (t.f & Z) !== 0 && ls(
      /** @type {Derived} */
      t
    ))
      return !0;
  return !1;
}
function Ir(e) {
  var t = Oe;
  try {
    return Oe = !0, e();
  } finally {
    Oe = t;
  }
}
const gt = Symbol("events"), as = /* @__PURE__ */ new Set(), fr = /* @__PURE__ */ new Set();
function Fl(e, t, n, r = {}) {
  function i(s) {
    if (r.capture || ur.call(t, s), !s.cancelBubble)
      return Tr(() => n == null ? void 0 : n.call(this, s));
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
  t instanceof HTMLMediaElement) && Cr(() => {
    t.removeEventListener(e, l, s);
  });
}
function ee(e, t, n) {
  (t[gt] ?? (t[gt] = {}))[e] = n;
}
function Nt(e) {
  for (var t = 0; t < e.length; t++)
    as.add(e[t]);
  for (var n of fr)
    n(e);
}
let ei = null;
function ur(e) {
  var g, f;
  var t = this, n = (
    /** @type {Node} */
    t.ownerDocument
  ), r = e.type, i = ((g = e.composedPath) == null ? void 0 : g.call(e)) || [], s = (
    /** @type {null | Element} */
    i[0] || e.target
  );
  ei = e;
  var l = 0, a = ei === e && e[gt];
  if (a) {
    var o = i.indexOf(a);
    if (o !== -1 && (t === document || t === /** @type {any} */
    window)) {
      e[gt] = t;
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
    var v = C, b = T;
    ke(null), Ye(null);
    try {
      for (var d, p = []; s !== null && s !== t; ) {
        try {
          var m = (f = s[gt]) == null ? void 0 : f[r];
          m != null && (!/** @type {any} */
          s.disabled || // DOM could've been updated already by the time this is reached, so we check this as well
          // -> the target could not have been disabled because it emits the event in the first place
          e.target === s) && m.call(s, e);
        } catch (_) {
          d ? p.push(_) : d = _;
        }
        if (e.cancelBubble) break;
        l++, s = l < i.length ? (
          /** @type {Element} */
          i[l]
        ) : null;
      }
      if (d) {
        for (let _ of p)
          queueMicrotask(() => {
            throw _;
          });
        throw d;
      }
    } finally {
      e[gt] = t, delete e.currentTarget, ke(v), Ye(b);
    }
  }
}
var oi;
const Zn = (
  // We gotta write it like this because after downleveling the pure comment may end up in the wrong location
  ((oi = globalThis == null ? void 0 : globalThis.window) == null ? void 0 : oi.trustedTypes) && /* @__PURE__ */ globalThis.window.trustedTypes.createPolicy("svelte-trusted-html", {
    /** @param {string} html */
    createHTML: (e) => e
  })
);
function Wl(e) {
  return (
    /** @type {string} */
    (Zn == null ? void 0 : Zn.createHTML(e)) ?? e
  );
}
function Yl(e) {
  var t = $r("template");
  return t.innerHTML = Wl(e.replaceAll("<!>", "<!---->")), t.content;
}
function cr(e, t) {
  var n = (
    /** @type {Effect} */
    T
  );
  n.nodes === null && (n.nodes = { start: e, end: t, a: null, t: null });
}
// @__NO_SIDE_EFFECTS__
function le(e, t) {
  var n = (t & Ds) !== 0, r, i = !e.startsWith("<!>");
  return () => {
    if (D)
      return cr(L, null), L;
    r === void 0 && (r = Yl(i ? e : "<!>" + e), r = /** @type {TemplateNode} */
    /* @__PURE__ */ on(r));
    var s = (
      /** @type {TemplateNode} */
      n || Yi ? document.importNode(r, !0) : r.cloneNode(!0)
    );
    return cr(s, s), s;
  };
}
function te(e, t) {
  if (D) {
    var n = (
      /** @type {Effect & { nodes: EffectNodes }} */
      T
    );
    ((n.f & Mt) === 0 || n.nodes.end === null) && (n.nodes.end = L), Vn();
    return;
  }
  e !== null && e.before(
    /** @type {Node} */
    t
  );
}
const zl = ["touchstart", "touchmove"];
function ql(e) {
  return zl.includes(e);
}
function Ie(e, t) {
  var n = t == null ? "" : typeof t == "object" ? `${t}` : t;
  n !== /** @type {any} */
  (e[Zt] ?? (e[Zt] = e.nodeValue)) && (e[Zt] = n, e.nodeValue = `${n}`);
}
function fs(e, t) {
  return us(e, t);
}
function Vl(e, t) {
  or(), t.intro = t.intro ?? !1;
  const n = t.target, r = D, i = L;
  try {
    for (var s = /* @__PURE__ */ on(n); s && (s.nodeType !== gn || /** @type {Comment} */
    s.data !== ui); )
      s = /* @__PURE__ */ it(s);
    if (!s)
      throw Vt;
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
    return l !== Vt && console.warn("Failed to hydrate: ", l), t.recover === !1 && tl(), or(), Vi(n), Je(!1), fs(e, t);
  } finally {
    Je(r), pe(i);
  }
}
const xn = /* @__PURE__ */ new Map();
function us(e, { target: t, anchor: n, props: r = {}, events: i, context: s, intro: l = !0, transformError: a }) {
  or();
  var o = void 0, c = Il(() => {
    var v = n ?? t.appendChild(He());
    _l(
      /** @type {TemplateNode} */
      v,
      {
        pending: () => {
        }
      },
      (p) => {
        nt({});
        var m = (
          /** @type {ComponentContext} */
          ie
        );
        if (s && (m.c = s), i && (r.$$events = i), D && cr(
          /** @type {TemplateNode} */
          p,
          null
        ), o = e(p, r) || {}, D && (T.nodes.end = L, L === null || L.nodeType !== gn || /** @type {Comment} */
        L.data !== _r))
          throw qn(), Vt;
        rt();
      },
      a
    );
    var b = /* @__PURE__ */ new Set(), d = (p) => {
      for (var m = 0; m < p.length; m++) {
        var g = p[m];
        if (!b.has(g)) {
          b.add(g);
          var f = ql(g);
          for (const h of [t, document]) {
            var _ = xn.get(h);
            _ === void 0 && (_ = /* @__PURE__ */ new Map(), xn.set(h, _));
            var y = _.get(g);
            y === void 0 ? (h.addEventListener(g, ur, { passive: f }), _.set(g, 1)) : _.set(g, y + 1);
          }
        }
      }
    };
    return d(Wn(as)), fr.add(d), () => {
      var f;
      for (var p of b)
        for (const _ of [t, document]) {
          var m = (
            /** @type {Map<string, number>} */
            xn.get(_)
          ), g = (
            /** @type {number} */
            m.get(p)
          );
          --g == 0 ? (_.removeEventListener(p, ur), m.delete(p), m.size === 0 && xn.delete(_)) : m.set(p, g);
        }
      fr.delete(d), v !== n && ((f = v.parentNode) == null || f.removeChild(v));
    };
  });
  return dr.set(o, c), o;
}
let dr = /* @__PURE__ */ new WeakMap();
function Bl(e, t) {
  const n = dr.get(e);
  return n ? (dr.delete(e), n(t)) : Promise.resolve();
}
var Me, Le, he, wt, vn, pn, Fn;
class Ul {
  /**
   * @param {TemplateNode} anchor
   * @param {boolean} transition
   */
  constructor(t, n = !0) {
    /** @type {TemplateNode} */
    F(this, "anchor");
    /** @type {Map<Batch, Key>} */
    $(this, Me, /* @__PURE__ */ new Map());
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
    $(this, Le, /* @__PURE__ */ new Map());
    /**
     * Similar to #onscreen with respect to the keys, but contains branches that are not yet
     * in the DOM, because their insertion is deferred.
     * @type {Map<Key, Branch>}
     */
    $(this, he, /* @__PURE__ */ new Map());
    /**
     * Keys of effects that are currently outroing
     * @type {Set<Key>}
     */
    $(this, wt, /* @__PURE__ */ new Set());
    /**
     * Whether to pause (i.e. outro) on change, or destroy immediately.
     * This is necessary for `<svelte:element>`
     */
    $(this, vn, !0);
    /**
     * @param {Batch} batch
     */
    $(this, pn, (t) => {
      if (u(this, Me).has(t)) {
        var n = (
          /** @type {Key} */
          u(this, Me).get(t)
        ), r = u(this, Le).get(n);
        if (r)
          Ln(r), u(this, wt).delete(n);
        else {
          var i = u(this, he).get(n);
          i && (Ln(i.effect), u(this, Le).set(n, i.effect), u(this, he).delete(n), i.fragment.lastChild.remove(), this.anchor.before(i.fragment), r = i.effect);
        }
        for (const [s, l] of u(this, Me)) {
          if (u(this, Me).delete(s), s === t)
            break;
          const a = u(this, he).get(l);
          a && (se(a.effect), u(this, he).delete(l));
        }
        for (const [s, l] of u(this, Le)) {
          if (s === n || u(this, wt).has(s)) continue;
          const a = () => {
            if (Array.from(u(this, Me).values()).includes(s)) {
              var c = document.createDocumentFragment();
              Rr(l, c), c.append(He()), u(this, he).set(s, { effect: l, fragment: c });
            } else
              se(l);
            u(this, wt).delete(s), u(this, Le).delete(s);
          };
          u(this, vn) || !r ? (u(this, wt).add(s), $t(l, a, !1)) : a();
        }
      }
    });
    /**
     * @param {Batch} batch
     */
    $(this, Fn, (t) => {
      u(this, Me).delete(t);
      const n = Array.from(u(this, Me).values());
      for (const [r, i] of u(this, he))
        n.includes(r) || (se(i.effect), u(this, he).delete(r));
    });
    this.anchor = t, E(this, vn, n);
  }
  /**
   *
   * @param {any} key
   * @param {null | ((target: TemplateNode) => void)} fn
   */
  ensure(t, n) {
    var r = (
      /** @type {Batch} */
      k
    ), i = Bi();
    if (n && !u(this, Le).has(t) && !u(this, he).has(t))
      if (i) {
        var s = document.createDocumentFragment(), l = He();
        s.append(l), u(this, he).set(t, {
          effect: we(() => n(l)),
          fragment: s
        });
      } else
        u(this, Le).set(
          t,
          we(() => n(this.anchor))
        );
    if (u(this, Me).set(r, t), i) {
      for (const [a, o] of u(this, Le))
        a === t ? r.unskip_effect(o) : r.skip_effect(o);
      for (const [a, o] of u(this, he))
        a === t ? r.unskip_effect(o.effect) : r.skip_effect(o.effect);
      r.oncommit(u(this, pn)), r.ondiscard(u(this, Fn));
    } else
      D && (this.anchor = L), u(this, pn).call(this, r);
  }
}
Me = new WeakMap(), Le = new WeakMap(), he = new WeakMap(), wt = new WeakMap(), vn = new WeakMap(), pn = new WeakMap(), Fn = new WeakMap();
function _n(e, t, n = !1) {
  var r;
  D && (r = L, Vn());
  var i = new Ul(e), s = n ? Ut : 0;
  function l(a, o) {
    if (D) {
      var c = _i(
        /** @type {TemplateNode} */
        r
      );
      if (a !== parseInt(c.substring(1))) {
        var v = In();
        pe(v), i.anchor = v, Je(!1), i.ensure(a, o), Je(!0);
        return;
      }
    }
    i.ensure(a, o);
  }
  Nr(() => {
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
    $t(
      b,
      () => {
        if (s) {
          if (s.pending.delete(b), s.done.add(b), s.pending.size === 0) {
            var d = (
              /** @type {Set<EachOutroGroup>} */
              e.outrogroups
            );
            hr(e, Wn(s.done)), d.delete(s), d.size === 0 && (e.outrogroups = null);
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
      ), v = (
        /** @type {Element} */
        c.parentNode
      );
      Vi(v), v.append(c), e.items.clear();
    }
    hr(e, t, !o);
  } else
    s = {
      pending: new Set(t),
      done: /* @__PURE__ */ new Set()
    }, (e.outrogroups ?? (e.outrogroups = /* @__PURE__ */ new Set())).add(s);
}
function hr(e, t, n = !0) {
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
      s.f |= je;
      const l = document.createDocumentFragment();
      Rr(s, l);
    } else
      se(t[i], n);
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
    l = D ? pe(/* @__PURE__ */ on(c)) : c.appendChild(He());
  }
  D && Vn();
  var v = null, b = /* @__PURE__ */ Ni(() => {
    var h = n();
    return (
      /** @type {V[]} */
      br(h) ? h : h == null ? [] : Wn(h)
    );
  }), d, p = /* @__PURE__ */ new Map(), m = !0;
  function g(h) {
    (y.effect.f & ve) === 0 && (y.pending.delete(h), y.fallback = v, Gl(y, d, l, t, r), v !== null && (d.length === 0 ? (v.f & je) === 0 ? Ln(v) : (v.f ^= je, nn(v, null, l)) : $t(v, () => {
      v = null;
    })));
  }
  function f(h) {
    y.pending.delete(h);
  }
  var _ = Nr(() => {
    d = /** @type {V[]} */
    A(b);
    var h = d.length;
    let w = !1;
    if (D) {
      var S = _i(l) === mr;
      S !== (h === 0) && (l = In(), pe(l), Je(!1), w = !0);
    }
    for (var N = /* @__PURE__ */ new Set(), O = (
      /** @type {Batch} */
      k
    ), W = Bi(), j = 0; j < h; j += 1) {
      D && L.nodeType === gn && /** @type {Comment} */
      L.data === _r && (l = /** @type {Comment} */
      L, w = !0, Je(!1));
      var q = d[j], G = r(q, j), K = m ? null : a.get(G);
      K ? (K.v && Gt(K.v, q), K.i && Gt(K.i, j), W && O.unskip_effect(K.e)) : (K = Kl(
        a,
        m ? l : ti ?? (ti = He()),
        q,
        G,
        j,
        i,
        t,
        n
      ), m || (K.e.f |= je), a.set(G, K)), N.add(G);
    }
    if (h === 0 && s && !v && (m ? v = we(() => s(l)) : (v = we(() => s(ti ?? (ti = He()))), v.f |= je)), h > N.size && Ks(), D && h > 0 && pe(In()), !m)
      if (p.set(O, N), W) {
        for (const [Te, x] of a)
          N.has(Te) || O.skip_effect(x.e);
        O.oncommit(g), O.ondiscard(f);
      } else
        g(O);
    w && Je(!0), A(b);
  }), y = { effect: _, items: a, pending: p, outrogroups: null, fallback: v };
  m = !1, D && (l = L);
}
function Jt(e) {
  for (; e !== null && (e.f & Re) === 0; )
    e = e.next;
  return e;
}
function Gl(e, t, n, r, i) {
  var q, G, K, Te, x, Q, yn, Wr, Yr;
  var s = (r & As) !== 0, l = t.length, a = e.items, o = Jt(e.effect.first), c, v = null, b, d = [], p = [], m, g, f, _;
  if (s)
    for (_ = 0; _ < l; _ += 1)
      m = t[_], g = i(m, _), f = /** @type {EachItem} */
      a.get(g).e, (f.f & je) === 0 && ((G = (q = f.nodes) == null ? void 0 : q.a) == null || G.measure(), (b ?? (b = /* @__PURE__ */ new Set())).add(f));
  for (_ = 0; _ < l; _ += 1) {
    if (m = t[_], g = i(m, _), f = /** @type {EachItem} */
    a.get(g).e, e.outrogroups !== null)
      for (const Ve of e.outrogroups)
        Ve.pending.delete(f), Ve.done.delete(f);
    if ((f.f & re) !== 0 && (Ln(f), s && ((Te = (K = f.nodes) == null ? void 0 : K.a) == null || Te.unfix(), (b ?? (b = /* @__PURE__ */ new Set())).delete(f))), (f.f & je) !== 0)
      if (f.f ^= je, f === o)
        nn(f, null, n);
      else {
        var y = v ? v.next : o;
        f === e.effect.last && (e.effect.last = f.prev), f.prev && (f.prev.next = f.next), f.next && (f.next.prev = f.prev), st(e, v, f), st(e, f, y), nn(f, y, n), v = f, d = [], p = [], o = Jt(v.next);
        continue;
      }
    if (f !== o) {
      if (c !== void 0 && c.has(f)) {
        if (d.length < p.length) {
          var h = p[0], w;
          v = h.prev;
          var S = d[0], N = d[d.length - 1];
          for (w = 0; w < d.length; w += 1)
            nn(d[w], h, n);
          for (w = 0; w < p.length; w += 1)
            c.delete(p[w]);
          st(e, S.prev, N.next), st(e, v, S), st(e, N, h), o = h, v = N, _ -= 1, d = [], p = [];
        } else
          c.delete(f), nn(f, o, n), st(e, f.prev, f.next), st(e, f, v === null ? e.effect.first : v.next), st(e, v, f), v = f;
        continue;
      }
      for (d = [], p = []; o !== null && o !== f; )
        (c ?? (c = /* @__PURE__ */ new Set())).add(o), p.push(o), o = Jt(o.next);
      if (o === null)
        continue;
    }
    (f.f & je) === 0 && d.push(f), v = f, o = Jt(f.next);
  }
  if (e.outrogroups !== null) {
    for (const Ve of e.outrogroups)
      Ve.pending.size === 0 && (hr(e, Wn(Ve.done)), (x = e.outrogroups) == null || x.delete(Ve));
    e.outrogroups.size === 0 && (e.outrogroups = null);
  }
  if (o !== null || c !== void 0) {
    var O = [];
    if (c !== void 0)
      for (f of c)
        (f.f & re) === 0 && O.push(f);
    for (; o !== null; )
      (o.f & re) === 0 && o !== e.fallback && O.push(o), o = Jt(o.next);
    var W = O.length;
    if (W > 0) {
      var j = (r & fi) !== 0 && l === 0 ? n : null;
      if (s) {
        for (_ = 0; _ < W; _ += 1)
          (yn = (Q = O[_].nodes) == null ? void 0 : Q.a) == null || yn.measure();
        for (_ = 0; _ < W; _ += 1)
          (Yr = (Wr = O[_].nodes) == null ? void 0 : Wr.a) == null || Yr.fix();
      }
      Xl(e, O, j);
    }
  }
  s && Ze(() => {
    var Ve, zr;
    if (b !== void 0)
      for (f of b)
        (zr = (Ve = f.nodes) == null ? void 0 : Ve.a) == null || zr.apply();
  });
}
function Kl(e, t, n, r, i, s, l, a) {
  var o = (l & Cs) !== 0 ? (l & Ns) === 0 ? /* @__PURE__ */ Fi(n, !1, !1) : Ct(n) : null, c = (l & Ms) !== 0 ? Ct(i) : null;
  return {
    v: o,
    i: c,
    e: we(() => (s(t, o ?? n, c ?? i, a), () => {
      e.delete(r);
    }))
  };
}
function nn(e, t, n) {
  if (e.nodes)
    for (var r = e.nodes.start, i = e.nodes.end, s = t && (t.f & je) === 0 ? (
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
      const i = $r("style");
      i.id = t.hash, i.textContent = t.code, r.appendChild(i);
    }
  });
}
function hs(e, t, n = !1) {
  if (e.multiple) {
    if (t == null)
      return;
    if (!br(t))
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
  }), Cr(() => {
    t.disconnect();
  });
}
function ni(e) {
  return "__value" in e ? e.__value : e.value;
}
const Zl = Symbol("is custom element"), Ql = Symbol("is html"), ea = mi ? "link" : "LINK", ta = mi ? "progress" : "PROGRESS";
function Bn(e) {
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
  D && (i[t] = e.getAttribute(t), t === "src" || t === "srcset" || t === "href" && e.nodeName === ea) || i[t] !== (i[t] = n) && (t === "loading" && (e[Bs] = n), n == null ? e.removeAttribute(t) : typeof n != "string" && ra(e).includes(t) ? e[t] = n : e.setAttribute(t, n));
}
function Dr(e) {
  return (
    /** @type {Record<string | symbol, unknown>} **/
    /** @type {any} */
    e[En] ?? (e[En] = {
      [Zl]: e.nodeName.includes("-"),
      [Ql]: e.namespaceURI === Ls
    })
  );
}
var ri = /* @__PURE__ */ new Map();
function ra(e) {
  var t = e.getAttribute("is") || e.nodeName, n = ri.get(t);
  if (n) return n;
  ri.set(t, n = []);
  for (var r, i = e, s = Element.prototype; s !== i; ) {
    r = Hs(i);
    for (var l in r)
      r[l].set && // better safe than sorry, we don't want spread attributes to mess with HTML content
      l !== "innerHTML" && l !== "textContent" && l !== "innerText" && n.push(l);
    i = di(i);
  }
  return n;
}
function Qn(e, t) {
  return e === t || (e == null ? void 0 : e[Et]) === t;
}
function Lr(e = {}, t, n, r) {
  var i = (
    /** @type {ComponentContext} */
    ie.r
  ), s = (
    /** @type {Effect} */
    T
  );
  return Gi(() => {
    var l, a;
    return Ar(() => {
      l = a, a = [], Ir(() => {
        Qn(n(...a), e) || (t(e, ...a), l && Qn(n(...l), e) && t(null, ...l));
      });
    }), () => {
      let o = s;
      for (; o !== i && o.parent !== null && o.parent.f & tr; )
        o = o.parent;
      const c = () => {
        a && Qn(n(...a), e) && t(null, ...a);
      }, v = o.teardown;
      o.teardown = () => {
        c(), v == null || v();
      };
    };
  }), e;
}
function I(e, t, n, r) {
  var w;
  var i = !0, s = (n & Is) !== 0, l = (n & Ps) !== 0, a = (
    /** @type {V} */
    r
  ), o = !0, c = (
    /** @type {Derived<V> | undefined} */
    void 0
  ), v = () => l && i ? (c ?? (c = /* @__PURE__ */ an(
    /** @type {() => V} */
    r
  )), A(c)) : (o && (o = !1, a = l ? Ir(
    /** @type {() => V} */
    r
  ) : (
    /** @type {V} */
    r
  )), a);
  let b;
  if (s) {
    var d = Et in e || pi in e;
    b = ((w = xt(e, t)) == null ? void 0 : w.set) ?? (d && t in e ? (S) => e[t] = S : void 0);
  }
  var p, m = !1;
  s ? [p, m] = pl(() => (
    /** @type {V} */
    e[t]
  )) : p = /** @type {V} */
  e[t], p === void 0 && r !== void 0 && (p = v(), b && (nl(), b(p)));
  var g;
  if (g = () => {
    var S = (
      /** @type {V} */
      e[t]
    );
    return S === void 0 ? v() : (o = !0, S);
  }, (n & Rs) === 0)
    return g;
  if (b) {
    var f = e.$$legacy;
    return (
      /** @type {() => V} */
      (function(S, N) {
        return arguments.length > 0 ? ((!N || f || m) && b(N ? g() : S), S) : g();
      })
    );
  }
  var _ = !1, y = ((n & Os) !== 0 ? an : Ni)(() => (_ = !1, g()));
  s && A(y);
  var h = (
    /** @type {Effect} */
    T
  );
  return (
    /** @type {() => V} */
    (function(S, N) {
      if (arguments.length > 0) {
        const O = N ? A(y) : s ? vt(S) : S;
        return Ae(y, O), _ = !0, a !== void 0 && (a = O), S;
      }
      return tt && _ || (h.f & ve) !== 0 ? y.v : A(y);
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
    $(this, Ke);
    /** @type {Record<string, any>} */
    $(this, ye);
    var s;
    var n = /* @__PURE__ */ new Map(), r = (l, a) => {
      var o = /* @__PURE__ */ Fi(a, !1, !1);
      return n.set(l, o), o;
    };
    const i = new Proxy(
      { ...t.props || {}, $$events: {} },
      {
        get(l, a) {
          return A(n.get(a) ?? r(a, Reflect.get(l, a)));
        },
        has(l, a) {
          return a === pi ? !0 : (A(n.get(a) ?? r(a, Reflect.get(l, a))), Reflect.has(l, a));
        },
        set(l, a, o) {
          return Ae(n.get(a) ?? r(a, o), o), Reflect.set(l, a, o);
        }
      }
    );
    E(this, ye, (t.hydrate ? Vl : fs)(t.component, {
      target: t.target,
      anchor: t.anchor,
      props: i,
      context: t.context,
      intro: t.intro ?? !1,
      recover: t.recover,
      transformError: t.transformError
    })), (!((s = t == null ? void 0 : t.props) != null && s.$$host) || t.sync === !1) && R(), E(this, Ke, i.$$events);
    for (const l of Object.keys(u(this, ye)))
      l === "$set" || l === "$destroy" || l === "$on" || Nn(this, l, {
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
      Bl(u(this, ye));
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
          i !== "default" && (l.name = i), te(s, l);
        };
      };
      if (await Promise.resolve(), !this.$$cn || this.$$c)
        return;
      const n = {}, r = la(this);
      for (const i of this.$$s)
        i in r && (i === "default" && !this.$$d.children ? (this.$$d.children = t(i), n.default = !0) : n[i] = t(i));
      for (const i of this.attributes) {
        const s = this.$$g_p(i.name);
        s in this.$$d || (this.$$d[s] = Cn(s, i.value, this.$$p_d, "toProp"));
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
      }), this.$$me = Rl(() => {
        Ar(() => {
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
        var b;
        o = Cn(a, o, t), this.$$d[a] = o;
        var c = this.$$c;
        if (c) {
          var v = (b = xt(c, a)) == null ? void 0 : b.get;
          v ? c[a] = o : c.$set({ [a]: o });
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
var aa = /* @__PURE__ */ le('<span class="lbl"> </span>'), oa = /* @__PURE__ */ le('<label class="xi-slider svelte-8xo3l7"><!> <input type="range" class="svelte-8xo3l7"/> <span class="val svelte-8xo3l7"> </span></label>');
const fa = {
  hash: "svelte-8xo3l7",
  code: '.xi-slider.svelte-8xo3l7 {display:inline-flex;align-items:center;gap:0.5rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}input[type="range"].svelte-8xo3l7 {accent-color:var(--xi-accent, #3b82f6);vertical-align:middle;}.val.svelte-8xo3l7 {min-width:2.5em;text-align:right;font-variant-numeric:tabular-nums;}'
};
function ua(e, t) {
  nt(t, !0), ct(e, fa);
  let n = I(t, "value", 15, 0), r = I(t, "min", 7, 0), i = I(t, "max", 7, 100), s = I(t, "step", 7, 1), l = I(t, "label", 7, ""), a = I(t, "disabled", 7, !1);
  const o = t.$$host, c = (h) => o.dispatchEvent(new CustomEvent(h, { detail: { value: n() }, bubbles: !0, composed: !0 }));
  function v(h) {
    n(Number(h.target.value)), c("input");
  }
  function b(h) {
    n(Number(h.target.value)), c("change");
  }
  var d = {
    get value() {
      return n();
    },
    set value(h = 0) {
      n(h), R();
    },
    get min() {
      return r();
    },
    set min(h = 0) {
      r(h), R();
    },
    get max() {
      return i();
    },
    set max(h = 100) {
      i(h), R();
    },
    get step() {
      return s();
    },
    set step(h = 1) {
      s(h), R();
    },
    get label() {
      return l();
    },
    set label(h = "") {
      l(h), R();
    },
    get disabled() {
      return a();
    },
    set disabled(h = !1) {
      a(h), R();
    }
  }, p = oa(), m = V(p);
  {
    var g = (h) => {
      var w = aa(), S = V(w, !0);
      z(w), ge(() => Ie(S, l())), te(h, w);
    };
    _n(m, (h) => {
      l() && h(g);
    });
  }
  var f = Ee(m, 2);
  Bn(f);
  var _ = Ee(f, 2), y = V(_, !0);
  return z(_), z(p), ge(() => {
    Qe(f, "min", r()), Qe(f, "max", i()), Qe(f, "step", s()), Pr(f, n()), f.disabled = a(), Ie(y, n());
  }), ee("input", f, v), ee("change", f, b), te(e, p), rt(d);
}
Nt(["input", "change"]);
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
var ca = /* @__PURE__ */ le('<span class="lbl"> </span>'), da = /* @__PURE__ */ le('<label class="xi-number svelte-1f6ykwb"><!> <input type="number" class="svelte-1f6ykwb"/></label>');
const ha = {
  hash: "svelte-1f6ykwb",
  code: ".xi-number.svelte-1f6ykwb {display:inline-flex;align-items:center;gap:0.5rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}input.svelte-1f6ykwb {width:6em;padding:0.15rem 0.3rem;accent-color:var(--xi-accent, #3b82f6);}"
};
function va(e, t) {
  nt(t, !0), ct(e, ha);
  let n = I(t, "value", 15, 0), r = I(t, "min", 7), i = I(t, "max", 7), s = I(t, "step", 7, 1), l = I(t, "label", 7, ""), a = I(t, "disabled", 7, !1);
  const o = t.$$host, c = (y) => o.dispatchEvent(new CustomEvent(y, { detail: { value: n() }, bubbles: !0, composed: !0 })), v = (y) => y.target.value === "" ? null : Number(y.target.value);
  function b(y) {
    n(v(y)), c("input");
  }
  function d(y) {
    n(v(y)), c("change");
  }
  var p = {
    get value() {
      return n();
    },
    set value(y = 0) {
      n(y), R();
    },
    get min() {
      return r();
    },
    set min(y) {
      r(y), R();
    },
    get max() {
      return i();
    },
    set max(y) {
      i(y), R();
    },
    get step() {
      return s();
    },
    set step(y = 1) {
      s(y), R();
    },
    get label() {
      return l();
    },
    set label(y = "") {
      l(y), R();
    },
    get disabled() {
      return a();
    },
    set disabled(y = !1) {
      a(y), R();
    }
  }, m = da(), g = V(m);
  {
    var f = (y) => {
      var h = ca(), w = V(h, !0);
      z(h), ge(() => Ie(w, l())), te(y, h);
    };
    _n(g, (y) => {
      l() && y(f);
    });
  }
  var _ = Ee(g, 2);
  return Bn(_), z(m), ge(() => {
    Qe(_, "min", r()), Qe(_, "max", i()), Qe(_, "step", s()), Pr(_, n()), _.disabled = a();
  }), ee("input", _, b), ee("change", _, d), te(e, m), rt(p);
}
Nt(["input", "change"]);
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
var pa = /* @__PURE__ */ le('<span class="lbl"> </span>'), ga = /* @__PURE__ */ le('<label class="xi-toggle svelte-141rfru"><input type="checkbox" class="svelte-141rfru"/> <!></label>');
const ma = {
  hash: "svelte-141rfru",
  code: ".xi-toggle.svelte-141rfru {display:inline-flex;align-items:center;gap:0.4rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);cursor:pointer;}input.svelte-141rfru {accent-color:var(--xi-accent, #3b82f6);}"
};
function _a(e, t) {
  nt(t, !0), ct(e, ma);
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
      n(d), R();
    },
    get label() {
      return r();
    },
    set label(d = "") {
      r(d), R();
    },
    get disabled() {
      return i();
    },
    set disabled(d = !1) {
      i(d), R();
    }
  }, o = ga(), c = V(o);
  Bn(c);
  var v = Ee(c, 2);
  {
    var b = (d) => {
      var p = pa(), m = V(p, !0);
      z(p), ge(() => Ie(m, r())), te(d, p);
    };
    _n(v, (d) => {
      r() && d(b);
    });
  }
  return z(o), ge(() => {
    vs(c, n()), c.disabled = i();
  }), ee("change", c, l), te(e, o), rt(a);
}
Nt(["change"]);
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
var ba = /* @__PURE__ */ le('<span class="lbl"> </span>'), ya = /* @__PURE__ */ le('<label class="opt svelte-mtcxej"><input type="radio" class="svelte-mtcxej"/> <span> </span></label>'), wa = /* @__PURE__ */ le('<div class="xi-radio svelte-mtcxej" role="radiogroup"><!> <!></div>');
const xa = {
  hash: "svelte-mtcxej",
  code: ".xi-radio.svelte-mtcxej {display:inline-flex;align-items:center;gap:0.75rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}.opt.svelte-mtcxej {display:inline-flex;align-items:center;gap:0.25rem;cursor:pointer;}input.svelte-mtcxej {accent-color:var(--xi-accent, #3b82f6);}"
};
function Ea(e, t) {
  nt(t, !0), ct(e, xa);
  let n = I(t, "value", 15, ""), r = I(t, "options", 23, () => []), i = I(t, "label", 7, ""), s = I(t, "disabled", 7, !1), l = I(t, "name", 7, "xi-radio");
  const a = t.$$host, o = /* @__PURE__ */ Ai(() => gs(r()));
  function c(g) {
    n(g), a.dispatchEvent(new CustomEvent("change", { detail: { value: n() }, bubbles: !0, composed: !0 }));
  }
  var v = {
    get value() {
      return n();
    },
    set value(g = "") {
      n(g), R();
    },
    get options() {
      return r();
    },
    set options(g = []) {
      r(g), R();
    },
    get label() {
      return i();
    },
    set label(g = "") {
      i(g), R();
    },
    get disabled() {
      return s();
    },
    set disabled(g = !1) {
      s(g), R();
    },
    get name() {
      return l();
    },
    set name(g = "xi-radio") {
      l(g), R();
    }
  }, b = wa(), d = V(b);
  {
    var p = (g) => {
      var f = ba(), _ = V(f, !0);
      z(f), ge(() => Ie(_, i())), te(g, f);
    };
    _n(d, (g) => {
      i() && g(p);
    });
  }
  var m = Ee(d, 2);
  return ds(m, 17, () => A(o), cs, (g, f) => {
    var _ = ya(), y = V(_);
    Bn(y);
    var h = Ee(y, 2), w = V(h, !0);
    z(h), z(_), ge(() => {
      Qe(y, "name", l()), Pr(y, A(f).value), vs(y, A(f).value === n()), y.disabled = s(), Ie(w, A(f).label);
    }), ee("change", y, () => c(A(f).value)), te(g, _);
  }), z(b), te(e, b), rt(v);
}
Nt(["change"]);
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
var ka = /* @__PURE__ */ le('<span class="lbl"> </span>'), $a = /* @__PURE__ */ le("<option> </option>"), Ta = /* @__PURE__ */ le('<label class="xi-dropdown svelte-1wd9iqr"><!> <select class="svelte-1wd9iqr"></select></label>');
const Sa = {
  hash: "svelte-1wd9iqr",
  code: ".xi-dropdown.svelte-1wd9iqr {display:inline-flex;align-items:center;gap:0.5rem;font:13px system-ui, sans-serif;color:var(--xi-fg, inherit);}select.svelte-1wd9iqr {padding:0.15rem 0.3rem;}"
};
function Ca(e, t) {
  nt(t, !0), ct(e, Sa);
  let n = I(t, "value", 15, ""), r = I(t, "options", 23, () => []), i = I(t, "label", 7, ""), s = I(t, "disabled", 7, !1);
  const l = t.$$host, a = /* @__PURE__ */ Ai(() => gs(r()));
  function o(g) {
    n(g.target.value), l.dispatchEvent(new CustomEvent("change", { detail: { value: n() }, bubbles: !0, composed: !0 }));
  }
  var c = {
    get value() {
      return n();
    },
    set value(g = "") {
      n(g), R();
    },
    get options() {
      return r();
    },
    set options(g = []) {
      r(g), R();
    },
    get label() {
      return i();
    },
    set label(g = "") {
      i(g), R();
    },
    get disabled() {
      return s();
    },
    set disabled(g = !1) {
      s(g), R();
    }
  }, v = Ta(), b = V(v);
  {
    var d = (g) => {
      var f = ka(), _ = V(f, !0);
      z(f), ge(() => Ie(_, i())), te(g, f);
    };
    _n(b, (g) => {
      i() && g(d);
    });
  }
  var p = Ee(b, 2);
  ds(p, 21, () => A(a), cs, (g, f) => {
    var _ = $a(), y = V(_, !0);
    z(_);
    var h = {};
    ge(() => {
      na(_, A(f).value === n()), Ie(y, A(f).label), h !== (h = A(f).value) && (_.value = (_.__value = A(f).value) ?? "");
    }), te(g, _);
  }), z(p);
  var m;
  return Jl(p), z(v), ge(() => {
    p.disabled = s(), m !== (m = n()) && (p.value = (p.__value = n()) ?? "", hs(p, n()));
  }), ee("change", p, o), te(e, v), rt(c);
}
Nt(["change"]);
customElements.define("xi-dropdown", dt(
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
function Na(e, t) {
  nt(t, !0), ct(e, Aa);
  let n = I(t, "key", 7, ""), r = I(t, "label", 7, ""), i = I(t, "max", 7, 60);
  const s = t.$$host;
  let l, a = /* @__PURE__ */ Pe(null), o = /* @__PURE__ */ Pe(vt([]));
  function c() {
    if (!l) return;
    const h = l.getContext && l.getContext("2d");
    if (!h) return;
    const w = l.width = l.clientWidth || 120, S = l.height = l.clientHeight || 28;
    if (h.clearRect(0, 0, w, S), A(o).length < 2) return;
    const N = Math.min(...A(o)), O = Math.max(...A(o)), W = O - N || 1;
    h.beginPath(), A(o).forEach((j, q) => {
      const G = q / (A(o).length - 1) * (w - 2) + 1, K = S - 2 - (j - N) / W * (S - 4);
      q ? h.lineTo(G, K) : h.moveTo(G, K);
    }), h.strokeStyle = getComputedStyle(s).getPropertyValue("--xi-accent") || "#3b82f6", h.lineWidth = 1.5, h.stroke();
  }
  function v(h) {
    const w = h && h[n()];
    w && (Ae(a, w.value, !0), typeof w.value == "number" && Number.isFinite(w.value) && (Ae(o, [...A(o), w.value].slice(-i()), !0), c()), s.dispatchEvent(new CustomEvent("sample", { detail: { value: w.value }, bubbles: !0, composed: !0 })));
  }
  Mr(() => {
    s.update = v, Object.defineProperty(s, "latest", { get: () => A(a), configurable: !0 }), Object.defineProperty(s, "history", { get: () => A(o).slice(), configurable: !0 }), c();
  });
  const b = (h) => h == null ? "—" : typeof h == "number" ? Number.isInteger(h) ? h : h.toFixed(3) : String(h);
  var d = {
    get key() {
      return n();
    },
    set key(h = "") {
      n(h), R();
    },
    get label() {
      return r();
    },
    set label(h = "") {
      r(h), R();
    },
    get max() {
      return i();
    },
    set max(h = 60) {
      i(h), R();
    }
  }, p = Ma(), m = V(p), g = V(m, !0);
  z(m);
  var f = Ee(m, 2);
  Lr(f, (h) => l = h, () => l);
  var _ = Ee(f, 2), y = V(_, !0);
  return z(_), z(p), ge(
    (h) => {
      Ie(g, r() || n()), Ie(y, h);
    },
    [() => b(A(a))]
  ), te(e, p), rt(d);
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
const Ra = 0.05, Ia = 64, Pa = (e) => Math.max(Ra, Math.min(Ia, e));
function vr(e) {
  return !e.imgW || !e.imgH || !e.viewW || !e.viewH || (e.scale = Math.min(e.viewW / e.imgW, e.viewH / e.imgH) * 0.95, e.panX = (e.viewW - e.imgW * e.scale) / 2, e.panY = (e.viewH - e.imgH * e.scale) / 2), e;
}
function Da(e) {
  return e.scale = 1, e.panX = (e.viewW - e.imgW) / 2, e.panY = (e.viewH - e.imgH) / 2, e;
}
function _s(e, t, n, r) {
  const { x: i, y: s } = jr(e, t, n);
  return e.scale = Pa(e.scale * r), e.panX = t - i * e.scale, e.panY = n - s * e.scale, e;
}
function La(e, t, n) {
  return e.panX += t, e.panY += n, e;
}
var ja = /* @__PURE__ */ le('<canvas class="svelte-1yjweo0"></canvas>');
const Ha = {
  hash: "svelte-1yjweo0",
  code: ":host {display:block;width:100%;height:100%;}canvas.svelte-1yjweo0 {width:100%;height:100%;display:block;background:var(--xi-viewer-bg, #111);cursor:grab;touch-action:none;image-rendering:pixelated;}canvas.svelte-1yjweo0:active {cursor:grabbing;}"
};
function Fa(e, t) {
  nt(t, !0), ct(e, Ha);
  const n = t.$$host;
  let r;
  const i = ms();
  let s = null, l = null;
  function a() {
    if (!r) return;
    const h = r.getContext("2d");
    h.imageSmoothingEnabled = !1, h.clearRect(0, 0, r.width, r.height), s && (h.setTransform(i.scale, 0, 0, i.scale, i.panX, i.panY), h.drawImage(s, 0, 0), h.setTransform(1, 0, 0, 1, 0, 0));
  }
  function o() {
    if (!r) return;
    const h = r.getBoundingClientRect();
    r.width = Math.max(1, Math.round(h.width)), r.height = Math.max(1, Math.round(h.height)), i.viewW = r.width, i.viewH = r.height, a();
  }
  function c(h, w) {
    n.dispatchEvent(new CustomEvent(h, { detail: w, bubbles: !0, composed: !0 }));
  }
  function v(h) {
    const w = new Image();
    w.onload = () => {
      const S = !i.imgW;
      s = w, i.imgW = w.naturalWidth || w.width, i.imgH = w.naturalHeight || w.height, l = document.createElement("canvas"), l.width = i.imgW, l.height = i.imgH, l.getContext("2d").drawImage(w, 0, 0), S && vr(i), a();
    }, w.src = typeof h == "string" ? h : h.dataUrl;
  }
  function b(h) {
    if (!s) return;
    h.preventDefault();
    const w = r.getBoundingClientRect();
    _s(i, h.clientX - w.left, h.clientY - w.top, h.deltaY < 0 ? 1.15 : 1 / 1.15), a(), c("viewchange", { scale: i.scale });
  }
  let d = null, p = !1;
  function m(h) {
    var w;
    s && (d = { x: h.clientX, y: h.clientY }, p = !1, (w = r.setPointerCapture) == null || w.call(r, h.pointerId));
  }
  function g(h) {
    if (!d) return;
    const w = h.clientX - d.x, S = h.clientY - d.y;
    (w || S) && (p = !0), La(i, w, S), d = { x: h.clientX, y: h.clientY }, a();
  }
  function f(h) {
    d && !p && _(h), d = null;
  }
  function _(h) {
    if (!s || !l) return;
    const w = r.getBoundingClientRect(), S = jr(i, h.clientX - w.left, h.clientY - w.top), N = Math.floor(S.x), O = Math.floor(S.y);
    let W = null;
    if (N >= 0 && O >= 0 && N < i.imgW && O < i.imgH) {
      const j = l.getContext("2d").getImageData(N, O, 1, 1).data;
      W = [j[0], j[1], j[2]];
    }
    c("pixelpick", { x: N, y: O, rgb: W });
  }
  Mr(() => {
    n.setFrame = v, n.fit = () => {
      vr(i), a(), c("viewchange", { scale: i.scale });
    }, n.oneToOne = () => {
      Da(i), a(), c("viewchange", { scale: i.scale });
    }, o();
    const h = new ResizeObserver(o);
    return h.observe(r), () => h.disconnect();
  });
  var y = ja();
  Lr(y, (h) => r = h, () => r), os("wheel", y, b), ee("pointerdown", y, m), ee("pointermove", y, g), ee("pointerup", y, f), te(e, y), rt();
}
Nt(["pointerdown", "pointermove", "pointerup"]);
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
function Ya() {
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
function za() {
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
const pr = { point: Wa, rect: Ya, polygon: za };
function uo(e, t) {
  pr[e] = t;
}
function ii(e) {
  return pr[e] ? pr[e]() : null;
}
var qa = /* @__PURE__ */ le('<div class="xi-editor svelte-1qrk1oj"><div class="bar svelte-1qrk1oj"><span class="lbl"> </span> <span class="spacer svelte-1qrk1oj"></span> <button class="cancel svelte-1qrk1oj">Cancel</button> <button class="commit svelte-1qrk1oj">Commit</button></div> <canvas class="svelte-1qrk1oj"></canvas></div>');
const Va = {
  hash: "svelte-1qrk1oj",
  code: ":host {display:block;width:100%;height:100%;}.xi-editor.svelte-1qrk1oj {display:flex;flex-direction:column;height:100%;}.bar.svelte-1qrk1oj {display:flex;align-items:center;gap:0.5rem;padding:0.35rem 0.5rem;font:13px system-ui, sans-serif;background:var(--xi-bar-bg, #1f2937);color:#e5e7eb;}.spacer.svelte-1qrk1oj {flex:1;}button.svelte-1qrk1oj {padding:0.2rem 0.7rem;border:0;border-radius:4px;cursor:pointer;font:inherit;}.commit.svelte-1qrk1oj {background:var(--xi-accent, #6366f1);color:#fff;}.cancel.svelte-1qrk1oj {background:#374151;color:#e5e7eb;}canvas.svelte-1qrk1oj {flex:1;width:100%;display:block;background:var(--xi-viewer-bg, #111);cursor:crosshair;touch-action:none;image-rendering:pixelated;}"
};
function Ba(e, t) {
  nt(t, !0), ct(e, Va);
  let n = I(t, "tool", 7, "rect"), r = I(t, "label", 7, "");
  const i = t.$$host;
  let s;
  const l = ms();
  let a = null, o = ii(n());
  const c = (x) => Oa(l, x.x, x.y);
  function v() {
    if (!s) return;
    const x = s.getContext("2d");
    x && (x.imageSmoothingEnabled = !1, x.setTransform(1, 0, 0, 1, 0, 0), x.clearRect(0, 0, s.width, s.height), a && (x.setTransform(l.scale, 0, 0, l.scale, l.panX, l.panY), x.drawImage(a, 0, 0), x.setTransform(1, 0, 0, 1, 0, 0)), o && o.draw(x, c));
  }
  function b() {
    if (!s) return;
    const x = s.getBoundingClientRect();
    s.width = Math.max(1, Math.round(x.width)), s.height = Math.max(1, Math.round(x.height)), l.viewW = s.width, l.viewH = s.height, v();
  }
  function d(x) {
    const Q = new Image();
    Q.onload = () => {
      const yn = !l.imgW;
      a = Q, l.imgW = Q.naturalWidth || Q.width, l.imgH = Q.naturalHeight || Q.height, yn && vr(l), v();
    }, Q.src = typeof x == "string" ? x : x.dataUrl;
  }
  function p(x) {
    o = ii(x) || o, v();
  }
  const m = (x) => {
    const Q = s.getBoundingClientRect();
    return jr(l, x.clientX - Q.left, x.clientY - Q.top);
  };
  function g(x) {
    o && (o.onDown(m(x)), v());
  }
  function f(x) {
    o && x.buttons && (o.onMove(m(x)), v());
  }
  function _(x) {
    o && (o.onUp(m(x)), v());
  }
  function y(x) {
    o && (o.onDbl(m(x)), v());
  }
  function h(x) {
    if (!a) return;
    x.preventDefault();
    const Q = s.getBoundingClientRect();
    _s(l, x.clientX - Q.left, x.clientY - Q.top, x.deltaY < 0 ? 1.15 : 1 / 1.15), v();
  }
  function w() {
    !o || !o.done() || i.dispatchEvent(new CustomEvent("commit", {
      detail: { tool: o.type, result: o.result() },
      bubbles: !0,
      composed: !0
    }));
  }
  function S() {
    i.dispatchEvent(new CustomEvent("cancel", { detail: {}, bubbles: !0, composed: !0 }));
  }
  Mr(() => {
    i.setFrame = d, i.setTool = p, i.getResult = () => o && o.done() ? o.result() : null, b();
    const x = new ResizeObserver(b);
    return x.observe(s), () => x.disconnect();
  });
  var N = {
    get tool() {
      return n();
    },
    set tool(x = "rect") {
      n(x), R();
    },
    get label() {
      return r();
    },
    set label(x = "") {
      r(x), R();
    }
  }, O = qa(), W = V(O), j = V(W), q = V(j, !0);
  z(j);
  var G = Ee(j, 4), K = Ee(G, 2);
  z(W);
  var Te = Ee(W, 2);
  return Lr(Te, (x) => s = x, () => s), z(O), ge(() => Ie(q, r() || n())), ee("click", G, S), ee("click", K, w), ee("pointerdown", Te, g), ee("pointermove", Te, f), ee("pointerup", Te, _), ee("dblclick", Te, y), os("wheel", Te, h), te(e, O), rt(N);
}
Nt([
  "click",
  "pointerdown",
  "pointermove",
  "pointerup",
  "dblclick"
]);
customElements.define("xi-image-editor", dt(Ba, { tool: {}, label: {} }, [], [], { mode: "open" }));
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
        this._emit("preview", ys(n));
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
  for (const v of o) {
    if (i && !i(v)) continue;
    const b = s.createElement("section");
    if (b.className = "xi-section", b.dataset.tag = v.tag || "control", v.section) {
      const d = s.createElement("h3");
      d.className = "xi-section-title", d.textContent = v.section, b.appendChild(d);
    }
    for (const d of v.controls || []) {
      const p = Xa[d.type] || "xi-number", m = s.createElement(p);
      d.label && m.setAttribute("label", d.label);
      for (const f of ["min", "max", "step"]) d[f] != null && m.setAttribute(f, String(d[f]));
      const g = s.createElement("div");
      g.className = "xi-control", g.appendChild(m), b.appendChild(g), d.options != null && (m.options = d.options), d.key in a && (m.value = a[d.key]), m.addEventListener("change", async (f) => {
        a[d.key] = f.detail.value;
        try {
          await n.setInstanceDef(r, { ...a });
        } catch {
        }
        e.dispatchEvent(new CustomEvent("xi-change", { detail: { key: d.key, value: f.detail.value }, bubbles: !0 }));
      }), c.push({ el: m, key: d.key });
    }
    e.appendChild(b);
  }
  return {
    // Re-read the def from the backend and push values back into the controls.
    async refresh() {
      const v = await n.getInstanceDef(r) || {};
      Object.assign(a, v);
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
  for (const p of n) {
    const m = i.createElement("div");
    m.className = "xi-tile", m.dataset.key = p.key;
    const g = i.createElement("div");
    g.className = "xi-tile-label", g.textContent = p.label, m.appendChild(g);
    let f;
    p.type === "trace" ? (f = i.createElement("xi-trace"), f.setAttribute("key", p.key)) : p.type === "image" ? (f = i.createElement("xi-image-viewer"), f.style.height = "180px") : (f = i.createElement("div"), f.className = "xi-value", f.textContent = "—"), m.appendChild(f), s.appendChild(m), l.set(p.key, { type: p.type, el: f });
  }
  const c = (p) => {
    const m = p.items || {};
    a.clear(), o.clear();
    for (const [g, f] of l) {
      const _ = m[g];
      if (_)
        if (f.type === "trace") f.el.update(m);
        else if (f.type === "image") {
          if (_.gid != null) {
            const y = _.src != null ? _.src : _.gid;
            o.set(_.gid, y);
            let h = a.get(y);
            h || a.set(y, h = /* @__PURE__ */ new Set()), h.add(f.el);
          }
        } else f.el.textContent = Ka(_.value);
    }
  }, v = (p) => {
    const m = o.has(p.gid) ? o.get(p.gid) : p.gid, g = a.get(m);
    if (g) for (const f of g) f.setFrame(p.dataUrl);
  }, b = t.onVars(c), d = t.onPreview(v);
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
function Ot(e, t) {
  return e.attachShadow({ mode: "open" }), e.shadowRoot.innerHTML = `<style>${Ja}</style>
    <div class="hd">${t || ""}</div><div class="body"></div>`, e.shadowRoot.querySelector(".body");
}
const Un = (e, t) => e.config && e.config.title || e.binding && e.binding.var || t, Xn = (e, t) => t && e.vars[t.var] ? e.vars[t.var].value : void 0;
function xs(e) {
  return e == null ? { kind: "none", label: "—", color: "#bbb" } : e <= -99e4 ? { kind: "sys", label: "SYS", color: "#c084fc" } : e > 0 ? { kind: "ok", label: e > 1 ? `OK${e}` : "OK", color: "#3ad17a" } : e < 0 ? { kind: "ng", label: e < -1 ? `NG${-e}` : "NG", color: "#ff5b5b" } : { kind: "na", label: "NA", color: "#ffb454" };
}
const Es = (e) => !e || e.result === !0 || !e.var;
class Za extends HTMLElement {
  connectedCallback() {
    this.body = Ot(this, Un(this, "Verdict")), this.body.style.cssText = "display:flex;flex-direction:column;align-items:center;justify-content:center;gap:6px;text-align:center", this.big = document.createElement("div"), this.big.style.cssText = "font-weight:800;font-size:clamp(20px,7vw,72px);line-height:1", this.sub = document.createElement("div"), this.sub.style.cssText = "font-size:12px;color:#888;max-width:96%;overflow:hidden;text-overflow:ellipsis;white-space:nowrap", this.body.append(this.big, this.sub);
  }
  feed(t) {
    const n = this.binding || {};
    if (Es(n)) {
      const l = t.result, a = xs(l ? l.code : null);
      this.big.textContent = a.label, this.big.style.color = a.color, this.sub.textContent = l && l.msg ? l.msg : "";
      return;
    }
    const r = Xn(t, n), i = r === !0 || r === "OK" || r === "ok" || r === "PASS", s = r === !1 || r === "NG" || r === "ng" || r === "FAIL";
    this.big.textContent = r === void 0 ? "—" : i ? "OK" : s ? "NG" : String(r), this.big.style.color = i ? "#3ad17a" : s ? "#ff5b5b" : "#ccc", this.sub.textContent = "";
  }
}
class Qa extends HTMLElement {
  connectedCallback() {
    this.body = Ot(this, Un(this, "Value")), this.body.style.cssText = "display:flex;align-items:center;justify-content:center;font-size:clamp(16px,5vw,40px);font-weight:600";
  }
  feed(t) {
    var r;
    const n = Xn(t, this.binding);
    this.body.textContent = n === void 0 ? "—" : typeof n == "number" ? +n.toFixed(((r = this.config) == null ? void 0 : r.decimals) ?? 3) : String(n);
  }
}
class eo extends HTMLElement {
  connectedCallback() {
    this.body = Ot(this, Un(this, "Image")), this.body.style.cssText = "padding:0", this.viewer = document.createElement("xi-image-viewer"), this.viewer.style.cssText = "width:100%;height:100%;display:block", this.body.appendChild(this.viewer);
  }
  feed(t) {
    const n = this.binding && t.vars[this.binding.var], r = n ? n.src != null ? n.src : n.gid : void 0, i = n && r != null ? t.images[r] : void 0;
    i && i !== this._u && typeof this.viewer.setFrame == "function" && (this.viewer.setFrame(i), this._u = i);
  }
}
class to extends HTMLElement {
  connectedCallback() {
    this.body = Ot(this, Un(this, "SPC")), this.buf = [], this.last = -1, this.cv = document.createElement("canvas"), this.cv.style.cssText = "width:100%;height:100%", this.body.appendChild(this.cv);
  }
  feed(t) {
    var n;
    if (t.run_id !== this.last) {
      this.last = t.run_id;
      const r = Xn(t, this.binding);
      if (typeof r == "number") {
        this.buf.push(r);
        const i = ((n = this.config) == null ? void 0 : n.window) || 100;
        this.buf.length > i && this.buf.shift();
      }
    }
    this.draw();
  }
  draw() {
    var d, p, m;
    const t = this.cv, n = t.getBoundingClientRect();
    if (!n.width) return;
    t.width = n.width, t.height = n.height;
    const r = t.getContext("2d");
    if (r.clearRect(0, 0, t.width, t.height), !this.buf.length) return;
    const i = ((d = this.config) == null ? void 0 : d.mean) ?? this.buf.reduce((g, f) => g + f, 0) / this.buf.length, s = (p = this.config) == null ? void 0 : p.ucl, l = (m = this.config) == null ? void 0 : m.lcl;
    let a = Math.min(...this.buf), o = Math.max(...this.buf);
    s != null && (o = Math.max(o, s)), l != null && (a = Math.min(a, l));
    const c = (o - a) * 0.1 || 1;
    a -= c, o += c;
    const v = (g) => t.height - (g - a) / (o - a) * t.height, b = (g, f, _) => {
      g != null && (r.strokeStyle = f, r.setLineDash(_ || []), r.beginPath(), r.moveTo(0, v(g)), r.lineTo(t.width, v(g)), r.stroke(), r.setLineDash([]));
    };
    b(i, "#666"), b(s, "#ff5b5b", [4, 3]), b(l, "#ff5b5b", [4, 3]), r.strokeStyle = "#4aa0f0", r.lineWidth = 1.5, r.beginPath(), this.buf.forEach((g, f) => {
      const _ = f / Math.max(1, this.buf.length - 1) * t.width;
      f ? r.lineTo(_, v(g)) : r.moveTo(_, v(g));
    }), r.stroke();
  }
}
class no extends HTMLElement {
  connectedCallback() {
    var t;
    this.body = Ot(this, ((t = this.config) == null ? void 0 : t.title) || "Throughput"), this.buf = [], this.last = -1, this.body.style.cssText = "display:flex;flex-direction:column;align-items:center;justify-content:center;gap:4px", this.big = document.createElement("div"), this.big.style.cssText = "font-size:clamp(18px,6vw,44px);font-weight:700;color:#9ad", this.sub = document.createElement("div"), this.sub.style.cssText = "font-size:12px;color:#888", this.body.append(this.big, this.sub);
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
    this.body = Ot(this, ((t = this.config) == null ? void 0 : t.title) || "Yield"), this.ok = 0, this.ng = 0, this.last = -1, this.body.style.cssText = "display:flex;flex-direction:column;align-items:center;justify-content:center;gap:4px", this.big = document.createElement("div"), this.big.style.cssText = "font-size:clamp(18px,6vw,44px);font-weight:700", this.sub = document.createElement("div"), this.sub.style.cssText = "font-size:12px;color:#888", this.body.append(this.big, this.sub);
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
      const l = Xn(t, n);
      l !== void 0 && (l === !0 || l === "OK" || l === "ok" || l === "PASS" ? this.ok++ : this.ng++);
    }
    const r = this.ok + this.ng, i = r ? 100 * this.ok / r : 0;
    this.big.textContent = `${i.toFixed(1)}%`, this.big.style.color = i >= (((s = this.config) == null ? void 0 : s.warn) ?? 95) ? "#3ad17a" : "#ffb454", this.sub.textContent = `OK ${this.ok} / NG ${this.ng}` + (this.na ? ` · NA ${this.na}` : "");
  }
}
class io extends HTMLElement {
  connectedCallback() {
    var t;
    this.body = Ot(this, ((t = this.config) == null ? void 0 : t.title) || "Dispatch groups"), this.body.style.cssText = "display:flex;flex-direction:column;gap:10px;padding:10px;overflow:auto", this.peak = {}, this.rows = {};
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
        const v = document.createElement("div");
        v.style.cssText = "display:flex;gap:3px;height:18px", l.append(a, v), this.body.appendChild(l), this.rows[r.name] = l = { row: l, name: o, meta: c, bar: v, cells: [] };
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
const Hr = (e) => !!(e && e.card), Rt = (e) => !!(e && (e.dir === "row" || e.dir === "col") && Array.isArray(e.children) && e.children.length >= 1), qe = (e) => !!(e && Array.isArray(e.tabs) && e.tabs.length >= 1 && e.tabs.every((t) => t && t.child)), bn = () => ({ type: "value", bind: {}, config: { title: "(empty)" } });
function Fr(e) {
  const t = e.children.length;
  return (Array.isArray(e.weights) && e.weights.length === t ? e.weights.slice() : Array(t).fill(1)).map((r) => typeof r == "number" && r > 0 ? r : 1);
}
function so(e) {
  const t = Fr(e), n = t.reduce((r, i) => r + i, 0) || 1;
  return t.map((r) => r / n);
}
function $s(e, t) {
  return qe(e) ? e.tabs[t].child : e.children[t];
}
function lo(e, t, n) {
  if (qe(e)) {
    const i = e.tabs.slice();
    return i[t] = { ...i[t], child: n }, { ...e, tabs: i };
  }
  const r = e.children.slice();
  return r[t] = n, { ...e, children: r };
}
function gr(e, t, n = []) {
  if (Hr(e)) {
    t(e.card, n);
    return;
  }
  Rt(e) ? e.children.forEach((r, i) => gr(r, t, [...n, i])) : qe(e) && e.tabs.forEach((r, i) => gr(r.child, t, [...n, i]));
}
function mo(e) {
  let t = 0;
  return gr(e, () => t++), t;
}
function ao(e, t) {
  let n = e;
  for (const r of t)
    if (Rt(n) || qe(n)) n = $s(n, r);
    else return;
  return n;
}
function $e(e, t, n) {
  if (t.length === 0) return n(e);
  const [r, ...i] = t;
  return lo(e, r, $e($s(e, r), i, n));
}
function _o(e, t, n, r = bn()) {
  return $e(e, t, (i) => ({ dir: n === "col" ? "col" : "row", children: [i, { card: r }], weights: [1, 1] }));
}
function bo(e, t, n, r = bn()) {
  if (n = n === "col" ? "col" : "row", t.length === 0) return { dir: n, children: [e, { card: r }], weights: [1, 1] };
  const i = t.slice(0, -1), s = t[t.length - 1], l = ao(e, i);
  return Rt(l) && l.dir === n ? $e(e, i, (a) => {
    const o = a.children.slice();
    o.splice(s + 1, 0, { card: r });
    const c = Fr(a);
    return c.splice(s + 1, 0, c[s]), { ...a, children: o, weights: c };
  }) : $e(e, t, (a) => ({ dir: n, children: [a, { card: r }], weights: [1, 1] }));
}
function yo(e, t) {
  if (t.length === 0) return { card: bn() };
  const n = t.slice(0, -1), r = t[t.length - 1];
  return $e(e, n, (i) => {
    if (!Rt(i)) return i;
    const s = i.children.filter((a, o) => o !== r), l = Fr(i).filter((a, o) => o !== r);
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
  return $e(e, t, (n) => ({ tabs: [{ name: "Page 1", child: n }, { name: "Page 2", child: { card: bn() } }], active: 0 }));
}
function ko(e, t, n, r = { card: bn() }) {
  return $e(e, t, (i) => qe(i) ? { ...i, tabs: [...i.tabs, { name: n || `Page ${i.tabs.length + 1}`, child: r }], active: i.tabs.length } : i);
}
function $o(e, t, n) {
  return $e(e, t, (r) => {
    if (!qe(r)) return r;
    const i = r.tabs.filter((s, l) => l !== n);
    return i.length === 1 ? i[0].child : { ...r, tabs: i, active: Math.max(0, Math.min(r.active || 0, i.length - 1)) };
  });
}
function To(e, t, n, r) {
  return $e(e, t, (i) => qe(i) ? { ...i, tabs: i.tabs.map((s, l) => l === n ? { ...s, name: r } : s) } : i);
}
function So(e, t, n) {
  return $e(e, t, (r) => qe(r) ? { ...r, active: n } : r);
}
function si(e, t = "root") {
  return Hr(e) ? e.card.type ? [] : [`${t}: leaf has no card.type`] : Rt(e) ? e.children.flatMap((n, r) => si(n, `${t}.${r}`)) : qe(e) ? e.tabs.flatMap((n, r) => si(n.child, `${t}.${n.name || r}`)) : [`${t}: node is not a leaf {card}, split {dir,children}, or tabs {tabs}`];
}
function Co(e, { client: t, dashboard: n, pollStatsMs: r = 200 }) {
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
  function v(f) {
    const _ = ks[f.type], y = i.createElement(_ ? `xi-card-${f.type}` : "div");
    return _ || (y.textContent = `unknown card: ${f.type}`, y.style.cssText = "color:#f88;padding:8px"), y.binding = f.bind || {}, y.config = f.config || {}, y.style.minWidth = "0", y.style.minHeight = "0", y.style.overflow = "hidden", _ && a.push(y), y;
  }
  function b(f) {
    let _ = Math.min(f.active || 0, f.tabs.length - 1);
    const y = i.createElement("div");
    y.style.cssText = "display:flex;flex-direction:column;min-width:0;min-height:0;width:100%;height:100%";
    const h = i.createElement("div");
    h.style.cssText = "display:flex;gap:2px;flex:0 0 auto;align-items:center;padding:2px 2px 0;overflow:auto";
    const w = i.createElement("div");
    w.style.cssText = "flex:1 1 0;min-width:0;min-height:0;position:relative";
    const S = [], N = [], O = () => {
      S.forEach((W, j) => {
        const q = j === _;
        W.style.background = q ? "#1e1e1e" : "#121212", W.style.color = q ? "#ddd" : "#888";
      }), N.forEach((W, j) => {
        W.style.display = j === _ ? "" : "none";
      });
    };
    return f.tabs.forEach((W, j) => {
      const q = i.createElement("div");
      q.style.cssText = "padding:4px 11px;cursor:pointer;border-radius:6px 6px 0 0;border:1px solid #333;border-bottom:none;font:12px system-ui,sans-serif", q.textContent = W.name || `Page ${j + 1}`, q.onclick = () => {
        _ = j, O();
      }, S.push(q), h.appendChild(q);
      const G = d(W.child);
      G.style.cssText += ";position:absolute;inset:0;min-width:0;min-height:0;border:1px solid #333;border-radius:0 6px 6px 6px;overflow:hidden", N.push(G), w.appendChild(G);
    }), O(), y.append(h, w), y;
  }
  function d(f) {
    if (Hr(f)) return v(f.card);
    if (qe(f)) return b(f);
    if (!Rt(f)) {
      const w = i.createElement("div");
      return w.textContent = "bad layout node", w.style.color = "#f88", w;
    }
    const _ = f.dir === "col", y = i.createElement("div");
    y.style.cssText = `display:flex;flex-direction:${_ ? "column" : "row"};min-width:0;min-height:0;width:100%;height:100%`;
    const h = so(f);
    return f.children.forEach((w, S) => {
      const N = d(w);
      N.style.flex = `${h[S]} 1 0`, N.style.minWidth = "0", N.style.minHeight = "0", y.appendChild(N);
    }), y;
  }
  function p() {
    a = [], e.replaceChildren(), e.style.cssText += ";display:flex;min-width:0;min-height:0";
    const f = n && n.layout;
    if (!f) return;
    const _ = d(f);
    _.style.flex = "1 1 0", _.style.minWidth = "0", _.style.minHeight = "0", e.appendChild(_), c();
  }
  const m = [
    t.onVars((f) => {
      l.run_id = f.run_id, l.vars = f.items;
      const _ = {};
      for (const y of Object.keys(f.items || {})) {
        const h = f.items[y];
        h && h.gid != null && (_[h.gid] = h.src != null ? h.src : h.gid);
      }
      l.gidToCanon = _, c();
    }),
    t.onPreview((f) => {
      const _ = l.gidToCanon && f.gid in l.gidToCanon ? l.gidToCanon[f.gid] : f.gid;
      l.images[_] = f.dataUrl, c();
    }),
    t.onEvent((f) => {
      f.name === "run_finished" && f.data && typeof f.data.ms == "number" ? l.run_ms = f.data.ms : f.name === "run_result" && f.data ? (l.result = f.data, c()) : (f.name === "safe_state" || f.name === "status") && (l.status = f.data, c());
    })
  ], g = setInterval(() => {
    t.cmd("dispatch_stats").then((f) => {
      f && Array.isArray(f.groups) && (l.groups = f.groups, c());
    }).catch(() => {
    });
  }, r);
  return p(), {
    setDashboard(f) {
      n = f, p();
    },
    state: l,
    destroy() {
      m.forEach((f) => f()), clearInterval(g), e.replaceChildren();
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
  pr as TOOLS,
  Mo as XI_COMPONENTS,
  ho as XiClient,
  bo as addSibling,
  ko as addTab,
  ws as bytesToBase64,
  po as collectStatusItems,
  mo as countLeaves,
  ys as decodePreviewFrame,
  gr as eachLeaf,
  bn as emptyCard,
  ao as getNode,
  Ga as inferDescriptor,
  Hr as isLeaf,
  Rt as isSplit,
  qe as isTabs,
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
