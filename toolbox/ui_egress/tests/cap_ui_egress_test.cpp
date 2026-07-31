//
// cap_ui_egress_test.cpp — the LIVE-UI egress pipeline (spec 31 / docs/internals/
// ui-egress.md) end to end against the THREE REAL built DLLs through the REAL
// capability plane:
//
//     push -> xi.ui.egress (ui_egress: latest-wins slot + timer flusher)
//          -> xi.jpeg.encode (imgcodec: the encoder)
//          -> xi.ui.sink     (expose: byte-blind store + subscription gate)
//
// It pins the runtime the e2e python driver (qa/qa_ui_egress) cannot make
// deterministic — the LRU/slot semantics — WITHOUT reaching into egress internals
// (they are private in the .cpp): everything is observed through egress's "stats"
// exchange and expose's "get"/"subscribe" exchange, exactly as a real host would.
//
// The flusher is a TIMER thread, so the one non-determinism is WHEN a flush fires.
// egress's flusher flushes strictly every 1000/fps ms (a push does NOT force an
// early flush — the wake CV predicate only fires on shutdown), so a LOW fps gives
// a wide, race-free window to stage a burst between flushes; every wait polls the
// "flushes" counter to a boundary rather than sleeping a fixed time.
//
// Proven, all deterministic:
//   * full path: a subscribed channel's push is encoded ONCE and lands as a stored
//     frame in expose (found via "get").
//   * NO SUBSCRIBER -> ZERO ENCODE: a push to an unsubscribed channel drops at the
//     probe (dropped_no_sub++), encodes does NOT move, expose stores nothing.
//   * DEDUP: the same content flushed again is an LRU hit (dedup_hits++), encodes
//     PINNED — the "encodes vs dedup_hits" proof.
//   * LATEST-WINS: a burst of distinct frames between two flushes collapses to the
//     LAST one only — the two superseded frames do ZERO encode work, and the last
//     (pre-warmed into the LRU) resolves as a dedup hit.
//   * LRU EVICTION: 40 distinct contents, lru_max 32 -> lru_entries caps at 32.
//   * teardown: both providers unregister; pack-registry balance.
//
// Host role exactly like cap_imgcodec_test: ImagePool host_api + install_pack_abi
// + install_cap_plane; each factory runs under a pre-allocated owner scope the
// adapter adopts (the PM create path).
//
#include <xi/xi_abi.h>
#include <xi/xi_cap_abi.hpp>
#include <xi/xi_cabi_adapter.hpp>
#include <xi/xi_image_pool.hpp>
#include <xi/xi_instance.hpp>
#include <xi/xi_pack_abi.hpp>
#include <xi/xi_seh.hpp>

// LoadLibraryA/GetProcAddress/HMODULE/GetLastError for make_instance below:
// real <windows.h> on Windows, dlopen/dlsym shim on POSIX (same names).
#include <xi/xi_dynlib.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifndef UI_EGRESS_DLL_PATH
#define UI_EGRESS_DLL_PATH "ui_egress/xi-ui_egress.dll"
#endif
#ifndef IMGCODEC_DLL_PATH
#define IMGCODEC_DLL_PATH "imgcodec/xi-imgcodec.dll"
#endif
#ifndef EXPOSE_DLL_PATH
#define EXPOSE_DLL_PATH "expose/xi-expose.dll"
#endif

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)
#define SECTION(name) std::fprintf(stderr, "\n[section] %s\n", name)

// Minimal "key":int scan over a flat stats/ack JSON (the cap_imgcodec_test idiom).
static int jint(const std::string& s, const char* key) {
    std::string pat = std::string("\"") + key + "\"";
    auto p = s.find(pat);
    if (p == std::string::npos) return -12345;
    p = s.find(':', p);
    if (p == std::string::npos) return -12345;
    return std::atoi(s.c_str() + p + 1);
}
static bool jfound_true(const std::string& s) {
    return s.find("\"found\":true") != std::string::npos;
}

// Load a plugin DLL, run its factory under an owner scope, wrap it in the host
// adapter, and register it (the exact PM create path cap_imgcodec_test uses).
static std::shared_ptr<xi::CAbiInstanceAdapter>
make_instance(const char* dll_path, const char* iname, const char* pname,
              const xi_host_api* host) {
    HMODULE dll = LoadLibraryA(dll_path);
    if (!dll) {
        std::fprintf(stderr, "FAIL: LoadLibrary(%s) err %lu\n", dll_path, GetLastError());
        return nullptr;
    }
    auto create = reinterpret_cast<xi::PluginInfo::CFactoryFn>(
        GetProcAddress(dll, "xi_plugin_create"));
    if (!create) { std::fprintf(stderr, "FAIL: no xi_plugin_create in %s\n", dll_path); return nullptr; }
    xi::ImagePoolOwnerScope scope;
    void* raw = scope.run_factory([&] { return create(host, iname); });
    if (!raw) { std::fprintf(stderr, "FAIL: factory(%s) returned null\n", pname); return nullptr; }
    auto inst = std::make_shared<xi::CAbiInstanceAdapter>(
        iname, pname, dll, raw, /*reentrant=*/true, /*max_conc=*/0);
    inst->adopt_owner_id(scope.release());
    xi::InstanceRegistry::instance().add(inst);
    return inst;
}

// A deterministic, content-distinct 16x16 gray image keyed by `seed`.
static std::vector<uint8_t> gen(int seed) {
    constexpr int W = 16, H = 16;
    std::vector<uint8_t> px((size_t)W * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            px[(size_t)y * W + x] = (uint8_t)((x * 7 + y * 13 + seed * 101) & 0xFF);
    return px;
}

static xi_pack_handle build_push(const xi_pack_v1* pk, const char* chan,
                                 const std::vector<uint8_t>& px) {
    xi_pack_builder b = pk->builder_new();
    pk->builder_add_str(b, "$channel", chan, (int32_t)std::strlen(chan));
    pk->builder_add_image(b, "image", 16, 16, 1, px.data());
    return pk->builder_seal(b);
}
static void push(const xi_cap_v1* cap, const xi_pack_v1* pk, xi_pack_handle req) {
    xi_pack_handle out = XI_PACK_NULL;
    cap->call("xi.ui.egress", req, &out);
    if (out != XI_PACK_NULL) pk->release(out);
}

static std::string stats(xi::CAbiInstanceAdapter* egr) {
    return egr->exchange("{\"command\":\"stats\"}");
}
// Poll a stats counter until it reaches >= target (or time out); returns the last
// value read. Used to wait for a flush boundary without a fixed sleep.
static int wait_stat(xi::CAbiInstanceAdapter* egr, const char* key, int target, int timeout_ms) {
    int v = jint(stats(egr), key);
    for (int waited = 0; v < target && waited < timeout_ms; waited += 5) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        v = jint(stats(egr), key);
    }
    return v;
}

int main() {
    std::printf("[test] xi.ui.egress — the live-UI pipeline across ui_egress + imgcodec + expose\n");
    xi::install_seh_translator();

    static xi_host_api host = xi::ImagePool::make_host_api();
    xi::install_pack_abi();
    xi::install_cap_plane();
    const auto* pk  = static_cast<const xi_pack_v1*>(host.get_interface("xi.pack", 1));
    const auto* cap = static_cast<const xi_cap_v1*>(host.get_interface("xi.cap", 1));
    CHECK(pk && cap);
    if (!pk || !cap) return 1;
    const size_t frames_baseline = xi::PackRegistry::instance().live_frames();

    SECTION("load the three real DLLs; providers register on create");
    auto codec  = make_instance(IMGCODEC_DLL_PATH,  "codec",  "imgcodec",  &host);
    auto expose = make_instance(EXPOSE_DLL_PATH,    "expose", "expose",    &host);
    auto egress = make_instance(UI_EGRESS_DLL_PATH, "egress", "ui_egress", &host);
    CHECK(codec && expose && egress);
    if (!codec || !expose || !egress) return 1;
    CHECK(cap->available("xi.jpeg.encode") == 1);
    CHECK(cap->available("xi.ui.sink") == 1);
    CHECK(cap->available("xi.ui.egress") == 1);
    // A LOW flush rate → a wide, race-free window to stage a burst between flushes.
    CHECK(egress->set_def("{\"fps\":5}"));
    { std::string s = stats(egress.get());
      CHECK(jint(s, "encodes") == 0);
      CHECK(jint(s, "registered") == 1 || s.find("\"registered\":true") != std::string::npos); }

    // expose subscribes to ui/x only (ui/nosub deliberately left unsubscribed).
    expose->exchange("{\"command\":\"subscribe\",\"channels\":[\"ui/x\"]}");

    const std::vector<uint8_t> A = gen(1), X = gen(2), Y = gen(3);

    SECTION("full path: a subscribed push is encoded ONCE and stored in expose");
    {
        xi_pack_handle req = build_push(pk, "ui/x", A);
        push(cap, pk, req);
        pk->release(req);
        CHECK(wait_stat(egress.get(), "flushes", 1, 3000) >= 1);
        std::string s = stats(egress.get());
        CHECK(jint(s, "encodes") == 1);          // one real encode
        CHECK(jint(s, "dedup_hits") == 0);
        CHECK(jint(s, "dropped_no_sub") == 0);
        std::string g = expose->exchange("{\"command\":\"get\",\"channel\":\"ui/x\"}");
        CHECK(jfound_true(g));                    // the frame reached expose
    }

    SECTION("VIEWPORT RELAY (doc 37): the browser viewport rides the ui.sink probe reply");
    {
        // Probe xi.ui.sink for a channel; read back {subscribed, viewport?} — the
        // exact path the live-view pluginlet's LiveView::demand_() drives.
        auto probe = [&](const char* chan, int64_t& sub, std::string& vp) {
            xi_pack_builder b = pk->builder_new();
            pk->builder_add_str(b, "$channel", chan, (int32_t)std::strlen(chan));
            xi_pack_handle req = pk->builder_seal(b), rsp = XI_PACK_NULL;
            cap->call("xi.ui.sink", req, &rsp);
            pk->release(req);
            sub = 0; vp.clear();
            if (rsp != XI_PACK_NULL) {
                pk->get_i64(rsp, "subscribed", &sub);
                const char* p = nullptr; int32_t n = 0;
                if (pk->get_str(rsp, "viewport", &p, &n) && p && n > 0) vp.assign(p, (size_t)n);
                pk->release(rsp);
            }
        };
        int64_t sub = 0; std::string vp;
        probe("ui/x", sub, vp);
        CHECK(sub == 1);
        CHECK(vp.empty());                          // subscribed, but no viewport reported yet

        // The UI widget reports the browser's on-screen viewport for the channel.
        expose->exchange("{\"command\":\"viewport\",\"channel\":\"ui/x\",\"x\":10,\"y\":20,\"w\":100,\"h\":50}");
        probe("ui/x", sub, vp);
        CHECK(sub == 1);
        CHECK(vp == "10,20,100,50");                 // relayed back verbatim on the next probe

        // An UNSUBSCRIBED channel REFUSES a viewport (nobody's watching -> meaningless),
        // so nothing is stored and its probe carries no viewport.
        std::string r = expose->exchange("{\"command\":\"viewport\",\"channel\":\"ui/nosub\",\"x\":1,\"y\":2,\"w\":3,\"h\":4}");
        CHECK(r.find("not subscribed") != std::string::npos);
        probe("ui/nosub", sub, vp);
        CHECK(sub == 0);
        CHECK(vp.empty());
    }

    SECTION("GENERIC control relay (doc 37): a NEW key needs no expose change");
    {
        // Read an arbitrary control key off the probe reply — the same path
        // live-view uses for "viewport", but for a key expose has never heard of.
        auto probe_key = [&](const char* chan, const char* key, std::string& out) {
            xi_pack_builder b = pk->builder_new();
            pk->builder_add_str(b, "$channel", chan, (int32_t)std::strlen(chan));
            xi_pack_handle req = pk->builder_seal(b), rsp = XI_PACK_NULL;
            cap->call("xi.ui.sink", req, &rsp);
            pk->release(req);
            out.clear();
            if (rsp != XI_PACK_NULL) {
                const char* sp = nullptr; int32_t sn = 0;
                if (pk->get_str(rsp, key, &sp, &sn) && sp && sn > 0) out.assign(sp, (size_t)sn);
                pk->release(rsp);
            }
        };

        // A pluginlet expose knows nothing about sets its own key...
        expose->exchange("{\"command\":\"control\",\"channel\":\"ui/x\",\"key\":\"hist_bins\",\"value\":\"64\"}");
        std::string bins; probe_key("ui/x", "hist_bins", bins);
        CHECK(bins == "64");                       // relayed verbatim, byte-blind

        // ...and it coexists with viewport: BOTH keys ride the same probe reply.
        std::string vp2; probe_key("ui/x", "viewport", vp2);
        CHECK(vp2 == "10,20,100,50");

        // Latest-wins per key.
        expose->exchange("{\"command\":\"control\",\"channel\":\"ui/x\",\"key\":\"hist_bins\",\"value\":\"128\"}");
        probe_key("ui/x", "hist_bins", bins);
        CHECK(bins == "128");

        // A missing key is refused; an unsubscribed channel still stores nothing.
        std::string bad = expose->exchange("{\"command\":\"control\",\"channel\":\"ui/x\",\"value\":\"9\"}");
        CHECK(bad.find("missing key") != std::string::npos);
        expose->exchange("{\"command\":\"control\",\"channel\":\"ui/nosub\",\"key\":\"k\",\"value\":\"v\"}");
        std::string none; probe_key("ui/nosub", "k", none);
        CHECK(none.empty());

        // Unsubscribing drops EVERY control for the channel (no stale state).
        expose->exchange("{\"command\":\"unsubscribe\",\"channels\":[\"ui/x\"]}");
        expose->exchange("{\"command\":\"subscribe\",\"channels\":[\"ui/x\"]}");
        probe_key("ui/x", "hist_bins", bins);
        CHECK(bins.empty());
        probe_key("ui/x", "viewport", vp2);
        CHECK(vp2.empty());
    }

    SECTION("NO SUBSCRIBER -> ZERO ENCODE: an unsubscribed channel drops at the probe");
    {
        const int enc0 = jint(stats(egress.get()), "encodes");
        xi_pack_handle req = build_push(pk, "ui/nosub", A);
        push(cap, pk, req);
        pk->release(req);
        CHECK(wait_stat(egress.get(), "dropped_no_sub", 1, 3000) >= 1);
        std::string s = stats(egress.get());
        CHECK(jint(s, "encodes") == enc0);        // encodes did NOT move — zero work
        std::string g = expose->exchange("{\"command\":\"get\",\"channel\":\"ui/nosub\"}");
        CHECK(!jfound_true(g));                    // expose stored NOTHING for it
    }

    SECTION("DEDUP: the same content flushed again is an LRU hit, encodes PINNED");
    {
        const int enc0 = jint(stats(egress.get()), "encodes");
        const int ded0 = jint(stats(egress.get()), "dedup_hits");
        const int flu0 = jint(stats(egress.get()), "flushes");
        xi_pack_handle req = build_push(pk, "ui/x", A);   // identical content to test 1
        push(cap, pk, req);
        pk->release(req);
        CHECK(wait_stat(egress.get(), "flushes", flu0 + 1, 3000) >= flu0 + 1);
        std::string s = stats(egress.get());
        CHECK(jint(s, "encodes") == enc0);            // NO new encode
        CHECK(jint(s, "dedup_hits") >= ded0 + 1);     // served from the memo
        // The hit passed the IDENTITY WITNESS (second independent hash) — same
        // content is a hit, never misclassified as a collision.
        CHECK(jint(s, "dedup_collisions") == 0);
    }

    SECTION("LATEST-WINS: a burst between flushes collapses to the LAST frame only");
    {
        const int enc0 = jint(stats(egress.get()), "encodes");
        const int ded0 = jint(stats(egress.get()), "dedup_hits");
        const int flu0 = jint(stats(egress.get()), "flushes");
        // Stage X, Y (never-seen) then A (already in the LRU) with no wait — all
        // land in the one slot before the next flush, so only A survives.
        xi_pack_handle rx = build_push(pk, "ui/x", X);
        xi_pack_handle ry = build_push(pk, "ui/x", Y);
        xi_pack_handle ra = build_push(pk, "ui/x", A);
        push(cap, pk, rx);
        push(cap, pk, ry);
        push(cap, pk, ra);
        pk->release(rx); pk->release(ry); pk->release(ra);
        CHECK(wait_stat(egress.get(), "flushes", flu0 + 1, 3000) >= flu0 + 1);
        std::string s = stats(egress.get());
        CHECK(jint(s, "encodes") == enc0);            // X and Y did ZERO encode work
        CHECK(jint(s, "dedup_hits") >= ded0 + 1);     // the survivor (A, the LAST) hit
    }

    SECTION("CROSS-CHANNEL DEDUP: identical content on TWO channels encodes ONCE");
    {
        // A fresh content Z pushed to two SUBSCRIBED channels in the same flush
        // window: whichever the flusher reaches first encodes Z; the twin channel
        // resolves as an LRU hit — content identity is channel-independent, so
        // "identical content encodes once" holds ACROSS channels (spec 31).
        expose->exchange("{\"command\":\"subscribe\",\"channels\":[\"ui/y\"]}");
        const std::vector<uint8_t> Z = gen(7);        // never seen before
        const int enc0 = jint(stats(egress.get()), "encodes");
        const int ded0 = jint(stats(egress.get()), "dedup_hits");
        xi_pack_handle zx = build_push(pk, "ui/x", Z);
        xi_pack_handle zy = build_push(pk, "ui/y", Z);
        push(cap, pk, zx);
        push(cap, pk, zy);
        pk->release(zx); pk->release(zy);
        // Wait for the twin to be served from the memo (robust whether the two
        // channels flush in one cycle or two).
        CHECK(wait_stat(egress.get(), "dedup_hits", ded0 + 1, 3000) >= ded0 + 1);
        CHECK(jint(stats(egress.get()), "encodes") == enc0 + 1);   // encoded EXACTLY once
    }

    SECTION("LRU EVICTION: 40 distinct contents, lru_max 32 -> lru_entries caps at 32");
    {
        CHECK(egress->set_def("{\"fps\":120}"));      // fast flushes: one per distinct frame
        for (int i = 0; i < 40; ++i) {
            const int flu = jint(stats(egress.get()), "flushes");
            std::vector<uint8_t> d = gen(100 + i);     // distinct, never seen before
            xi_pack_handle req = build_push(pk, "ui/x", d);
            push(cap, pk, req);
            pk->release(req);
            wait_stat(egress.get(), "flushes", flu + 1, 3000);
        }
        std::string s = stats(egress.get());
        CHECK(jint(s, "lru_entries") == 32);          // bounded exactly at lru_max
    }

    SECTION("RAW PASSTHROUGH: encode=false ships the blob verbatim — zero encode, no LRU");
    {
        // The "this channel walks raw" knob (doc 31 route 1): with encode=false
        // the flusher bypasses dispatch AND the memo — the pushed self-describing
        // blob rides the wire as-is, so pull and push both see raw.
        CHECK(egress->set_def("{\"encode\":false}"));
        CHECK(egress->get_def().find("\"encode\":false") != std::string::npos);
        const int enc0 = jint(stats(egress.get()), "encodes");
        const int lru0 = jint(stats(egress.get()), "lru_entries");
        const std::vector<uint8_t> R = gen(55);        // never seen before
        xi_pack_handle req = build_push(pk, "ui/x", R);
        push(cap, pk, req);
        pk->release(req);
        CHECK(wait_stat(egress.get(), "raw_passthrough", 1, 3000) >= 1);
        std::string s = stats(egress.get());
        CHECK(jint(s, "encodes") == enc0);             // the codec was never touched
        CHECK(jint(s, "lru_entries") == lru0);         // and the memo not grown
        std::string g = expose->exchange("{\"command\":\"get\",\"channel\":\"ui/x\"}");
        CHECK(jfound_true(g));                          // the raw frame reached expose
        CHECK(egress->set_def("{\"encode\":true}"));    // restore for teardown parity
    }

    SECTION("teardown: both providers unregister; pack balance");
    {
        xi::InstanceRegistry::instance().remove("egress");
        egress.reset();
        CHECK(cap->available("xi.ui.egress") == 0);
        xi::InstanceRegistry::instance().remove("expose");
        expose.reset();
        CHECK(cap->available("xi.ui.sink") == 0);
        xi::InstanceRegistry::instance().remove("codec");
        codec.reset();
        CHECK(cap->available("xi.jpeg.encode") == 0);
    }
    CHECK(xi::PackRegistry::instance().live_frames() == frames_baseline);

    if (g_failures == 0) {
        std::printf("\n[test] cap_ui_egress: ALL OK\n");
        return 0;
    }
    std::fprintf(stderr, "\n[test] cap_ui_egress: %d FAILURE(S)\n", g_failures);
    return 1;
}
