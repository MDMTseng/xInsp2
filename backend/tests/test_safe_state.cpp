//
// test_safe_state.cpp — unit test for xi::SafeStateSink (xi_safe_state.hpp).
//
// The FE supervisor's safe-state seam is a SAFETY component: when the backend
// dies, this is what tells the line to go safe. These units pin its contract —
// reason→string mapping, factory fallthrough, log formatting, empty-field
// placeholders, and overflow/null safety — so a refactor can't quietly break
// the message an operator (or a future PLC transport) depends on.
//
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

#include <xi/xi_safe_state.hpp>

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// Run an action against a sink whose log is a tmpfile, return what it wrote.
template <class Fn>
static std::string capture(Fn&& fn) {
    std::FILE* f = std::tmpfile();
    xi::LoggingSafeStateSink sink(f);
    fn(sink);
    std::fflush(f);
    std::rewind(f);
    std::string out;
    char buf[1024];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

static bool contains(const std::string& h, const char* needle) {
    return h.find(needle) != std::string::npos;
}

int main() {
    using xi::SafeStateReason;
    using xi::SafeStateEvent;

    // SS-U1: to_string maps each enum + unknown fallback.
    {
        CHECK(std::string(xi::to_string(SafeStateReason::BackendExit)) == "BackendExit");
        CHECK(std::string(xi::to_string(SafeStateReason::PortUnresponsive)) == "PortUnresponsive");
        CHECK(std::string(xi::to_string(SafeStateReason::BootTimeout)) == "BootTimeout");
        CHECK(std::string(xi::to_string(SafeStateReason::RespawnLimitExceeded)) == "RespawnLimitExceeded");
        CHECK(std::string(xi::to_string(SafeStateReason::SupervisorShutdown)) == "SupervisorShutdown");
        // Out-of-range value → "Unknown" (defensive default in the switch).
        CHECK(std::string(xi::to_string(static_cast<SafeStateReason>(99))) == "Unknown");
    }

    // SS-U2: factory fallthrough — log / empty / unknown all give a logging sink.
    {
        auto a = xi::make_safe_state_sink("log");
        auto b = xi::make_safe_state_sink("");
        auto c = xi::make_safe_state_sink("modbus");  // not implemented yet → falls through
        CHECK(a && b && c);
        CHECK(std::string(a->name()) == "log");
        CHECK(std::string(b->name()) == "log");
        CHECK(std::string(c->name()) == "log");
    }

    // SS-U3: enter_safe_state formatting carries reason + rc hex + module + phase + report.
    {
        SafeStateEvent ev;
        ev.reason          = SafeStateReason::BackendExit;
        ev.backend_rc      = (int)0xC0000005;
        ev.faulting_module = "plugin_v0.dll";
        ev.last_phase      = "inspect";
        ev.report_path     = "C:\\tmp\\report.json";
        ev.ts_ms           = 1748599200123LL;
        std::string s = capture([&](xi::LoggingSafeStateSink& k){ k.enter_safe_state(ev); });
        CHECK(contains(s, "ENTER SAFE STATE"));
        CHECK(contains(s, "ts=1748599200123"));   // SS-U8: timestamp surfaced for forensics
        CHECK(contains(s, "reason=BackendExit"));
        CHECK(contains(s, "rc=0xC0000005"));
        CHECK(contains(s, "module=plugin_v0.dll"));
        CHECK(contains(s, "phase=inspect"));
        CHECK(contains(s, "report=C:\\tmp\\report.json"));
    }

    // SS-U4: clear_safe_state form.
    {
        std::string s = capture([&](xi::LoggingSafeStateSink& k){ k.clear_safe_state(); });
        CHECK(contains(s, "CLEAR SAFE STATE"));
    }

    // SS-U5: empty fields render as "-" placeholders (a clean SupervisorShutdown).
    {
        SafeStateEvent ev;
        ev.reason = SafeStateReason::SupervisorShutdown;  // no crash data
        std::string s = capture([&](xi::LoggingSafeStateSink& k){ k.enter_safe_state(ev); });
        CHECK(contains(s, "reason=SupervisorShutdown"));
        CHECK(contains(s, "module=-"));
        CHECK(contains(s, "phase=-"));
        CHECK(contains(s, "report=-"));
    }

    // SS-U6: overflow safety — oversized fields must not crash / overrun the 512 buf.
    {
        SafeStateEvent ev;
        ev.reason          = SafeStateReason::BackendExit;
        ev.faulting_module = std::string(4096, 'M');
        ev.report_path     = std::string(4096, 'R');
        std::string s = capture([&](xi::LoggingSafeStateSink& k){ k.enter_safe_state(ev); });
        // vsnprintf truncates into the fixed buffer; we just require it produced
        // a bounded, well-formed line and didn't corrupt memory.
        CHECK(contains(s, "ENTER SAFE STATE"));
        CHECK(s.size() < 1024);
    }

    // SS-U7: null FILE* (stderr-only path) — enter/clear must not crash.
    {
        xi::LoggingSafeStateSink sink(nullptr);
        SafeStateEvent ev;
        ev.reason = SafeStateReason::PortUnresponsive;
        sink.enter_safe_state(ev);   // writes to stderr only
        sink.clear_safe_state();
        CHECK(true);  // reaching here without a crash is the assertion
    }

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "%d CHECK(s) FAILED\n", g_failures);
    return 1;
}
