//
// record_replay.cpp — the replay SOURCE (doc 10 gate P3): reads canonical
// .xex1 (XEX1-v3) dumps from a directory and emits each one back into the
// graph as a SEALED PACK through the host xi.pack@1 door — record_save's
// mirror, closing the record -> save -> replay loop.
//
//   record_save (sink):   sealed pack --shared dump--> <base>.xex1
//   record_replay (source): <base>.xex1 --shared parse--> sealed pack -> emit
//
// The file is parsed by xex1_pack_parse.hpp — the SAME untrusted-disk edge the
// host-side loader uses (magic/version gate, ingress canonicalization, tagged
// entry walk with tag/value agreement enforced). This plugin only REBUILDS:
// every parsed entry goes through the host builder (PackOut), because a plugin
// must not mint host pool handles / packs itself — that is exactly why parse
// and build are split (see xex1_pack_parse.hpp banner).
//
// Fidelity: the reserved $channel/$seq entries are restored FIRST (from the
// frame header the sink lifted them into), then every frame entry in wire
// order. Re-dumping the emitted pack through encode_pack_v3 reproduces the
// file bytes EXACTLY (the parity proof lives in record_replay_pack_test.cpp).
// Copies: file bytes -> pack arena / image pool is ONE unavoidable ingress
// copy; after seal() the pack flows through dispatch zero-copy (handle only).
//
// Driving model (mirrors json_source): each process() call replays the NEXT
// file (lexicographic order) and returns a small ack Record — the payload
// rides the pack plane, not the Record. A parse failure emits a sealed $fault
// pack (fail loud, the pilot convention) and the ack carries the reason; the
// cursor still advances so one bad file cannot wedge a loop.
//
// Config (set_def / UI):
//   - dir:     directory scanned for *.xex1 (non-recursive)
//   - loop:    wrap to the first file after the last (default false)
//   - enabled: gate (default true)
// Exchange commands: {"command":"rewind"} rescans + restarts; get_status.
//

#include <xi/xi_abi.hpp>
#include "yyjson.h"                // def/command parsing, like record_save
#include "xex1_pack_parse.hpp"     // xi::xex1::parse_frame_v3_file — the shared parse

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

class RecordReplay : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    // One process() call = one file replayed (the ack Record is bookkeeping;
    // the payload is the emitted pack).
    xi::Record process(const xi::Record& /*input*/) override {
        xi::Record ack;
        if (!enabled_)   { ack.set("replayed", false); ack.set("reason", "disabled"); return ack; }
        if (dir_.empty()) { ack.set("replayed", false); ack.set("reason", "no dir set"); return ack; }
        if (!pack_iface()) {
            // No pack plane, no replay: this source is pack-native by design.
            ack.set("replayed", false);
            ack.set("reason", "no pack plane (host does not publish xi.pack@1)");
            return ack;
        }

        if (!scanned_) rescan_();
        if (position_ >= files_.size()) {
            if (loop_ && !files_.empty()) position_ = 0;
            else {
                ack.set("replayed", false);
                ack.set("reason", files_.empty() ? "no .xex1 files in dir" : "end of files");
                ack.set("total", (int64_t)files_.size());
                return ack;
            }
        }

        const std::string path = files_[position_];
        const size_t pos = ++position_;   // 1-based position of the file just consumed

        xi::xex1::ParsedFrame pf = xi::xex1::parse_frame_v3_file(path);
        if (!pf.ok) {
            // Fail loud on the pack plane too (pilot convention): the consumer
            // gets a sealed $fault pack to route to a verdict. The cursor has
            // advanced, so a corrupt file cannot wedge the sequence.
            xi::PackOut f = new_pack();
            if (f.valid()) {
                f.str("file", std::filesystem::path(path).filename().generic_string());
                f.fault("bad_replay_file", "file", pf.error);
                emit(std::move(f));
            }
            ack.set("replayed", false);
            ack.set("file", std::filesystem::path(path).filename().generic_string());
            ack.set("error", pf.error);
            ack.set("position", (int64_t)pos);
            ack.set("total", (int64_t)files_.size());
            return ack;
        }

        xi::PackOut f = new_pack();
        if (!f.valid()) { ack.set("replayed", false); ack.set("reason", "pack builder unavailable"); return ack; }

        // Restore the reserved entries the sink LIFTED into the frame header,
        // first — so a pack that carried them round-trips entry-for-entry.
        f.str(xi::Record::kChannelKey, pf.channel);
        f.i64(xi::Record::kSeqKey, (int64_t)pf.seq);

        // Rebuild every frame entry in wire order, typed by its on-wire tag.
        for (const xi::xex1::ParsedEntry& e : pf.entries) {
            const uint8_t* vp = pf.at(e.off);
            const size_t   vn = e.len;
            switch (e.tag) {
                case XI_PACK_TAG_I64: {
                    xi::mp::Reader r(vp, vn); xi::mp::Element v; r.next(v);
                    f.i64(e.key.c_str(), v.kind == xi::mp::Kind::UInt ? (int64_t)v.u : v.i);
                    break;
                }
                case XI_PACK_TAG_F64: {
                    xi::mp::Reader r(vp, vn); xi::mp::Element v; r.next(v);
                    f.f64(e.key.c_str(), v.d);
                    break;
                }
                case XI_PACK_TAG_STR: {
                    xi::mp::Reader r(vp, vn); xi::mp::Element v; r.next(v);
                    f.str(e.key.c_str(), std::string_view((const char*)v.data, v.len));
                    break;
                }
                case XI_PACK_TAG_BIN: {
                    xi::mp::Reader r(vp, vn); xi::mp::Element v; r.next(v);
                    f.bin(e.key.c_str(), v.data, v.len);
                    break;
                }
                case XI_PACK_TAG_MP:
                    f.mp(e.key.c_str(), vp, vn);   // canonical nested bytes, verbatim
                    break;
                case XI_PACK_TAG_IMAGE:
                    // Dims/px length already proven consistent by the parser.
                    f.image(e.key.c_str(), e.w, e.h, e.c, pf.at(e.px_off));
                    break;
                default: break;   // unreachable: the parser refused unknown tags
            }
        }
        emit(std::move(f));   // seal + hand to host dispatch (the graph's pack door)

        ack.set("replayed", true);
        ack.set("file", std::filesystem::path(path).filename().generic_string());
        ack.set("channel", pf.channel);
        ack.set("seq", (int64_t)pf.seq);
        ack.set("entries", (int64_t)pf.entries.size());
        ack.set("position", (int64_t)pos);
        ack.set("total", (int64_t)files_.size());
        return ack;
    }

    std::string exchange(const std::string& cmd) override {
        yyjson_doc* doc = yyjson_read(cmd.c_str(), cmd.size(), 0);
        yyjson_val* p = doc ? yyjson_doc_get_root(doc) : nullptr;
        if (!p) { yyjson_doc_free(doc); return get_def(); }
        yyjson_val* c = yyjson_obj_get(p, "command");
        yyjson_val* v = yyjson_obj_get(p, "value");
        if (c && yyjson_is_str(c)) {
            std::string command = yyjson_get_str(c);
            if (command == "rewind") {
                rescan_();
            } else if (command == "set_dir" && v && yyjson_is_str(v)) {
                dir_ = yyjson_get_str(v);
                rescan_();
            } else if (command == "set_loop" && v) {
                loop_ = yyjson_get_bool(v);
            } else if (command == "set_enabled" && v) {
                enabled_ = yyjson_get_bool(v);
            }
            // get_status falls through to get_def() below
        }
        yyjson_doc_free(doc);
        return get_def();
    }

    std::string get_def() const override {
        char buf[1024];
        std::snprintf(buf, sizeof(buf),
            R"({"dir":"%s","loop":%s,"enabled":%s,"position":%d,"total":%d})",
            escape_for_json(dir_).c_str(),
            loop_ ? "true" : "false",
            enabled_ ? "true" : "false",
            (int)position_, (int)files_.size());
        return buf;
    }

    bool set_def(const std::string& json) override {
        yyjson_doc* doc = yyjson_read(json.c_str(), json.size(), 0);
        yyjson_val* p = doc ? yyjson_doc_get_root(doc) : nullptr;
        if (!p) { yyjson_doc_free(doc); return false; }
        yyjson_val* d = yyjson_obj_get(p, "dir");
        if (d && yyjson_is_str(d)) {
            std::string nd = yyjson_get_str(d);
            if (nd != dir_) { dir_ = std::move(nd); scanned_ = false; position_ = 0; }
        }
        yyjson_val* lp = yyjson_obj_get(p, "loop");    if (lp) loop_ = yyjson_get_bool(lp);
        yyjson_val* en = yyjson_obj_get(p, "enabled"); if (en) enabled_ = yyjson_get_bool(en);
        yyjson_doc_free(doc);
        return true;
    }

private:
    // Scan dir_ for *.xex1 (non-recursive), lexicographic order, cursor to 0.
    void rescan_() {
        files_.clear();
        position_ = 0;
        scanned_ = true;
        std::error_code ec;
        for (const auto& de : std::filesystem::directory_iterator(dir_, ec)) {
            if (!de.is_regular_file(ec)) continue;
            if (de.path().extension() == ".xex1")
                files_.push_back(de.path().generic_string());
        }
        std::sort(files_.begin(), files_.end());
    }

    static std::string escape_for_json(const std::string& s) {
        std::string out;
        for (char c : s) {
            if (c == '"' || c == '\\') { out.push_back('\\'); }
            if (c == '\n') { out += "\\n"; continue; }
            out.push_back(c);
        }
        return out;
    }

    std::string dir_;
    bool enabled_ = true;
    bool loop_ = false;
    bool scanned_ = false;
    size_t position_ = 0;              // index of the NEXT file to replay
    std::vector<std::string> files_;   // sorted *.xex1 paths from the last scan
};

XI_PLUGIN_IMPL(RecordReplay)
// NOTE: no XI_PLUGIN_PACK_DOOR — record_replay is a SOURCE. It does not consume
// packs; it EMITS them through the host xi.pack@1 iface (new_pack + emit), the
// same path mock_camera / json_source / synced_stereo take in pack mode.
