// record_replay example — a recorded run comes back as a REAL source.
//
// There is no camera in this project. `recorded/` holds five `.xex1` captures
// (produced by toolbox/record_save/example — the other end of this loop), and
// record_replay feeds them back into the graph one at a time. What arrives is
// not a "replayed frame" object with its own API: it is an ordinary sealed pack
// on an ordinary trigger, indistinguishable from a live grab except that
// `t.primary_source()` says "replay". That is the whole claim, and it is what
// makes a recorded run usable for tuning a recipe on your desk.
//
// The fidelity is exact, not approximate:
//
//   * the reserved $channel/$seq the sink lifted into the file header are put
//     BACK as entries — a capture round-trips entry-for-entry, no gains, no
//     losses (a file that never carried them replays without them);
//   * every other entry is rebuilt by its on-wire type tag, so the nested `meta`
//     map is still a map and the image is still an image;
//   * the pixels are the recorded pixels — this script re-adds them and compares
//     against the `psum` checksum that was sealed in at record time.
//
// record_replay is a PULL source: it emits when something ticks its door, and
// that emit arrives as the NEXT trigger. So a run here is one of two things and
// the script handles both:
//
//   timer run   (t.is_active() == false) -> pump the source once, no verdict
//   trigger run (t.is_active() == true)  -> a replayed capture; verify it
//
// The end of the file list is part of the contract too. With `loop: false` the
// source simply stops emitting: the pump keeps ticking, nothing more arrives,
// and `get_status` shows position == total. A replay that silently wrapped and
// re-fed you frame 1 would be far worse than one that stops.
//
// Try it: point the source at a live capture directory instead —
//   exchange_instance("replay", {"command":"set_dir",
//                                "value":"toolbox/record_save/example/captures"})
// then {"command":"rewind"} and it replays whatever record_save just wrote.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>
#include <xi/xi_result.hpp>

#include <cstdio>
#include <string>

XI_INSPECT_ENTRY(t, frame) {
    (void)frame;

    if (!t.is_active()) {
        // ---- the pump: one process() call = "advance one file" -------------
        // A one-entry pack is the verb; an EMPTY pack short-circuits host-side
        // and would never reach the door. Past the last file this is an honest
        // no-op — the source has nothing left to emit and says so by emitting
        // nothing.
        xi::ScriptPackBuilder pump;
        pump.add_i64("advance", 1);
        xi::use("replay").process(pump.seal());
        xi::result(0, "pumped replay — its emit arrives as the next trigger");
        return;
    }

    auto p = t.pack();
    if (!p) { xi::ng(2, "active trigger with no pack"); return; }

    // A corrupt / truncated / foreign file is not an exception: the source
    // emits a normal sealed pack carrying $fault, and its cursor still advances
    // so one bad file cannot wedge the replay. Check it first.
    if (p.is_fault()) {
        xi::ng(1, std::string("bad file on disk: ")
                      .append(p.fault_detail().value_or(
                              p.fault_reason().value_or("?"))).c_str());
        return;
    }

    // ---- what was restored --------------------------------------------------
    const std::string src = t.primary_source();
    const auto      chan  = p.get_str("$channel");        // lifted -> header -> back
    const long long hseq  = (long long)p.get_i64("$seq").value_or(-1);
    const long long seq   = (long long)p.get_i64("seq").value_or(-2);  // a normal entry
    const long long psum  = (long long)p.get_i64("psum").value_or(-1);
    const auto      meta  = p.get_mp("meta");             // nested map, verbatim
    auto            img   = p.image_blob("frame");

    int entries = 0;
    p.for_each([&](auto&&, auto&&) { ++entries; });

    // The checksum sealed in at record time, recomputed against the pixels that
    // came off disk. This is the byte-level half of "the replay is the frame".
    long long got = 0;
    if (img) for (uint8_t b : img->payload) got += b;

    const bool from_replay = (src == "replay");
    const bool reserved_ok = chan && *chan == "cap" && hseq >= 0 && seq == hseq;
    const bool pixels_ok   = img && psum >= 0 && got == psum;
    const bool meta_ok     = meta && !meta->empty();
    const bool count_ok    = entries == 6;   // $channel $seq seq psum meta frame

    // Re-surface it so the recorded frame is visible in the UI exactly as a live
    // one would be.
    xi::ScriptPackBuilder e;
    e.add_str("$channel", "replayed");
    e.add_i64("$seq", hseq);
    e.add_i64("seq", seq);
    e.add_i64("psum", psum);
    if (meta) e.add_mp("meta", *meta);
    if (img)  e.add_image_blob("frame", img->width, img->height, img->channels, "u8",
                               img->payload.data(), (int64_t)img->payload.size());
    xi::use("expose").push(e.seal());

    char msg[224];
    std::snprintf(msg, sizeof msg,
                  "replay seq=%lld src=%s chan=%d psum=%d(%lld) meta=%d entries=%d",
                  seq, src.c_str(), reserved_ok ? 1 : 0, pixels_ok ? 1 : 0, got,
                  meta_ok ? 1 : 0, entries);

    if (from_replay && reserved_ok && pixels_ok && meta_ok && count_ok) xi::ok(1, msg);
    else                                                                xi::ng(1, msg);
}
