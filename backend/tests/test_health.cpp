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
#include <xi/xi_health_mirror.hpp>
#include "yyjson.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
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
    // The FE mirror: mirror_json()/mirror_snapshot() project the TOP-LEVEL health
    // (state + since_ms + last_reason) into the tiny file the FE polls — the FE
    // can't call get_health without stealing the single WS client slot. last_reason
    // carries the reason that drove the last non-ok state, and clears on a clean
    // return to running/loaded/boot.
    SECTION("FE mirror: state + last_reason projection");
    {
        H.reset_for_test();
        // Clean boot: state=boot, no reason.
        std::string mstate, mreason; int64_t msince = 0;
        H.mirror_snapshot(mstate, msince, mreason);
        CHECK(mstate == "boot");
        CHECK(mreason.empty());

        // Drive to running, then a caught fault -> degraded carries the reason.
        H.set_state(SysState::ProjectLoaded);
        H.set_state(SysState::Running);
        H.mirror_snapshot(mstate, msince, mreason);
        CHECK(mstate == "running");
        CHECK(mreason.empty());   // clean running clears any prior reason

        H.mark_instance_degraded("cam0", xi::kReasonPluginFault);
        H.mirror_snapshot(mstate, msince, mreason);
        CHECK(mstate == "degraded");
        CHECK(mreason == "plugin_fault");   // the fault reason is mirrored

        // The JSON form parses and carries the same fields.
        {
            std::string mj = H.mirror_json();
            yyjson_doc* d = yyjson_read(mj.data(), mj.size(), 0);
            CHECK(d != nullptr);
            if (d) {
                yyjson_val* r = yyjson_doc_get_root(d);
                CHECK(std::string(yyjson_get_str(yyjson_obj_get(r, "state"))) == "degraded");
                CHECK(std::string(yyjson_get_str(yyjson_obj_get(r, "last_reason"))) == "plugin_fault");
                CHECK(yyjson_is_int(yyjson_obj_get(r, "since_ms")));
                CHECK(yyjson_is_int(yyjson_obj_get(r, "ts_ms")));
                yyjson_doc_free(d);
            }
        }

        // Clearing the fault flips back to running AND clears the reason.
        H.clear_instance_degraded("cam0");
        H.mirror_snapshot(mstate, msince, mreason);
        CHECK(mstate == "running");
        CHECK(mreason.empty());

        // A watchdog hard trip -> fault carries watchdog_trip even with no component.
        H.set_state(SysState::Fault);
        H.mirror_snapshot(mstate, msince, mreason);
        CHECK(mstate == "fault");
        CHECK(mreason == "watchdog_trip");
    }

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

    // ---------------------------------------------------------------------
    // F3 regression: the FE health-mirror write must NEVER expose a torn/empty
    // file, even when many threads write it concurrently. The real notifier fires
    // with HealthRegistry::mu_ released, so two workers faulting different
    // instances can land in the writer at once; the writer's function-static mutex
    // + atomic_write must keep the canonical file always-complete. A concurrent
    // reader polling the file (like the FE supervisor) must always parse valid JSON.
    SECTION("F3: concurrent health-mirror writes never tear the file");
    {
        namespace fs = std::filesystem;
        fs::path dir = fs::temp_directory_path() / "xinsp_health_mirror_test";
        std::error_code ec; fs::create_directories(dir, ec);
        fs::path mirror = dir / "health.json";
        fs::remove(mirror, ec);

        constexpr int kWriters = 8;
        constexpr int kIters   = 400;
        std::atomic<bool> go{false};
        std::atomic<bool> stop{false};
        std::atomic<int>  torn{0};      // reader saw an unparseable/empty file
        std::atomic<int>  reads{0};

        // Each writer emits a DISTINCT, differently-sized valid JSON payload so a
        // clobbered/interleaved write would produce invalid JSON a reader can catch.
        auto writer = [&](int id) {
            while (!go.load()) std::this_thread::yield();
            for (int i = 0; i < kIters; ++i) {
                std::ostringstream js;
                js << "{\"state\":\"degraded\",\"writer\":" << id
                   << ",\"iter\":" << i
                   << ",\"pad\":\"" << std::string((size_t)(id * 37 + i % 53), 'x')
                   << "\"}";
                xi::write_health_mirror_file(mirror.string(), js.str());
            }
        };
        // The reader models the FE supervisor: it opens the canonical path and
        // parses it. It must see a whole JSON object every time — never empty,
        // never truncated. (An absent file before the first write is skipped.)
        auto reader = [&]() {
            while (!go.load()) std::this_thread::yield();
            while (!stop.load()) {
                std::ifstream f(mirror, std::ios::binary);
                if (!f) continue;
                std::string s((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
                if (s.empty()) continue;   // between remove and first write
                reads.fetch_add(1);
                yyjson_doc* d = yyjson_read(s.data(), s.size(), 0);
                if (!d || !yyjson_is_obj(yyjson_doc_get_root(d))) {
                    torn.fetch_add(1);
                }
                if (d) yyjson_doc_free(d);
            }
        };

        std::vector<std::thread> ts;
        for (int i = 0; i < kWriters; ++i) ts.emplace_back(writer, i);
        std::thread rd(reader);
        std::thread rd2(reader);
        go.store(true);
        for (auto& t : ts) t.join();
        stop.store(true);
        rd.join(); rd2.join();

        // The file must end complete and valid too.
        std::ifstream f(mirror, std::ios::binary);
        std::string s((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
        yyjson_doc* d = yyjson_read(s.data(), s.size(), 0);
        CHECK(d != nullptr);
        CHECK(d && yyjson_is_obj(yyjson_doc_get_root(d)));
        if (d) yyjson_doc_free(d);

        CHECK(torn.load() == 0);         // <-- the F3 assertion: no torn read, ever
        CHECK(reads.load() > 0);         // the reader actually observed the file
        std::fprintf(stderr, "[F3] %d reads, %d torn\n", reads.load(), torn.load());
        fs::remove_all(dir, ec);
    }

    if (fails) {
        std::fprintf(stderr, "\n[test_health] %d CHECK(s) FAILED\n", fails);
        return 1;
    }
    std::fprintf(stderr, "\n[test_health] OK\n");
    return 0;
}
