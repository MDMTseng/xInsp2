//
// test_health.cpp — the core-owned health/state contract (adoption-map item 10 /
// triage Bucket E). Design: docs/new_gen/04-health-contract.md.
//
// Drives xi::HealthRegistry through its state machine the way the real lifecycle
// paths do (open→loaded, start→running, caught fault→degraded, recover→running,
// stop→draining→loaded, fatal→fault) and asserts:
//
//   1. Every documented transition lands in the right state and emits exactly one
//      health_changed event; no-op transitions coalesce (no event).
//   2. running ⇄ degraded is DERIVED: a runtime fault (instance overlay or a
//      failed script) auto-flips running→degraded; clearing it flips back. Off the
//      running/degraded states the derivation does NOT fire (a fault outside a run
//      records the component but leaves the top state where it was).
//   3. The wire shapes: health_changed events and append_component() output PARSE
//      as JSON and carry the documented keys/values against schema xi.health/1.
//
// Like test_metrics this asserts the machine MOVES + the shape is right; it needs
// no backend/toolchain — the registry is header-only.
//
#include <xi/xi_health.hpp>
#include "yyjson.h"

#include <mutex>
#include <string>
#include <vector>
#include <cstdio>

static int fails = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::fprintf(stderr, "FAIL %s:%d: %s\n",                    \
                __FILE__, __LINE__, #expr);                             \
            ++fails;                                                    \
        }                                                               \
    } while (0)
#define SECTION(name) std::fprintf(stderr, "\n[section] %s\n", name)

// --- captured events --------------------------------------------------------
static std::mutex g_mu;
static std::vector<std::string> g_events;

static void reset_events() { std::lock_guard<std::mutex> lk(g_mu); g_events.clear(); }
static size_t event_count() { std::lock_guard<std::mutex> lk(g_mu); return g_events.size(); }
static std::string last_event() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_events.empty() ? std::string() : g_events.back();
}

// Pull a string field out of a parsed event's `data` object.
static std::string data_str(const std::string& ev, const char* key) {
    yyjson_doc* d = yyjson_read(ev.data(), ev.size(), 0);
    if (!d) return "<unparseable>";
    yyjson_val* root = yyjson_doc_get_root(d);
    yyjson_val* data = yyjson_obj_get(root, "data");
    yyjson_val* v = data ? yyjson_obj_get(data, key) : nullptr;
    std::string out = (v && yyjson_is_str(v)) ? yyjson_get_str(v) : std::string("<missing>");
    yyjson_doc_free(d);
    return out;
}

// Pull data.component.<key> (empty string if no component object).
static std::string comp_str(const std::string& ev, const char* key) {
    yyjson_doc* d = yyjson_read(ev.data(), ev.size(), 0);
    if (!d) return "<unparseable>";
    yyjson_val* root = yyjson_doc_get_root(d);
    yyjson_val* data = yyjson_obj_get(root, "data");
    yyjson_val* comp = data ? yyjson_obj_get(data, "component") : nullptr;
    yyjson_val* v = comp ? yyjson_obj_get(comp, key) : nullptr;
    std::string out = (v && yyjson_is_str(v)) ? yyjson_get_str(v) : std::string();
    yyjson_doc_free(d);
    return out;
}

static bool has_component(const std::string& ev) {
    yyjson_doc* d = yyjson_read(ev.data(), ev.size(), 0);
    if (!d) return false;
    yyjson_val* root = yyjson_doc_get_root(d);
    yyjson_val* data = yyjson_obj_get(root, "data");
    bool has = data && yyjson_obj_get(data, "component");
    yyjson_doc_free(d);
    return has;
}

int main() {
    using xi::health;
    using xi::SysState;
    using xi::CompHealth;

    auto& H = health();
    H.set_notifier([](const std::string& ev) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_events.push_back(ev);
    });
    H.reset_for_test();

    // ---------------------------------------------------------------------
    SECTION("boot → project_loaded → running");
    reset_events();
    CHECK(H.state() == SysState::Boot);

    CHECK(H.set_state(SysState::ProjectLoaded));           // open_project
    CHECK(H.state() == SysState::ProjectLoaded);
    CHECK(event_count() == 1);
    CHECK(data_str(last_event(), "state") == "project_loaded");
    CHECK(data_str(last_event(), "schema") == "xi.health/1");
    CHECK(!has_component(last_event()));                    // pure state transition

    CHECK(H.set_state(SysState::Running));                 // start
    CHECK(H.state() == SysState::Running);
    CHECK(event_count() == 2);
    CHECK(data_str(last_event(), "state") == "running");

    // ---------------------------------------------------------------------
    SECTION("running → degraded is DERIVED from a caught instance fault");
    reset_events();
    H.mark_instance_degraded("cam0", xi::kReasonPluginFault);   // SEH catch path
    CHECK(H.state() == SysState::Degraded);                 // auto-flip
    CHECK(event_count() == 1);
    CHECK(data_str(last_event(), "state") == "degraded");
    CHECK(has_component(last_event()));
    CHECK(comp_str(last_event(), "kind") == "instance");
    CHECK(comp_str(last_event(), "name") == "cam0");
    CHECK(comp_str(last_event(), "health") == "degraded");
    CHECK(comp_str(last_event(), "reason_code") == "plugin_fault");

    // Coalesce: same fault again → no event, still degraded.
    reset_events();
    H.mark_instance_degraded("cam0", xi::kReasonPluginFault);
    CHECK(event_count() == 0);
    CHECK(H.state() == SysState::Degraded);

    // Overlay is readable for the get_health merge.
    { std::string r; int64_t s = 0;
      CHECK(H.instance_degraded("cam0", r, s));
      CHECK(r == "plugin_fault"); }

    // ---------------------------------------------------------------------
    SECTION("degraded → running when the fault clears");
    reset_events();
    CHECK(H.clear_instance_degraded("cam0"));
    CHECK(H.state() == SysState::Running);                  // auto-flip back
    CHECK(event_count() == 1);
    CHECK(data_str(last_event(), "state") == "running");
    { std::string r; int64_t s = 0; CHECK(!H.instance_degraded("cam0", r, s)); }
    // Clearing a non-degraded instance is a no-op (no event).
    reset_events();
    CHECK(!H.clear_instance_degraded("cam0"));
    CHECK(event_count() == 0);

    // ---------------------------------------------------------------------
    SECTION("item 14: on_fault=refuse marks the overlay failed/quarantined");
    reset_events();
    CHECK(H.state() == SysState::Running);
    // A quarantine is a runtime-fault overlay at CompHealth::Failed (pulled from
    // service), distinct from the `degraded` (kept in service) crash overlay.
    H.mark_instance_fault("sink0", CompHealth::Failed, xi::kReasonQuarantined);
    CHECK(H.state() == SysState::Degraded);                 // any overlay ⇒ degraded top
    CHECK(event_count() == 1);
    CHECK(comp_str(last_event(), "kind") == "instance");
    CHECK(comp_str(last_event(), "name") == "sink0");
    CHECK(comp_str(last_event(), "health") == "failed");
    CHECK(comp_str(last_event(), "reason_code") == "quarantined");
    // instance_fault surfaces the health level for the get_health merge.
    { xi::CompHealth h; std::string r; int64_t s = 0;
      CHECK(H.instance_fault("sink0", h, r, s));
      CHECK(h == CompHealth::Failed);
      CHECK(r == "quarantined"); }
    // A quarantine → degraded transition on the SAME instance coalesces cleanly
    // (health + reason both change) and re-enable clears it back to running.
    reset_events();
    H.mark_instance_fault("sink0", CompHealth::Degraded, xi::kReasonPluginFault);
    CHECK(comp_str(last_event(), "health") == "degraded");
    CHECK(H.clear_instance_degraded("sink0"));
    CHECK(H.state() == SysState::Running);

    // ---------------------------------------------------------------------
    SECTION("a failed script also drives running → degraded → running");
    reset_events();
    H.set_script(CompHealth::Failed, xi::kReasonCompileError, "inspect.cpp");
    CHECK(H.state() == SysState::Degraded);
    CHECK(comp_str(last_event(), "kind") == "script");
    CHECK(comp_str(last_event(), "health") == "failed");
    CHECK(comp_str(last_event(), "reason_code") == "compile_error");
    H.set_script(CompHealth::Ok, "", "inspect.cpp");
    CHECK(H.state() == SysState::Running);
    { std::string n, r; CompHealth h; int64_t s = 0;
      CHECK(H.script_snapshot(n, h, r, s));
      CHECK(n == "inspect.cpp"); CHECK(h == CompHealth::Ok); }

    // ---------------------------------------------------------------------
    SECTION("stop: running → draining → project_loaded");
    reset_events();
    CHECK(H.set_state(SysState::Draining));
    CHECK(data_str(last_event(), "state") == "draining");
    CHECK(H.set_state(SysState::ProjectLoaded));
    CHECK(H.state() == SysState::ProjectLoaded);
    CHECK(event_count() == 2);

    // ---------------------------------------------------------------------
    SECTION("a fault OUTSIDE running records the component but does NOT flip state");
    reset_events();
    CHECK(H.state() == SysState::ProjectLoaded);
    H.mark_instance_degraded("cam1", xi::kReasonPluginFault);
    CHECK(H.state() == SysState::ProjectLoaded);            // no derivation off-run
    CHECK(has_component(last_event()));
    CHECK(comp_str(last_event(), "name") == "cam1");
    H.clear_all_instance_degraded();

    // ---------------------------------------------------------------------
    SECTION("close → boot; watchdog → fault; coalesce");
    reset_events();
    CHECK(H.set_state(SysState::Boot));                    // close_project
    CHECK(H.state() == SysState::Boot);
    // Coalesce a no-op transition.
    reset_events();
    CHECK(!H.set_state(SysState::Boot));
    CHECK(event_count() == 0);
    CHECK(H.set_state(SysState::Fault));                   // watchdog hard trip
    CHECK(data_str(last_event(), "state") == "fault");

    // ---------------------------------------------------------------------
    SECTION("append_component() wire shape");
    {
        std::string out;
        xi::HealthRegistry::append_component(out, xi::kKindSource, "cam0",
                                             CompHealth::Ok, "", 1234,
                                             ",\"last_emit_age_ms\":41");
        yyjson_doc* d = yyjson_read(out.data(), out.size(), 0);
        CHECK(d != nullptr);
        if (d) {
            yyjson_val* r = yyjson_doc_get_root(d);
            CHECK(std::string(yyjson_get_str(yyjson_obj_get(r, "kind"))) == "source");
            CHECK(std::string(yyjson_get_str(yyjson_obj_get(r, "name"))) == "cam0");
            CHECK(std::string(yyjson_get_str(yyjson_obj_get(r, "health"))) == "ok");
            CHECK(yyjson_get_sint(yyjson_obj_get(r, "since_ms")) == 1234);
            CHECK(yyjson_get_sint(yyjson_obj_get(r, "last_emit_age_ms")) == 41);
            yyjson_doc_free(d);
        }
    }

    // ---------------------------------------------------------------------
    SECTION("name escaping in the component/event JSON stays valid");
    {
        H.reset_for_test();
        H.set_state(SysState::Running);
        reset_events();
        H.mark_instance_degraded("weird\"name\\with\ttabs", xi::kReasonPluginFault);
        // The emitted event must still parse.
        yyjson_doc* d = yyjson_read(last_event().data(), last_event().size(), 0);
        CHECK(d != nullptr);
        if (d) {
            CHECK(comp_str(last_event(), "name") == std::string("weird\"name\\with\ttabs"));
            yyjson_doc_free(d);
        }
    }

    if (fails) {
        std::fprintf(stderr, "\n[test_health] %d CHECK(s) FAILED\n", fails);
        return 1;
    }
    std::fprintf(stderr, "\n[test_health] OK\n");
    return 0;
}
