#pragma once
//
// xi_param.hpp — tunable values for xInsp2 inspection routines.
//
// A Param<T> is a named, type-checked, range-annotated value that the UI
// renders as a slider / number picker / checkbox. Editing it in the UI
// updates the in-memory value via the WS `set_param` command; the running
// script reads it transparently through implicit conversion.
//
// Example:
//
//   xi::Param<double> sigma{"sigma", 3.0, {0.1, 10.0}};
//   xi::Param<int>    low  {"canny_low", 50, {0, 255}};
//
//   void inspect(Image frame) {
//       VAR(blurred, gaussian(gray, sigma));      // reads sigma
//       VAR(edges,   canny(blurred, low, 150));
//   }
//
// Params auto-register into the ParamRegistry at construction time. The
// service exposes the registry via `list_params`, and persists current
// values to project.json on any UI edit.
//

#include <atomic>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace xi {

template <class T>
struct Range {
    T min{};
    T max{};
    constexpr Range() = default;
    constexpr Range(T lo, T hi) : min(lo), max(hi) {}
};

// Type erasure for the registry so all params sit in one map regardless of T.
class ParamBase {
public:
    virtual ~ParamBase() = default;
    virtual const std::string& name() const = 0;
    virtual std::string         type_name() const = 0;
    virtual std::string         as_json() const = 0;       // { "value":..., "min":..., "max":... }
    virtual bool                set_from_json(const std::string& v) = 0;
};

class ParamRegistry {
public:
    static ParamRegistry& instance() {
        static ParamRegistry r;
        return r;
    }

    void add(ParamBase* p) {
        std::lock_guard<std::mutex> g(mu_);
        params_[p->name()] = p;
    }

    void remove(ParamBase* p) {
        std::lock_guard<std::mutex> g(mu_);
        auto it = params_.find(p->name());
        if (it != params_.end() && it->second == p) params_.erase(it);
    }

    ParamBase* find(const std::string& name) {
        std::lock_guard<std::mutex> g(mu_);
        auto it = params_.find(name);
        return it == params_.end() ? nullptr : it->second;
    }

    std::vector<ParamBase*> list() {
        std::lock_guard<std::mutex> g(mu_);
        std::vector<ParamBase*> out;
        out.reserve(params_.size());
        for (auto& [k, v] : params_) out.push_back(v);
        return out;
    }

    void clear() {
        std::lock_guard<std::mutex> g(mu_);
        params_.clear();
    }

private:
    std::mutex                                   mu_;
    std::unordered_map<std::string, ParamBase*>  params_;
};

// Param<T> — concrete tunable. Registered on construction, unregistered on
// destruction. Thread-safe reads via std::atomic for arithmetic T.
template <class T>
class Param : public ParamBase {
    static_assert(std::is_arithmetic_v<T> || std::is_same_v<T, bool>,
                  "xi::Param<T> currently supports arithmetic and bool types. "
                  "Use xi::Instance<T> for heavier state.");
public:
    Param(std::string name, T initial, Range<T> range = {})
        : name_(std::move(name)), value_(initial), range_(range) {
        ParamRegistry::instance().add(this);
    }

    ~Param() override {
        ParamRegistry::instance().remove(this);
    }

    Param(const Param&) = delete;
    Param& operator=(const Param&) = delete;

    // Implicit conversion — reads the current value.
    operator T() const { return value_.load(std::memory_order_relaxed); }

    T get() const { return value_.load(std::memory_order_relaxed); }

    void set(T v) {
        if (range_.min != range_.max) {
            if (v < range_.min) v = range_.min;
            if (v > range_.max) v = range_.max;
        }
        value_.store(v, std::memory_order_relaxed);
    }

    const std::string& name() const override { return name_; }

    std::string type_name() const override {
        if constexpr (std::is_same_v<T, bool>)    return "bool";
        if constexpr (std::is_integral_v<T>)      return "int";
        if constexpr (std::is_floating_point_v<T>) return "float";
        return "number";
    }

    std::string as_json() const override {
        // Minimal hand-rolled JSON to keep this header dep-free. Names
        // are escaped (param names may contain quotes or backslashes
        // from user-supplied identifiers).
        std::string s = "{\"name\":";
        escape_json(s, name_);
        s += ",\"type\":\"" + type_name() + "\",";
        s += "\"value\":" + to_str(get());
        if (range_.min != range_.max) {
            s += ",\"min\":" + to_str(range_.min);
            s += ",\"max\":" + to_str(range_.max);
        }
        s += "}";
        return s;
    }

    static void escape_json(std::string& out, const std::string& in) {
        out.push_back('"');
        for (char c : in) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if ((unsigned char)c < 0x20) {
                        char b[8];
                        std::snprintf(b, sizeof(b), "\\u%04x", (unsigned)c);
                        out += b;
                    } else out.push_back(c);
            }
        }
        out.push_back('"');
    }

    bool set_from_json(const std::string& v) override {
        // Accept a bare number / true / false. Full JSON parsing lives in
        // the service layer; by the time we get here the value is a scalar.
        if constexpr (std::is_same_v<T, bool>) {
            if (v == "true")  { set(true);  return true; }
            if (v == "false") { set(false); return true; }
            return false;
        } else {
            try {
                if constexpr (std::is_integral_v<T>) {
                    set(static_cast<T>(std::stoll(v)));
                } else {
                    set(static_cast<T>(std::stod(v)));
                }
                return true;
            } catch (...) {
                return false;
            }
        }
    }

private:
    static std::string to_str(T v) {
        if constexpr (std::is_same_v<T, bool>) return v ? "true" : "false";
        else return std::to_string(v);
    }

    std::string                name_;
    std::atomic<T>             value_;
    Range<T>                   range_;
};

} // namespace xi
