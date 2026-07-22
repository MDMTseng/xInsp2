// record_save example — the ack is the receipt, and `enabled` is a real gate.
//
// record_save takes ONE sealed pack and lays it down as ONE `.xex1` file. What
// it hands back is not a status flag you hope about — it is a receipt:
//
//     saved=1 count=3 base=cap_000003.xex1 bytes=2436
//
// `count` is the sink's own capture counter (it feeds the `{count}` token in
// naming_rule, so `base` tells you the exact filename that now exists), and
// `bytes` is the length actually written. Nothing advances unless the write
// succeeded: a full disk or an unsafe naming_rule comes back saved=0 with a
// reason, and the counter stays where it was. There is no separate "did it
// work?" channel to poll.
//
// The other half of the lesson is the gate. With `enabled: false` the sink is
// honest about doing nothing — `saved=0 reason=disabled` — and no file appears.
// That is the difference between a recorder that is off and a recorder that is
// broken, and it is why this script maps the two to DIFFERENT verdicts:
//
//     saved            -> xi::ok(1)      the capture is on disk
//     reason=disabled  -> xi::result(0)  nothing to judge; the operator said no
//     anything else    -> xi::ng(1)      a real failure, surfaced loudly
//
// What lands in the file is exactly the pack we sealed here — the reserved
// $channel/$seq entries are lifted into the frame header, every other entry is
// written with its type tag. The bytes are produced by the SAME encoder the
// expose plugin pushes on the wire, so disk ≈ wire ≈ memory. That is what makes
// record_replay able to feed the file straight back in as a source (see
// toolbox/record_replay/example).
//
// Try it: `exchange_instance("rec", {"command":"set_enabled","value":false})`
// while the camera runs and watch the verdicts switch to "recording is off".
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>
#include <xi/xi_result.hpp>

#include <cstdio>
#include <string>

XI_INSPECT_ENTRY(t, frame) {
    (void)frame;
    if (!t.is_active()) return;          // the camera drives; nothing to capture

    auto tp = t.pack();
    if (!tp) return;
    const long long seq = (long long)tp.get_i64("seq").value_or(-1);
    auto img = tp.image_blob("frame");
    if (!img) { xi::ng(2, "camera pack carried no 'frame' image"); return; }

    // A checksum of the pixels we are about to persist. It rides IN the capture,
    // so the file is self-validating: anything that reads it back (the driver
    // here, record_replay later) can re-add the pixels and compare.
    long long psum = 0;
    for (uint8_t b : img->payload) psum += b;

    // A nested canonical-msgpack entry — provenance that travels with the frame.
    // Nested values survive the round trip verbatim; they are not flattened.
    xi::mp::Writer meta;
    meta.map(2);
    meta.key("origin");      meta.str("cam");
    meta.key("trigger_seq"); meta.int_(seq);

    // The capture pack. $channel/$seq are RESERVED: the sink lifts them out of
    // the entry list and into the file's frame header (and record_replay puts
    // them back), so they are how a saved file identifies itself.
    xi::ScriptPackBuilder b;
    bool built = b.valid();
    built = b.add_str("$channel", "cap") && built;
    built = b.add_i64("$seq", seq) && built;
    built = b.add_i64("seq", seq) && built;
    built = b.add_i64("psum", psum) && built;
    built = b.add_mp("meta", meta) && built;
    built = b.add_image_blob("frame", img->width, img->height, img->channels, "u8",
                             img->payload.data(),
                             (int64_t)img->payload.size()) && built;
    auto cap = b.seal();
    if (!(built && cap.valid())) { xi::ng(2, "could not build the capture pack"); return; }

    // ---- the sink door, and its receipt -------------------------------------
    auto ack = xi::use("rec").process(cap);
    if (!ack.valid() || ack.is_fault()) {
        xi::ng(2, std::string("record_save faulted: ")
                      .append(ack.fault_reason().value_or("no ack pack")).c_str());
        return;
    }
    const bool  saved  = ack.get_bool("saved").value_or(false);
    const auto  reason = ack.get_str("reason");
    const long long count = (long long)ack.get_i64("count").value_or(-1);
    const long long bytes = (long long)ack.get_i64("bytes").value_or(-1);
    const auto  base   = ack.get_str("base_name");

    // The SAME sealed pack also goes to the wire — one handle, two consumers,
    // zero pixel copies. What you see in the UI is what is in the file.
    xi::use("expose").push(cap);

    char msg[224];
    if (saved) {
        std::snprintf(msg, sizeof msg,
                      "save seq=%lld saved=1 count=%lld base=%.*s bytes=%lld psum=%lld",
                      seq, count, (int)(base ? base->size() : 0),
                      base ? base->data() : "", bytes, psum);
        // A receipt that claims success but names no file or no bytes is a bug,
        // not a save.
        if (count > 0 && bytes > 0 && base) xi::ok(1, msg);
        else                                xi::ng(1, msg);
        return;
    }

    std::snprintf(msg, sizeof msg, "save seq=%lld saved=0 reason=%.*s",
                  seq, (int)(reason ? reason->size() : 0),
                  reason ? reason->data() : "");
    if (reason && *reason == "disabled")
        xi::result(0, msg);              // switched off on purpose — not a defect
    else
        xi::ng(1, msg);                  // disk full, bad naming_rule, no dir...
}
