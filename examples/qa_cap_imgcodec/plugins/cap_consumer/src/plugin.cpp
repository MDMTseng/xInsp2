//
// cap_consumer — the QA consumer side of the capability plane pilot (docs/
// new_gen/14). Its xi.pack@1 door receives a script-built image pack, builds
// an encode REQUEST pack, and calls the "xi.jpeg.encode" capability BY NAME
// through the host forwarding funnel (get_interface "xi.cap"@1) — it never
// sees the providing imgcodec instance or its vtable. The returned JPEG is
// then round-tripped through "xi.image.decode" and everything the script
// needs to assert (bytes, funnel rcs, dedup counters, decode dims check)
// rides back in the door's output pack.
//
// Input pack:  image "gray" (any 1/3/4ch 8-bit), optional i64 "quality".
// Output pack: bin "jpeg", i64 "enc_rc" (funnel rc), i64 "cache_hit",
//              i64 "encodes" (the provider's lifetime encode counter — the
//              dedup proof), i64 "dec_rc", i64 "dec_ok" (decoded dims match).
//
#include <xi/xi_abi.hpp>

class CapConsumer : public xi::Plugin {
public:
    CapConsumer(const xi_host_api* host, const std::string& name)
        : xi::Plugin(host, name) {
        if (host && host->get_interface)
            cap_ = static_cast<const xi_cap_v1*>(host->get_interface("xi.cap", 1));
    }

    using xi::Plugin::process;   // keep the Record overload visible (pack-only override)

    void process(xi::PackIn& in, xi::PackOut& out) override {
        const xi_pack_v1* pk = pack_iface();
        if (!pk || !cap_) {
            out.fault("missing_capability", "xi.cap",
                      "cap_consumer: host publishes no capability plane");
            return;
        }
        auto img = in.image("gray");
        if (!img || !img->pixels) {
            out.fault("missing_input", "gray",
                      "cap_consumer: required image entry 'gray' is missing");
            return;
        }
        const int64_t q = in.i64_or("quality", 90);

        // ---- encode via the capability (host-forwarded, by name) ----------
        xi_pack_builder b = pk->builder_new();
        pk->builder_add_image(b, "image", img->width, img->height,
                              img->channels, img->pixels);
        pk->builder_add_i64(b, "quality", q);
        pk->builder_add_i64(b, "$v", 1);
        xi_pack_handle req = pk->builder_seal(b);
        xi_pack_handle rsp = XI_PACK_NULL;
        const int32_t enc_rc = cap_->call("xi.jpeg.encode", req, &rsp);
        pk->release(req);

        int64_t cache_hit = -1, encodes = -1;
        int32_t dec_rc = -99;
        int     dec_ok = 0;
        if (enc_rc == XI_CAP_OK && rsp != XI_PACK_NULL) {
            const void* jp = nullptr; int32_t jn = 0;
            if (pk->get_bin(rsp, "jpeg", &jp, &jn) && jp && jn > 0) {
                out.bin("jpeg", jp, (size_t)jn);
                // ---- decode round trip through the second capability ------
                xi_pack_builder db = pk->builder_new();
                pk->builder_add_bin(db, "data", jp, jn);
                pk->builder_add_i64(db, "$v", 1);
                xi_pack_handle dreq = pk->builder_seal(db);
                xi_pack_handle drsp = XI_PACK_NULL;
                dec_rc = cap_->call("xi.image.decode", dreq, &drsp);
                if (dec_rc == XI_CAP_OK && drsp != XI_PACK_NULL) {
                    xi_pack_image dimg{};
                    if (pk->get_image(drsp, "image", &dimg))
                        dec_ok = (dimg.width == img->width &&
                                  dimg.height == img->height) ? 1 : 0;
                    pk->release(drsp);
                }
                pk->release(dreq);
            }
            pk->get_i64(rsp, "cache_hit", &cache_hit);
            pk->get_i64(rsp, "encodes", &encodes);
            pk->release(rsp);
        }
        out.i64("enc_rc", enc_rc);
        out.i64("cache_hit", cache_hit);
        out.i64("encodes", encodes);
        out.i64("dec_rc", dec_rc);
        out.i64("dec_ok", dec_ok);
    }
private:
    const xi_cap_v1* cap_ = nullptr;
};

XI_PLUGIN_IMPL(CapConsumer)
XI_PLUGIN_PACK_DOOR(CapConsumer)
