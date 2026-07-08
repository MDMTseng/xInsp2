#pragma once
//
// xi_mp_build.hpp — sequential, count-free builders and read-views over the
// canonical msgpack codec (xi_mp.hpp).
//
// WRITE — MapBuilder / ArrayBuilder let you build a map/array WITHOUT knowing
// the element count up front: open a container, append entries in order, close.
// close() backpatches the true count into the fixed-width header (Writer::
// open_map/open_array/set_count). Maps and arrays nest arbitrarily, in any
// order — the only rule is LIFO: a child container must close before its parent
// writes again (msgpack bytes are linear, so two open siblings would interleave).
// A DEBUG-only interleave guard (Writer::dbg_leaf_) asserts that rule at the
// exact offending call; release builds carry zero guard state.
//
//   xi::mp::Doc doc;
//   auto m = doc.root_map();
//   m.add("count", (int64_t)3);
//   auto arr = m.open_array("objects");        // nested array of maps
//   for (auto& o : objs) { auto e = arr.open_map(); e.add("x", o.x).add("y", o.y); e.close(); }
//   arr.close();
//   m.close();
//   // doc.bytes() is now one canonical msgpack map.
//
// READ — MapReader / ArrayReader walk a canonical map/array by pull-decoding
// (skip_value threads past values you don't ask for). Scalar getters return
// std::optional; sub()/at() hand back the raw canonical bytes of a nested
// value so you can descend with another reader. Zero-copy: all views point into
// the caller's buffer.
//
#include <xi/xi_mp.hpp>

#include <cassert>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace xi {
namespace mp {

// ===========================================================================
// WRITE
// ===========================================================================

#ifndef NDEBUG
#define XI_MP_ASSERT_LEAF(w, self)                                             \
    assert((w)->dbg_leaf_ == (self) &&                                         \
           "xi::mp builder: wrote to a parent while a child container was "    \
           "still open — close the child first (LIFO nesting)")
#else
#define XI_MP_ASSERT_LEAF(w, self) ((void)0)
#endif

class ArrayBuilder;
class MapReader;    // read views (defined below) — builders can splice them
class ArrayReader;
class Node;

// A map being built. Move-only (it owns a backpatch slot in the Writer buffer);
// closes on destruction if not closed explicitly.
class MapBuilder {
public:
    explicit MapBuilder(Writer& w) : w_(&w) {
#ifndef NDEBUG
        prev_leaf_ = w.dbg_leaf_;
#endif
        at_ = w.open_map();
#ifndef NDEBUG
        w.dbg_leaf_ = this;
#endif
    }
    MapBuilder(MapBuilder&& o) noexcept { steal_(o); }
    MapBuilder& operator=(MapBuilder&& o) noexcept { if (this != &o) { close(); steal_(o); } return *this; }
    MapBuilder(const MapBuilder&) = delete;
    MapBuilder& operator=(const MapBuilder&) = delete;
    ~MapBuilder() { close(); }

    MapBuilder& add(std::string_view k, int64_t v)          { key_(k); w_->int_(v);    return *this; }
    MapBuilder& add(std::string_view k, double v)           { key_(k); w_->float_(v);  return *this; }
    MapBuilder& add(std::string_view k, std::string_view v) { key_(k); w_->str(v);     return *this; }
    MapBuilder& add(std::string_view k, bool v)             { key_(k); w_->boolean(v); return *this; }
    MapBuilder& add(std::string_view k, const char* v)      { return add(k, std::string_view(v)); }

    // Splice an already-CANONICAL msgpack subtree under `k` verbatim (no
    // re-encoding). Bytes MUST be canonical (from this library's Writer, or a
    // canonicalize() pass) — foreign/narrow bytes would break the doc's profile.
    // Pairs with the readers' bytes()/data(): add_raw(k, sub.bytes()).
    MapBuilder& add_raw(std::string_view k, const uint8_t* p, size_t n) { key_(k); w_->raw_canonical(p, n); return *this; }
    MapBuilder& add_raw(std::string_view k, std::pair<const uint8_t*, size_t> b) { return add_raw(k, b.first, b.second); }
    MapBuilder& add(std::string_view k, const MapReader& v);      // splice a read map
    MapBuilder& add(std::string_view k, const ArrayReader& v);    // splice a read array

    // Nested containers under `k`. The returned child must close before this
    // map writes again (LIFO; the debug guard enforces it).
    MapBuilder   open_map(std::string_view k)   { key_(k); return MapBuilder(*w_); }
    ArrayBuilder open_array(std::string_view k);   // defined after ArrayBuilder

    void close() {
        if (!w_) return;
        w_->set_count(at_, n_);
#ifndef NDEBUG
        w_->dbg_leaf_ = prev_leaf_;
#endif
        w_ = nullptr;
    }

private:
    void key_(std::string_view k) { XI_MP_ASSERT_LEAF(w_, this); w_->key(k); ++n_; }
    void steal_(MapBuilder& o) {
        w_ = o.w_; at_ = o.at_; n_ = o.n_;
#ifndef NDEBUG
        prev_leaf_ = o.prev_leaf_;
        if (w_ && w_->dbg_leaf_ == &o) w_->dbg_leaf_ = this;
#endif
        o.w_ = nullptr;
    }
    Writer*  w_  = nullptr;
    size_t   at_ = 0;
    uint32_t n_  = 0;
#ifndef NDEBUG
    const void* prev_leaf_ = nullptr;
#endif
};

// An array being built. Elements have no keys; same LIFO / backpatch rules.
class ArrayBuilder {
public:
    explicit ArrayBuilder(Writer& w) : w_(&w) {
#ifndef NDEBUG
        prev_leaf_ = w.dbg_leaf_;
#endif
        at_ = w.open_array();
#ifndef NDEBUG
        w.dbg_leaf_ = this;
#endif
    }
    ArrayBuilder(ArrayBuilder&& o) noexcept { steal_(o); }
    ArrayBuilder& operator=(ArrayBuilder&& o) noexcept { if (this != &o) { close(); steal_(o); } return *this; }
    ArrayBuilder(const ArrayBuilder&) = delete;
    ArrayBuilder& operator=(const ArrayBuilder&) = delete;
    ~ArrayBuilder() { close(); }

    ArrayBuilder& push(int64_t v)          { elem_(); w_->int_(v);    return *this; }
    ArrayBuilder& push(double v)           { elem_(); w_->float_(v);  return *this; }
    ArrayBuilder& push(std::string_view v) { elem_(); w_->str(v);     return *this; }
    ArrayBuilder& push(bool v)             { elem_(); w_->boolean(v); return *this; }
    ArrayBuilder& push(const char* v)      { return push(std::string_view(v)); }

    // Splice a canonical subtree as one element (see MapBuilder::add_raw).
    ArrayBuilder& push_raw(const uint8_t* p, size_t n) { elem_(); w_->raw_canonical(p, n); return *this; }
    ArrayBuilder& push_raw(std::pair<const uint8_t*, size_t> b) { return push_raw(b.first, b.second); }
    ArrayBuilder& push(const MapReader& v);
    ArrayBuilder& push(const ArrayReader& v);

    MapBuilder   open_map()   { elem_no_write_(); return MapBuilder(*w_); }
    ArrayBuilder open_array() { elem_no_write_(); return ArrayBuilder(*w_); }

    void close() {
        if (!w_) return;
        w_->set_count(at_, n_);
#ifndef NDEBUG
        w_->dbg_leaf_ = prev_leaf_;
#endif
        w_ = nullptr;
    }

private:
    void elem_()          { XI_MP_ASSERT_LEAF(w_, this); ++n_; }
    void elem_no_write_() { XI_MP_ASSERT_LEAF(w_, this); ++n_; }
    void steal_(ArrayBuilder& o) {
        w_ = o.w_; at_ = o.at_; n_ = o.n_;
#ifndef NDEBUG
        prev_leaf_ = o.prev_leaf_;
        if (w_ && w_->dbg_leaf_ == &o) w_->dbg_leaf_ = this;
#endif
        o.w_ = nullptr;
    }
    Writer*  w_  = nullptr;
    size_t   at_ = 0;
    uint32_t n_  = 0;
#ifndef NDEBUG
    const void* prev_leaf_ = nullptr;
#endif
};

inline ArrayBuilder MapBuilder::open_array(std::string_view k) { key_(k); return ArrayBuilder(*w_); }

// A msgpack document: owns a Writer, hands out a root map or array, yields the
// finished canonical bytes.
class Doc {
public:
    MapBuilder   root_map()   { return MapBuilder(w_); }
    ArrayBuilder root_array() { return ArrayBuilder(w_); }
    const Bytes& bytes() const { return w_.bytes(); }
    Writer&      writer()      { return w_; }
    void         clear()       { w_.clear(); }
private:
    Writer w_;
};

// ===========================================================================
// READ
// ===========================================================================

// Advance the reader past exactly one complete value (recursing through nested
// containers). next() already steps past Str/Bin/Ext payloads, so only Map /
// Array need to recurse. Returns false on a malformed/truncated buffer.
inline bool skip_value(Reader& r) {
    Element e;
    if (r.next(e) != Status::Ok) return false;
    if (e.kind == Kind::Array) {
        for (uint32_t i = 0; i < e.len; ++i)
            if (!skip_value(r)) return false;
    } else if (e.kind == Kind::Map) {
        for (uint32_t i = 0; i < e.len; ++i) {
            if (!skip_value(r)) return false;   // key
            if (!skip_value(r)) return false;   // value
        }
    }
    return true;
}

// Widening coercions — a read never has to match the exact wire type. Because
// Reader::next() already folds every integer width (uint8/16/32/64 -> UInt,
// int8/16/32/64 + fixint -> Int), these cover the whole ladder in one place, so
// MapReader and ArrayReader read the same way:
//   * i64  <- any signed/unsigned integer width
//   * f64  <- float, or any integer (widened)
//   * bool <- a real bool, or an integer 0/1 (tolerant, like PackIn::bool_or)
// A uint64 above INT64_MAX wraps on the i64 path (use f64 if that range matters).
inline std::optional<int64_t> as_i64(const Element& e) {
    if (e.kind == Kind::Int)  return e.i;
    if (e.kind == Kind::UInt) return (int64_t)e.u;
    return std::nullopt;
}
inline std::optional<double> as_f64(const Element& e) {
    if (e.kind == Kind::Float) return e.d;
    if (e.kind == Kind::Int)   return (double)e.i;
    if (e.kind == Kind::UInt)  return (double)e.u;
    return std::nullopt;
}
inline std::optional<std::string_view> as_str(const Element& e) {
    if (e.kind == Kind::Str) return std::string_view((const char*)e.data, e.len);
    return std::nullopt;
}
inline std::optional<bool> as_bool(const Element& e) {
    if (e.kind == Kind::Bool) return e.b;
    if (e.kind == Kind::Int)  return e.i != 0;
    if (e.kind == Kind::UInt) return e.u != 0;
    return std::nullopt;
}

class ArrayReader;
class Node;

// Zero-copy read view over a canonical msgpack MAP's bytes. Absent/wrong-type
// getters return nullopt. sub()/array() return a nested value's raw bytes.
class MapReader {
public:
    MapReader() = default;
    MapReader(const uint8_t* data, size_t len) : data_(data), len_(len) {}

    bool valid() const { return data_ != nullptr; }

    // The canonical msgpack bytes backing THIS view — this map's whole value,
    // header included (for a nested map, just that sub-map's slice). Borrowed:
    // valid only while the source buffer lives. Re-open with MapReader(p, n).
    const uint8_t* data()  const { return data_; }
    size_t         bytes_size() const { return len_; }
    std::pair<const uint8_t*, size_t> bytes() const { return {data_, len_}; }

    std::optional<int64_t> i64(std::string_view k) const {
        Element v; return seek_(k, v) ? as_i64(v) : std::nullopt;
    }
    std::optional<double> f64(std::string_view k) const {
        Element v; return seek_(k, v) ? as_f64(v) : std::nullopt;
    }
    std::optional<std::string_view> str(std::string_view k) const {
        Element v; return seek_(k, v) ? as_str(v) : std::nullopt;
    }
    std::optional<bool> boolean(std::string_view k) const {
        Element v; return seek_(k, v) ? as_bool(v) : std::nullopt;
    }
    bool has(std::string_view k) const { Element v; return seek_(k, v); }

    // Walk every entry as (key, Node value) — generic / producer-agnostic, for
    // tooling that does not know the keys. fn(std::string_view, Node). Defined
    // after Node.
    template <class Fn> void for_each(Fn&& fn) const;

    // Raw canonical bytes of the value at `k` (for a nested map/array/anything).
    std::optional<std::pair<const uint8_t*, size_t>> raw(std::string_view k) const {
        if (!data_) return std::nullopt;
        Reader r(data_, len_);
        Element m; if (r.next(m) != Status::Ok || m.kind != Kind::Map) return std::nullopt;
        for (uint32_t i = 0; i < m.len; ++i) {
            Element key; if (r.next(key) != Status::Ok || key.kind != Kind::Str) return std::nullopt;
            std::string_view ks((const char*)key.data, key.len);
            size_t vbeg = r.offset();
            if (!skip_value(r)) return std::nullopt;
            if (ks == k) return std::make_pair(data_ + vbeg, r.offset() - vbeg);
        }
        return std::nullopt;
    }
    MapReader   map(std::string_view k) const {
        auto b = raw(k); return b ? MapReader(b->first, b->second) : MapReader();
    }
    ArrayReader array(std::string_view k) const;

    // Path access — get<T>(path, fallback) for scalars/containers, or
    // getNode(path).asMap()/.asArray() for structural. See Node below.
    Node node() const;
    Node getNode(std::string_view path) const;
    template <class T> T get(std::string_view path, T fallback = T{}) const;

private:
    // Position a fresh element `out` at the value for key `k` (by decoding it).
    bool seek_(std::string_view k, Element& out) const {
        if (!data_) return false;
        Reader r(data_, len_);
        Element m; if (r.next(m) != Status::Ok || m.kind != Kind::Map) return false;
        for (uint32_t i = 0; i < m.len; ++i) {
            Element key; if (r.next(key) != Status::Ok || key.kind != Kind::Str) return false;
            std::string_view ks((const char*)key.data, key.len);
            if (ks == k) return r.next(out) == Status::Ok;
            if (!skip_value(r)) return false;
        }
        return false;
    }
    const uint8_t* data_ = nullptr;
    size_t         len_  = 0;
};

// Zero-copy read view over a canonical msgpack ARRAY's bytes. Typed accessors
// mirror MapReader's, keyed by index instead of string (same widening via
// as_i64/as_f64/as_bool). Each index access decodes from the start (O(n)) — the
// arrays here are short (status/data lists); for a full scan prefer for_each.
class ArrayReader {
public:
    ArrayReader() = default;
    ArrayReader(const uint8_t* data, size_t len) : data_(data), len_(len) {}

    bool valid() const { return data_ != nullptr; }
    uint32_t size() const {                              // ELEMENT count (not bytes)
        if (!data_) return 0;
        Reader r(data_, len_); Element a;
        if (r.next(a) != Status::Ok || a.kind != Kind::Array) return 0;
        return a.len;
    }

    // The canonical msgpack bytes backing THIS array view (header included).
    // Borrowed. Note size() is the element count; use bytes_size() for the byte
    // length.
    const uint8_t* data()  const { return data_; }
    size_t         bytes_size() const { return len_; }
    std::pair<const uint8_t*, size_t> bytes() const { return {data_, len_}; }

    // Typed element access by index — same shape and widening as MapReader.
    std::optional<int64_t>          i64(uint32_t i) const { Element v; return at_(i, v) ? as_i64(v) : std::nullopt; }
    std::optional<double>           f64(uint32_t i) const { Element v; return at_(i, v) ? as_f64(v) : std::nullopt; }
    std::optional<std::string_view> str(uint32_t i) const { Element v; return at_(i, v) ? as_str(v) : std::nullopt; }
    std::optional<bool>             boolean(uint32_t i) const { Element v; return at_(i, v) ? as_bool(v) : std::nullopt; }
    MapReader   map(uint32_t i) const   { auto b = raw(i); return b ? MapReader(b->first, b->second) : MapReader(); }
    ArrayReader array(uint32_t i) const { auto b = raw(i); return b ? ArrayReader(b->first, b->second) : ArrayReader(); }

    // Raw canonical bytes of element i (for descending into a nested value).
    std::optional<std::pair<const uint8_t*, size_t>> raw(uint32_t i) const { return raw_at_(i); }

    // Single pass over raw element bytes; fn(const uint8_t*, size_t). Descend a
    // map element with MapReader(ptr,len), an array with ArrayReader.
    template <class Fn>
    void for_each(Fn&& fn) const {
        if (!data_) return;
        Reader r(data_, len_); Element a;
        if (r.next(a) != Status::Ok || a.kind != Kind::Array) return;
        for (uint32_t i = 0; i < a.len; ++i) {
            size_t vbeg = r.offset();
            if (!skip_value(r)) return;
            fn(data_ + vbeg, r.offset() - vbeg);
        }
    }

    // Visit each element as a Node — fn(Node). Defined after Node.
    template <class Fn> void for_each_node(Fn&& fn) const;
    // Collect every element coerced to T (int64_t/double/bool/string_view/
    // std::string/MapReader/ArrayReader/Node). Defined after Node.
    template <class T> std::vector<T> to_vector() const;

private:
    bool at_(uint32_t idx, Element& out) const {
        if (!data_) return false;
        Reader r(data_, len_); Element a;
        if (r.next(a) != Status::Ok || a.kind != Kind::Array || idx >= a.len) return false;
        for (uint32_t i = 0; i < idx; ++i) if (!skip_value(r)) return false;
        return r.next(out) == Status::Ok;
    }
    std::optional<std::pair<const uint8_t*, size_t>> raw_at_(uint32_t idx) const {
        if (!data_) return std::nullopt;
        Reader r(data_, len_); Element a;
        if (r.next(a) != Status::Ok || a.kind != Kind::Array || idx >= a.len) return std::nullopt;
        for (uint32_t i = 0; i < idx; ++i) if (!skip_value(r)) return std::nullopt;
        size_t b = r.offset();
        if (!skip_value(r)) return std::nullopt;
        return std::make_pair(data_ + b, r.offset() - b);
    }
    const uint8_t* data_ = nullptr;
    size_t         len_  = 0;
};

inline ArrayReader MapReader::array(std::string_view k) const {
    auto b = raw(k); return b ? ArrayReader(b->first, b->second) : ArrayReader();
}

// A path-addressable view of ONE msgpack value (map / array / scalar). Navigate
// with a dotted/bracketed path — getNode(".a.b.c[3]") — or operator[]; the
// as* coercions take a DEFAULT (returned when the node is missing or the wrong
// type), so a whole lookup is one expression:
//   int64_t n = root.getNode(".data.objects[3].x").asInt64(-1);
//   auto     a = root.getNode(".a.b.c").asArray();
// Every step is nullopt-safe: a missing key / out-of-range index / type
// mismatch yields an invalid Node, and reads then fall back to the default.
class Node {
public:
    Node() = default;
    Node(const uint8_t* d, size_t n) : data_(d), len_(n), valid_(d != nullptr) {}

    bool exists() const { return valid_; }
    explicit operator bool() const { return valid_; }

    // The canonical msgpack bytes of THIS value (scalar/map/array), for hashing,
    // re-embedding (Writer::raw_canonical), or re-opening. Borrowed; {nullptr,0}
    // when the node is invalid.
    const uint8_t* data()  const { return data_; }
    size_t         bytes_size() const { return len_; }
    std::pair<const uint8_t*, size_t> bytes() const { return {data_, len_}; }

    // Type introspection — for tooling that renders an unknown tree. kind() is
    // the decoded msgpack Kind (Nil for an invalid/missing node — use exists()
    // to tell them apart). is_*() are false on an invalid node.
    Kind kind() const { Element e; return decode_(e) ? e.kind : Kind::Nil; }
    bool is_map()    const { return valid_ && kind() == Kind::Map; }
    bool is_array()  const { return valid_ && kind() == Kind::Array; }
    bool is_int()    const { Kind k = kind(); return valid_ && (k == Kind::Int || k == Kind::UInt); }
    bool is_double() const { return valid_ && kind() == Kind::Float; }
    bool is_str()    const { return valid_ && kind() == Kind::Str; }
    bool is_bool()   const { return valid_ && kind() == Kind::Bool; }
    bool is_null()   const { return valid_ && kind() == Kind::Nil; }

    // Coercions (same widening as MapReader; missing/wrong-type -> default).
    int64_t          asInt64(int64_t def = 0) const            { Element e; return decode_(e) ? as_i64(e).value_or(def) : def; }
    double           asDouble(double def = 0) const            { Element e; return decode_(e) ? as_f64(e).value_or(def) : def; }
    std::string_view asString(std::string_view def = {}) const { Element e; return decode_(e) ? as_str(e).value_or(def) : def; }
    bool             asBool(bool def = false) const            { Element e; return decode_(e) ? as_bool(e).value_or(def) : def; }

    // Structural views of this node.
    MapReader   asMap()   const { return valid_ ? MapReader(data_, len_)   : MapReader(); }
    ArrayReader asArray() const { return valid_ ? ArrayReader(data_, len_) : ArrayReader(); }

    // Coerce THIS node to T (no navigation). The type dispatch shared by get<T>
    // and ArrayReader::to_vector<T>.
    template <class T>
    T as(T fallback = T{}) const {
        if constexpr (std::is_same_v<T, bool>)                  return asBool(fallback);
        else if constexpr (std::is_integral_v<T>)               return (T)asInt64((int64_t)fallback);
        else if constexpr (std::is_floating_point_v<T>)         return (T)asDouble((double)fallback);
        else if constexpr (std::is_same_v<T, std::string_view>) return asString(fallback);
        else if constexpr (std::is_same_v<T, std::string>)      return std::string(asString(std::string_view(fallback)));
        else if constexpr (std::is_same_v<T, MapReader>)        return asMap();
        else if constexpr (std::is_same_v<T, ArrayReader>)      return asArray();
        else if constexpr (std::is_same_v<T, Node>)             return *this;
        else static_assert(!sizeof(T*), "as<T>: T must be int/float/bool/string_view/std::string/MapReader/ArrayReader/Node");
    }

    // Programmatic navigation (one step).
    Node operator[](std::string_view key) const { return child_key_(key); }
    Node operator[](uint32_t idx) const          { return child_idx_(idx); }
    Node operator[](int idx) const               { return child_idx_((uint32_t)idx); }

    // Dotted/bracketed path: ".a.b.c[3]", "objects[2].x", "[0]". Leading '.'
    // optional; keys run until the next '.' or '['. Returns an invalid Node on a
    // missing/typed/malformed step (reads then fall back to the default).
    Node getNode(std::string_view path) const {
        Node cur = *this;
        size_t i = 0;
        while (i < path.size() && cur.valid_) {
            char c = path[i];
            if (c == '.') { ++i; continue; }
            if (c == '[') {
                ++i; uint32_t idx = 0; bool any = false;
                while (i < path.size() && path[i] >= '0' && path[i] <= '9') {
                    idx = idx * 10u + (uint32_t)(path[i] - '0'); ++i; any = true;
                }
                if (!any || i >= path.size() || path[i] != ']') return Node();  // malformed
                ++i;
                cur = cur.child_idx_(idx);
            } else {
                size_t s = i;
                while (i < path.size() && path[i] != '.' && path[i] != '[') ++i;
                cur = cur.child_key_(path.substr(s, i - s));
            }
        }
        return cur;
    }

    // Typed one-shot: navigate `path`, then coerce to T with a fallback.
    //   scalars   : get<int64_t>/<double>/<bool>/<std::string_view>/<std::string>
    //   containers: get<MapReader>/<ArrayReader> (an absent/wrong-type path
    //               yields an invalid, read-safe view — same as getNode(path)
    //               .asMap()/.asArray(); the fallback is unused for these)
    template <class T>
    T get(std::string_view path, T fallback = T{}) const { return getNode(path).template as<T>(fallback); }

private:
    bool decode_(Element& e) const {
        if (!valid_) return false;
        Reader r(data_, len_);
        return r.next(e) == Status::Ok;
    }
    Node child_key_(std::string_view k) const {
        if (!valid_) return Node();
        auto b = MapReader(data_, len_).raw(k);
        return b ? Node(b->first, b->second) : Node();
    }
    Node child_idx_(uint32_t idx) const {
        if (!valid_) return Node();
        auto b = ArrayReader(data_, len_).raw(idx);
        return b ? Node(b->first, b->second) : Node();
    }
    const uint8_t* data_ = nullptr;
    size_t         len_  = 0;
    bool           valid_ = false;
};

inline Node MapReader::node() const { return Node(data_, len_); }
inline Node MapReader::getNode(std::string_view path) const { return node().getNode(path); }
template <class T>
inline T MapReader::get(std::string_view path, T fallback) const { return node().template get<T>(path, fallback); }

// --- definitions needing the complete Node type -----------------------------

template <class Fn>
inline void MapReader::for_each(Fn&& fn) const {
    if (!data_) return;
    Reader r(data_, len_); Element m;
    if (r.next(m) != Status::Ok || m.kind != Kind::Map) return;
    for (uint32_t i = 0; i < m.len; ++i) {
        Element k; if (r.next(k) != Status::Ok || k.kind != Kind::Str) return;
        std::string_view ks((const char*)k.data, k.len);
        size_t vbeg = r.offset();
        if (!skip_value(r)) return;
        fn(ks, Node(data_ + vbeg, r.offset() - vbeg));
    }
}

template <class Fn>
inline void ArrayReader::for_each_node(Fn&& fn) const {
    if (!data_) return;
    Reader r(data_, len_); Element a;
    if (r.next(a) != Status::Ok || a.kind != Kind::Array) return;
    for (uint32_t i = 0; i < a.len; ++i) {
        size_t vbeg = r.offset();
        if (!skip_value(r)) return;
        fn(Node(data_ + vbeg, r.offset() - vbeg));
    }
}

template <class T>
inline std::vector<T> ArrayReader::to_vector() const {
    std::vector<T> out;
    if (!data_) return out;
    Reader r(data_, len_); Element a;
    if (r.next(a) != Status::Ok || a.kind != Kind::Array) return out;
    out.reserve(a.len);
    for (uint32_t i = 0; i < a.len; ++i) {
        size_t vbeg = r.offset();
        if (!skip_value(r)) break;
        out.push_back(Node(data_ + vbeg, r.offset() - vbeg).template as<T>());
    }
    return out;
}

inline MapBuilder&   MapBuilder::add(std::string_view k, const MapReader& v)   { return add_raw(k, v.data(), v.bytes_size()); }
inline MapBuilder&   MapBuilder::add(std::string_view k, const ArrayReader& v) { return add_raw(k, v.data(), v.bytes_size()); }
inline ArrayBuilder& ArrayBuilder::push(const MapReader& v)   { return push_raw(v.data(), v.bytes_size()); }
inline ArrayBuilder& ArrayBuilder::push(const ArrayReader& v) { return push_raw(v.data(), v.bytes_size()); }

} // namespace mp
} // namespace xi
