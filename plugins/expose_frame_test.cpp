//
// expose_frame_test.cpp — the polaris2 wave-2 GENERIC PROOF (docs/new_gen/08
// Wave 2 step 3): the REAL expose DLL walks a sealed frame with ZERO producer
// knowledge and emits it as an XEX1 wire frame.
//
// Loads the shipped expose plugin through the genuine C-ABI load path and drives
// the xi.frame@1 frame-in door:
//
//   * DOOR PROBE — expose (a SINK) now publishes xi_plugin_get_interface and
//     answers ("xi.frame", 1): it consumes frames.
//   * v2 opt-in — with frame_wire_v2 set, a subscribed channel's frame is dumped
//     as the canonical XEX1-v2 frame (scalars/str/mp pass through, image inlined
//     as raw bin). The emitted bytes are captured off the host binary sink and
//     decoded back — channel/seq lifted from $channel/$seq, every entry present.
//   * v1 default — without the opt-in, the same walk produces the legacy XEX1-v1
//     display frame (v:1), so existing clients keep working by default.
//   * SUBSCRIPTION GATING — an unsubscribed channel emits nothing.
//
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#endif

#include <xi/xi_cabi_adapter.hpp>   // CAbiInstanceAdapter (has_frame_door / process_frame_door)
#include <xi/xi_image_pool.hpp>     // make_host_api + cumulative().live_now
#include <xi/xi_frame_abi.hpp>      // install_frame_abi + frame_v1_iface + FrameRegistry
#include <xi/xi_binary_sink.hpp>    // capture emit_binary (the plugin -> WS byte pipe)
#include <xi/xi_mp.hpp>             // decode the emitted XEX1 body

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#ifndef EXPOSE_DLL_PATH
#define EXPOSE_DLL_PATH "xi-expose.dll"
#endif

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)
#define SECTION(name) std::printf("[test] %s\n", name)

// --- capture the plugin's emit_binary off the host sink ---------------------
static std::vector<std::vector<uint8_t>> g_emitted;
static void capture_binary(const void* data, int len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    g_emitted.emplace_back(p, p + (len > 0 ? len : 0));
}

// --- a tiny XEX1 body walker (canonical msgpack) ----------------------------
static bool skip_value(xi::mp::Reader& r) {
    xi::mp::Element e;
    if (r.next(e) != xi::mp::Status::Ok) return false;
    if (e.kind == xi::mp::Kind::Array) {
        for (uint32_t i = 0; i < e.len; ++i) if (!skip_value(r)) return false;
    } else if (e.kind == xi::mp::Kind::Map) {
        for (uint32_t i = 0; i < e.len; ++i) { if (!skip_value(r)) return false; if (!skip_value(r)) return false; }
    }
    return true;
}
static std::string as_str(const xi::mp::Element& e) {
    return std::string(reinterpret_cast<const char*>(e.data), e.len);
}

// Decoded view of an XEX1-v2 frame (only the fields this test asserts).
struct V2View {
    bool ok = false;
    int64_t v = 0, seq = 0;
    std::string channel;
    bool has_count = false; int64_t count = 0;
    bool has_score = false; double score = 0;
    bool has_label = false; std::string label = "";
    bool has_image = false; int64_t iw = 0, ih = 0, ic = 0;
    std::vector<uint8_t> px;
};

static V2View decode_v2(const std::vector<uint8_t>& frame) {
    V2View out;
    if (frame.size() < 5 || frame[0] != 'X' || frame[1] != 'E' || frame[2] != 'X' || frame[3] != '1')
        return out;
    xi::mp::Reader r(frame.data() + 4, frame.size() - 4);
    xi::mp::Element top;
    if (r.next(top) != xi::mp::Status::Ok || top.kind != xi::mp::Kind::Map) return out;
    for (uint32_t i = 0; i < top.len; ++i) {
        xi::mp::Element k;
        if (r.next(k) != xi::mp::Status::Ok || k.kind != xi::mp::Kind::Str) return out;
        std::string key = as_str(k);
        if (key == "v" || key == "seq") {
            xi::mp::Element v; if (r.next(v) != xi::mp::Status::Ok) return out;
            (key == "v" ? out.v : out.seq) = v.i;
        } else if (key == "channel") {
            xi::mp::Element v; if (r.next(v) != xi::mp::Status::Ok || v.kind != xi::mp::Kind::Str) return out;
            out.channel = as_str(v);
        } else if (key == "frame") {
            xi::mp::Element fm; if (r.next(fm) != xi::mp::Status::Ok || fm.kind != xi::mp::Kind::Map) return out;
            for (uint32_t j = 0; j < fm.len; ++j) {
                xi::mp::Element fk;
                if (r.next(fk) != xi::mp::Status::Ok || fk.kind != xi::mp::Kind::Str) return out;
                std::string fkey = as_str(fk);
                if (fkey == "count")      { xi::mp::Element v; r.next(v); out.has_count = true; out.count = v.i; }
                else if (fkey == "score") { xi::mp::Element v; r.next(v); out.has_score = true; out.score = v.d; }
                else if (fkey == "label") { xi::mp::Element v; r.next(v); out.has_label = true; out.label = as_str(v); }
                else if (fkey == "frame") {  // image descriptor {w,h,c,px}
                    xi::mp::Element im;
                    if (r.next(im) != xi::mp::Status::Ok || im.kind != xi::mp::Kind::Map) return out;
                    out.has_image = true;
                    for (uint32_t m = 0; m < im.len; ++m) {
                        xi::mp::Element ik; r.next(ik); std::string ikey = as_str(ik);
                        xi::mp::Element iv; r.next(iv);
                        if (ikey == "w") out.iw = iv.i;
                        else if (ikey == "h") out.ih = iv.i;
                        else if (ikey == "c") out.ic = iv.i;
                        else if (ikey == "px") out.px.assign(iv.data, iv.data + iv.len);
                    }
                } else if (!skip_value(r)) return out;   // opaque entry (e.g. mp)
            }
        } else if (!skip_value(r)) return out;
    }
    out.ok = true;
    return out;
}

int main() {
    std::printf("[test] expose frame door (generic walk -> XEX1-v2)\n");

    xi::install_frame_abi();
    xi_host_api host = xi::ImagePool::make_host_api();
    const xi_frame_v1* fi = xi::frame_v1_iface();
    xi::binary_sink() = &capture_binary;

    const int base_live = xi::ImagePool::instance().cumulative().live_now;

    HMODULE dll = LoadLibraryA(EXPOSE_DLL_PATH);
    if (!dll) { std::fprintf(stderr, "FAIL: LoadLibrary(%s) err %lu\n", EXPOSE_DLL_PATH, GetLastError()); return 1; }
    auto factory = reinterpret_cast<xi::PluginInfo::CFactoryFn>(GetProcAddress(dll, "xi_plugin_create"));
    auto get_iface = reinterpret_cast<xi_plugin_get_interface_fn>(GetProcAddress(dll, "xi_plugin_get_interface"));
    CHECK(factory != nullptr);
    CHECK(get_iface != nullptr);
    if (!factory) return 1;
    void* inst = factory(&host, "sink0");
    CHECK(inst != nullptr);
    if (!inst) return 1;
    auto expose = std::make_unique<xi::CAbiInstanceAdapter>("sink0", "sink0", dll, inst);

    // ---------------------------------------------------------------------
    // (A) Door probe: expose (a sink) publishes xi.frame@1.
    // ---------------------------------------------------------------------
    SECTION("door probe: expose answers xi.frame@1 (it consumes frames)");
    CHECK(get_iface && get_iface("xi.frame", 1) != nullptr);
    CHECK(expose->has_frame_door());

    // Build one input frame: reserved $channel/$seq + a generic entry mix.
    std::vector<uint8_t> pixels = {10, 20, 30, 40};   // 2x2x1 gray
    auto build_input = [&]() -> xi_frame_handle {
        xi_frame_builder b = fi->builder_new();
        fi->builder_add_str(b, "$channel", "cam0", 4);
        fi->builder_add_i64(b, "$seq", 7);
        fi->builder_add_i64(b, "count", 42);
        fi->builder_add_f64(b, "score", 1.5);
        fi->builder_add_str(b, "label", "ok", 2);
        fi->builder_add_image(b, "frame", 2, 2, 1, pixels.data());
        return fi->builder_seal(b);
    };

    // ---------------------------------------------------------------------
    // (B) v2 opt-in: subscribed channel -> a decodable XEX1-v2 canonical dump.
    // ---------------------------------------------------------------------
    SECTION("v2 opt-in: generic walk emits the canonical frame dump");
    CHECK(expose->set_def("{\"frame_wire_v2\":true}"));
    expose->exchange("{\"command\":\"subscribe\",\"channels\":[\"cam0\"]}");
    g_emitted.clear();
    {
        xi_frame_handle in = build_input();
        xi_frame_handle ack = expose->process_frame_door(in);
        CHECK(ack != XI_FRAME_NULL);
        if (ack) {
            const char* c = nullptr; int32_t cl = 0;
            CHECK(fi->get_str(ack, "channel", &c, &cl) == 1 && std::string(c, cl) == "cam0");
            int64_t seen = -1;
            CHECK(fi->get_i64(ack, "seen", &seen) == 1 && seen == 1);
            fi->release(ack);
        }
        fi->release(in);

        CHECK(g_emitted.size() == 1);
        if (g_emitted.size() == 1) {
            V2View d = decode_v2(g_emitted[0]);
            CHECK(d.ok);
            CHECK(d.v == 2);
            CHECK(d.channel == "cam0");      // lifted from the $channel entry
            CHECK(d.seq == 7);               // lifted from the $seq entry
            CHECK(d.has_count && d.count == 42);
            CHECK(d.has_score && d.score == 1.5);
            CHECK(d.has_label && d.label == "ok");
            CHECK(d.has_image && d.iw == 2 && d.ih == 2 && d.ic == 1);
            CHECK(d.px == pixels);           // pixels inlined as bin, byte-for-byte
        }
    }

    // ---------------------------------------------------------------------
    // (C) v1 default: without the opt-in, the same walk emits a v1 frame.
    // ---------------------------------------------------------------------
    SECTION("v1 default: opt-out emits the legacy display frame (v:1)");
    CHECK(expose->set_def("{\"frame_wire_v2\":false}"));
    g_emitted.clear();
    {
        xi_frame_handle in = build_input();
        xi_frame_handle ack = expose->process_frame_door(in);
        if (ack) fi->release(ack);
        fi->release(in);
        CHECK(g_emitted.size() == 1);
        if (g_emitted.size() == 1) {
            const auto& w = g_emitted[0];
            CHECK(w.size() > 5 && w[0] == 'X' && w[3] == '1');
            // v1 body is a fixmap; its first key is "v" -> 1. Just confirm the
            // magic + that the canonical decoder does NOT see a v2 marker.
            V2View d = decode_v2(w);
            CHECK(d.v == 1);
        }
    }

    // ---------------------------------------------------------------------
    // (D) Subscription gating: an unsubscribed channel emits nothing.
    // ---------------------------------------------------------------------
    SECTION("gating: unsubscribed channel pushes no binary frame");
    expose->exchange("{\"command\":\"unsubscribe\",\"channels\":[\"cam0\"]}");
    g_emitted.clear();
    {
        xi_frame_handle in = build_input();
        xi_frame_handle ack = expose->process_frame_door(in);
        if (ack) fi->release(ack);
        fi->release(in);
        CHECK(g_emitted.empty());
    }

    // ---- teardown + pool balance ----
    xi::binary_sink() = nullptr;
    expose.reset();          // ~CAbiInstanceAdapter -> xi_plugin_destroy + leak sweep
    FreeLibrary(dll);

    CHECK(xi::ImagePool::instance().cumulative().live_now == base_live);
    CHECK(xi::FrameRegistry::instance().live_frames() == 0);

    if (g_failures == 0) { std::printf("\nALL TESTS PASSED\n"); return 0; }
    std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
    return 1;
}
