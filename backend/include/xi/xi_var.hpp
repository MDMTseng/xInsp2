#pragma once
//
// xi_var.hpp — value tracking for xInsp2 inspection routines.
//
// VAR(name, expr) — evaluate `expr`, store the result in a per-run value
// store keyed by `name`, and leave an in-scope variable named `name` for
// the user to keep writing normal C++.
//
// Example:
//
//   void inspect(Image frame) {
//       VAR(gray,    toGray(frame));
//       VAR(blurred, gaussian(gray, 3.0));
//       VAR(edges,   canny(blurred, 50, 150));
//   }
//
// After the run, the service walks ValueStore::current() and serializes each
// entry into a `vars` message (see protocol/messages.md). Images get a gid
// and are streamed separately as JPEG `preview` binary frames.
//
// Design notes:
//  - The store is thread-local so nested `inspect()` calls on different
//    worker threads don't collide.
//  - Entries are type-erased via a small variant. A type trait decides how
//    each value is rendered (image, number, string, json, custom).
//  - Values are move-captured. For large images the stored copy is the only
//    one kept alive for preview after the run — cheap enough for JPEG encode.
//

#include <any>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "xi_image.hpp"
#include "xi_record.hpp"

namespace xi {

// Categorizes how the frontend should render a tracked value.
enum class VarKind : uint8_t {
    Unknown = 0,
    Image,     // bitmap-like, will be JPEG-encoded on the preview path
    Number,    // int/float/double
    Boolean,
    String,
    Json,      // arbitrary structured data already serialized
    Custom,    // plugin-provided renderer
};

// Minimal metadata for one tracked value. The actual value is in `payload`
// as a std::any — the renderer callback knows how to pull it out.
struct VarEntry {
    std::string name;
    VarKind     kind = VarKind::Unknown;
    std::any    payload;
    // For images: the gid assigned when emitting the `vars` message so the
    // frontend can match binary preview frames back to variables.
    uint32_t    gid = 0;
    // For numbers/booleans/strings — inline serialization ready to go.
    std::string inline_json;
};

// Trait: how should a value of type T be tracked?
// Users can specialize this to teach VAR() about custom types.
template <class T, class = void>
struct VarTraits {
    static constexpr VarKind kind = VarKind::Unknown;
    // Default: stash the value, no inline serialization.
    static void fill(VarEntry& e, T&& v) {
        e.kind    = VarKind::Custom;
        e.payload = std::forward<T>(v);
    }
};

// --- built-in traits ---

template <class T>
struct VarTraits<T, std::enable_if_t<std::is_arithmetic_v<T> && !std::is_same_v<T, bool>>> {
    static constexpr VarKind kind = VarKind::Number;
    static void fill(VarEntry& e, T v) {
        e.kind        = VarKind::Number;
        if constexpr (std::is_floating_point_v<T>) {
            // std::to_string(NaN/Inf) yields "nan"/"inf" — invalid JSON that
            // corrupts the ENTIRE vars message (the client's parse fails, the
            // frame is dropped). Emit a quoted sentinel instead: valid JSON,
            // stays visibly non-finite rather than silently vanishing. (v != v
            // is the NaN test; matches Record's non-finite handling.)
            if (v != v || v > std::numeric_limits<T>::max() || v < std::numeric_limits<T>::lowest()) {
                const char* s = (v != v) ? "NaN" : (v > 0 ? "Infinity" : "-Infinity");
                e.inline_json = std::string("\"") + s + "\"";
                return;
            }
        }
        // Full round-trip precision: std::to_string uses %f (6 decimals), which
        // truncates small magnitudes to "0.000000" and drops precision on the
        // vars/history wire. %.17g round-trips a double exactly.
        if constexpr (std::is_floating_point_v<T>) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.17g", (double)v);
            e.inline_json = buf;
        } else {
            e.inline_json = std::to_string(v);
        }
    }
};

template <>
struct VarTraits<bool> {
    static constexpr VarKind kind = VarKind::Boolean;
    static void fill(VarEntry& e, bool v) {
        e.kind        = VarKind::Boolean;
        e.inline_json = v ? "true" : "false";
    }
};

template <>
struct VarTraits<std::string> {
    static constexpr VarKind kind = VarKind::String;
    static void fill(VarEntry& e, std::string v) {
        e.kind = VarKind::String;
        // Caller is responsible for escaping when they build the final JSON.
        e.payload = std::move(v);
    }
};

// VAR(x, "literal") — without this specialization, the `const char*`
// decay would hit the default `Custom` trait and stash a raw pointer
// (dangling if the literal came from a temporary). Copy into std::string.
template <>
struct VarTraits<const char*> {
    static constexpr VarKind kind = VarKind::String;
    static void fill(VarEntry& e, const char* v) {
        e.kind    = VarKind::String;
        e.payload = std::string(v ? v : "");
    }
};

template <>
struct VarTraits<char*> {
    static constexpr VarKind kind = VarKind::String;
    static void fill(VarEntry& e, char* v) {
        e.kind    = VarKind::String;
        e.payload = std::string(v ? v : "");
    }
};

template <>
struct VarTraits<Image> {
    static constexpr VarKind kind = VarKind::Image;
    static void fill(VarEntry& e, Image img) {
        e.kind = VarKind::Image;
        e.payload = std::move(img);
    }
};

template <>
struct VarTraits<Record> {
    static constexpr VarKind kind = VarKind::Json;
    static void fill(VarEntry& e, Record rec) {
        e.kind = VarKind::Json;
        e.inline_json = rec.data_json();
        e.payload = std::move(rec);  // keep the full Record for image access
    }
};

// Per-run, per-thread value store. Cleared at the start of each run by the
// service, walked at the end to build the `vars` message.
class ValueStore {
public:
    // Thread-local accessor — xi::ValueStore::current() inside VAR().
    static ValueStore& current() {
        thread_local ValueStore inst;
        return inst;
    }

    void clear() {
        std::lock_guard<std::mutex> g(mu_);
        entries_.clear();
    }

    // Copy the value into the store and return the caller's original
    // untouched. Never moves from the user's value — otherwise the user
    // would be left with a moved-from variable after VAR().
    //
    // For cheap types (arithmetic, bool, small strings) the extra copy is
    // noise. For heavy types (images) specialize VarTraits to stash a
    // shared handle rather than a deep copy.
    template <class T>
    decltype(auto) track(std::string_view name, T&& value) {
        VarEntry e;
        e.name = std::string(name);
        using Decayed = std::decay_t<T>;
        Decayed copy_for_store = value;
        VarTraits<Decayed>::fill(e, std::move(copy_for_store));
        {
            std::lock_guard<std::mutex> g(mu_);
            entries_.push_back(std::move(e));
        }
        return std::forward<T>(value);
    }

    // Read-only snapshot for serialization.
    std::vector<VarEntry> snapshot() const {
        std::lock_guard<std::mutex> g(mu_);
        return entries_;
    }

private:
    mutable std::mutex     mu_;
    std::vector<VarEntry>  entries_;
};

// VAR(name, expr) — track and bind.
//
// Two forms:
//   VAR(name, expr);        // statement form, introduces `auto name`
//   VAR_RAW(name, expr);    // same but flags as raw (no JPEG on preview)
//
// The value is evaluated exactly once. `name` is available after the macro.
//
// VAR introduces a local binding for `name`, so it CANNOT surface a value you
// already have a variable for, and the same `name` twice in one scope is a C++
// redefinition: `VAR(count, count)` (or two `VAR(count, …)`) expands to a second
// `auto count = …` and cl.exe fires C2374/C2371. The backend recognises this and
// appends a clear hint to the diagnostic — "duplicate VAR(count) at lines X, Y …
// use EMIT(count)" (see xi_script_compiler.hpp augment_var_redefinitions) — so it
// no longer reads as a mysterious macro error. To surface a PRE-EXISTING in-scope
// value, use EMIT (below) — it declares nothing:
//
//     int count = blobs.size();
//     EMIT(count);                 // surfaces `count`; no redefinition
//
// Use VAR to declare-and-surface in one step; use EMIT to surface something you
// already have. (See docs/guides/write-a-script.md.)
#define VAR(name, expr)                                                        \
    auto name = ::xi::ValueStore::current().track(#name, (expr))

#define VAR_RAW(name, expr)                                                    \
    auto name = ::xi::ValueStore::current().track(#name "!raw", (expr))

// Surface an EXISTING in-scope variable under its own name WITHOUT declaring a
// new binding — the collision-free way to do what `VAR(foo, foo)` can't. Expands
// to a void-discarded expression statement, so it's safe anywhere VAR is.
#define EMIT(name)     (void)::xi::ValueStore::current().track(#name, (name))
#define EMIT_RAW(name) (void)::xi::ValueStore::current().track(#name "!raw", (name))

} // namespace xi
