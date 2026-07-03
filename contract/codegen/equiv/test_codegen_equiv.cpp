//
// test_codegen_equiv.cpp -- the COMPILED half of the codegen equivalence proof
// (docs/new_gen/08 Wave 3; docs/new_gen/02 guard 4).
//
// This TU includes the GENERATED <plugin>_keys.gen.h / _io.gen.h / _schema.gen.h
// (regenerated into the build dir by the codegen custom command -- see
// backend/CMakeLists.txt) IN PLACE OF the hand-written headers, and drives the
// EXACT same builder/extractor call sites the plugins' own tests use
// (plugins/blob_analysis/tests/test_blob_analysis.cpp,
//  plugins/mock_camera/tests/test_mock_camera.cpp). If the generator ever emits
// a renamed/missing constant or a changed method signature, THIS FILE STOPS
// COMPILING -- that is the drop-in claim proven at compile time. The runtime
// asserts prove the generated builders/extractors move the same bytes.
//
// It is DLL-free on purpose (the plugin .cpp DLLs are a separate cmake project
// the gate's ctest run does not build): the output Records are reconstructed
// from JSON exactly as the plugin tests' run_process() does after the C-ABI hop,
// so the extractor path is identical. Image round-trip (the "binary"/"frame"
// image-bag slots) needs a live pool/host and is out of scope here, exactly as
// the blob happy-path test notes for its "binary" output.
//
// The Python half (contract/codegen/check_equiv.py, the codegen_equiv ctest)
// proves the OTHER direction: generated == hand-written == decl key sets.
//

#include <xi/xi_contract.hpp>
#include <xi/xi_json.hpp>
#include <xi/xi_record.hpp>
#include <xi/xi_test.hpp>

// The generated artifacts under test (parallel to the hand-written headers).
#include "blob_analysis_io.gen.h"
#include "blob_analysis_schema.gen.h"
#include "mock_camera_io.gen.h"
#include "mock_camera_schema.gen.h"
// The codegen-gap-#2 trio (reply extractors / param defaults / raw-JSON
// splicing / patch builder / frame composites), generated for the first time:
#include "config_swap_probe_io.gen.h"
#include "json_source_io.gen.h"
#include "synced_stereo_io.gen.h"
#include "synced_stereo_schema.gen.h"

#include <string>
#include <utility>

static xi::Record from_json(const std::string& s) {
    return xi::Record::from_json_bytes(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

// ===========================================================================
// blob_analysis (operator) -- Input builder + Output extractor, generated.
// ===========================================================================

// The happy-path call site, byte-for-byte from test_blob_analysis.cpp:
//   xi::blob::Input().threshold(100).min_area(2)  -> a Record whose JSON the
// plugin then reads. We assert the builder wrote exactly those keys + the stamp.
XI_TEST(gen_blob_input_builder_matches_call_site) {
    xi::Record in = xi::blob::Input().threshold(100).min_area(2);
    XI_EXPECT(in.get_int(xi::blob::keys::kThreshold) == 100);
    XI_EXPECT(in.get_int(xi::blob::keys::kMinArea) == 2);
    XI_EXPECT(in.get_int(xi::contract::kSchemaKey) == xi::blob::kSchemaVersion);
    // Every declared setter exists with the hand-written signature (compile proof).
    xi::Record all = xi::blob::Input().threshold(1).min_area(2).max_area(3).invert(true);
    XI_EXPECT(all.get_bool(xi::blob::keys::kInvert) == true);
    XI_EXPECT(all.get_int(xi::blob::keys::kMaxArea) == 3);
}

// A missing required input, read back through the GENERATED Output extractor --
// same assertion shape as blob_missing_gray_is_structured_failure.
XI_TEST(gen_blob_missing_input_is_structured_failure) {
    xi::blob::Output out{ xi::contract::missing_input(xi::blob::keys::kGray,
                                                      xi::contract::Type::Image) };
    XI_EXPECT(!out.ok());
    XI_EXPECT(out.fault_code() == xi::contract::kMissingInput);
    xi::Record fault = out.record().get_record(xi::contract::kFaultKey);
    XI_EXPECT(fault.get_string(xi::contract::kKey) == std::string(xi::blob::keys::kGray));
    XI_EXPECT(fault.get_string(xi::contract::kExpectedType) == "image");
}

XI_TEST(gen_blob_schema_skew_is_structured_failure) {
    xi::blob::Output out{ xi::contract::schema_mismatch(999, xi::blob::kSchemaVersion) };
    XI_EXPECT(!out.ok());
    XI_EXPECT(out.fault_code() == xi::contract::kSchemaMismatch);
    xi::Record fault = out.record().get_record(xi::contract::kFaultKey);
    XI_EXPECT(fault.get_int(xi::contract::kHeaderSchema) == 999);
    XI_EXPECT(fault.get_int(xi::contract::kPluginSchema) == xi::blob::kSchemaVersion);
}

// The flagship happy-path read, EVERY per-blob field incl. the nested contour
// polygon -- transcribed from blob_happy_path_via_builder_and_extractor, reading
// a reconstructed-from-JSON output Record exactly like run_process() returns.
XI_TEST(gen_blob_output_extractor_reads_every_field) {
    const std::string out_json = R"({
      "blob_count": 2, "threshold_used": 100,
      "blobs": [
        {"area": 30, "cx": 5.0, "cy": 5.0, "min_x": 2, "min_y": 2, "max_x": 8, "max_y": 8,
         "contour_points": 3, "contour": [{"x":2,"y":2},{"x":8,"y":2},{"x":8,"y":8}]},
        {"area": 90, "cx": 25.0, "cy": 25.0, "min_x": 20, "min_y": 20, "max_x": 30, "max_y": 30,
         "contour_points": 4, "contour": [{"x":20,"y":20},{"x":30,"y":20},{"x":30,"y":30},{"x":20,"y":30}]}
      ]})";
    xi::blob::Output out{ from_json(out_json) };

    XI_EXPECT(out.ok());
    XI_EXPECT(out.blob_count() == 2);
    XI_EXPECT(out.blob_count() == out.blobs_size());
    XI_EXPECT(out.threshold_used() == 100);

    for (int b = 0; b < out.blob_count(); ++b) {
        auto blob = out.blob(b);
        XI_EXPECT(blob.area() > 0);
        XI_EXPECT(blob.cx() >= blob.min_x() && blob.cx() <= blob.max_x());
        XI_EXPECT(blob.cy() >= blob.min_y() && blob.cy() <= blob.max_y());
        XI_EXPECT(blob.min_x() <= blob.max_x());
        XI_EXPECT(blob.min_y() <= blob.max_y());

        XI_EXPECT(blob.contour_size() > 0);
        XI_EXPECT(blob.contour_size() == blob.contour_points());
        for (int i = 0; i < blob.contour_size(); ++i) {
            auto pt = blob.contour(i);
            XI_EXPECT(pt.x() >= blob.min_x() && pt.x() <= blob.max_x());
            XI_EXPECT(pt.y() >= blob.min_y() && pt.y() <= blob.max_y());
        }
    }
}

// The generated PackSchema keyset (offset-accessor frame path). slot_of is a
// compile-time constant, so these are static_asserts -- a renamed slot is a
// build error, exactly the keyset-drift guard xi_pack.hpp promises.
namespace {
using BS = xi::blob::BlobAnalysisSchema;
static_assert(BS::slot_count() == 8, "generated blob schema slot count");
static_assert(BS::slot_of("threshold") == BS::kThreshold, "slot_of maps declared key");
static_assert(BS::slot_of("blob_count") == BS::kBlobCount, "slot_of maps declared key");
static_assert(BS::slot_of("not_a_key") == -1, "slot_of rejects undeclared key");
}  // namespace

XI_TEST(gen_blob_typed_frame_round_trips) {
    xi::TypedPackBuilder<BS> b;
    b.set_i64<BS::kBlobCount>(2);
    b.set_i64<BS::kThresholdUsed>(100);
    xi::TypedPack<BS> f = b.seal();
    XI_EXPECT(f.get_i64<BS::kBlobCount>().value() == 2);
    XI_EXPECT(f.get_i64<BS::kThresholdUsed>().value() == 100);
    XI_EXPECT(!f.has<BS::kThreshold>());   // declared but unset
}

// ===========================================================================
// mock_camera (source) -- Config / Command / Frame, generated.
// ===========================================================================

XI_TEST(gen_mock_camera_config_and_command_match_call_sites) {
    // Same call site as mock_camera_happy_path_via_config_and_command.
    std::string cfg = xi::mock_camera::Config().width(320).height(240).fps(20);
    auto j = xi::Json::parse(cfg);
    XI_EXPECT(j[xi::mock_camera::keys::kWidth].as_int() == 320);
    XI_EXPECT(j[xi::mock_camera::keys::kHeight].as_int() == 240);
    XI_EXPECT(j[xi::mock_camera::keys::kFps].as_int() == 20);
    XI_EXPECT(j[xi::contract::kSchemaKey].as_int() == xi::mock_camera::kSchemaVersion);

    auto fps = xi::Json::parse(xi::mock_camera::Command::set_fps(25));
    XI_EXPECT(fps[xi::mock_camera::keys::kCommand].as_string() == std::string(xi::mock_camera::keys::kSetFps));
    XI_EXPECT(fps[xi::mock_camera::keys::kValue].as_int() == 25);

    auto res = xi::Json::parse(xi::mock_camera::Command::set_resolution(64, 48));
    XI_EXPECT(res[xi::mock_camera::keys::kCommand].as_string() == std::string(xi::mock_camera::keys::kSetResolution));
    XI_EXPECT(res[xi::mock_camera::keys::kWidth].as_int() == 64);
    XI_EXPECT(res[xi::mock_camera::keys::kHeight].as_int() == 48);

    auto start = xi::Json::parse(xi::mock_camera::Command::start());
    XI_EXPECT(start[xi::mock_camera::keys::kCommand].as_string() == std::string(xi::mock_camera::keys::kStart));
}

// The Frame extractor accessors exist and behave (image round-trip needs a live
// pool, so this checks the empty-frame verdict -- enough to prove the API).
XI_TEST(gen_mock_camera_frame_extractor_accessors_exist) {
    xi::mock_camera::Frame f{ xi::Record() };
    XI_EXPECT(!f.has_frame());
}

namespace {
using MS = xi::mock_camera::MockCameraSchema;
static_assert(MS::slot_count() == 3, "generated mock_camera schema slot count (frame + seq + gain)");
static_assert(MS::slot_of("frame") == MS::kFrame, "slot_of maps the frame key");
static_assert(MS::slot_of("seq") == MS::kSeq, "slot_of maps the pack-mode seq entry");
static_assert(MS::slot_of("gain") == MS::kGain, "slot_of maps the feedback-loop gain echo");
}  // namespace

// ===========================================================================
// config_swap_probe (source) -- Config / Command / Status REPLY extractor.
// The reply-extractor family (codegen gap #2): a typed reader over the
// exchange() reply STRING, exactly the call sites test_config_swap_probe.cpp
// drives (Status{ send_cmd(inst, Command::get_status()) }).
// ===========================================================================

XI_TEST(gen_csp_config_and_command_match_call_sites) {
    std::string cfg = xi::config_swap_probe::Config().value(42);
    auto j = xi::Json::parse(cfg);
    XI_EXPECT(j[xi::config_swap_probe::keys::kValue].as_int() == 42);
    XI_EXPECT(j[xi::contract::kSchemaKey].as_int() == xi::config_swap_probe::kSchemaVersion);

    auto cmd = xi::Json::parse(xi::config_swap_probe::Command::get_status());
    XI_EXPECT(cmd[xi::config_swap_probe::keys::kCommand].as_string()
              == std::string(xi::config_swap_probe::keys::kGetStatus));
}

XI_TEST(gen_csp_status_reply_extractor_reads_every_field) {
    // The exact reply shape the plugin's get_status emits (see the plugin test's
    // staged/committed assertions: s.active(), s.has_staged(), s.staged_value()).
    xi::config_swap_probe::Status s{
        R"({"active":42,"staged":true,"staged_value":99,"last_seen":7,"proc":3})" };
    XI_EXPECT(s.valid());
    XI_EXPECT(s.active() == 42);
    XI_EXPECT(s.has_staged());
    XI_EXPECT(s.staged_value() == 99);
    XI_EXPECT(s.last_seen() == 7);
    XI_EXPECT(s.proc() == 3);  // the whitelisted (long long) over as_double()
    long long p = s.proc();    // compile proof: the cast changed the return type
    XI_EXPECT(p == 3);

    xi::config_swap_probe::Status none{ R"({"active":1,"staged":false})" };
    XI_EXPECT(!none.has_staged());
    XI_EXPECT(xi::config_swap_probe::Status{ "not json" }.valid() == false);
}

// ===========================================================================
// json_source (source) -- raw-JSON Config splice, raw command param, Patch.
// The open-value family: builders CONCATENATE pre-serialized JSON text (that
// openness is the plugin's whole point), byte-for-byte what the hand-written
// json_source_io.h produced and test_json_source.cpp sends.
// ===========================================================================

XI_TEST(gen_json_source_config_splices_raw_json) {
    // Same call site as json_source_config_wrapper_and_reset: the data payload
    // rides UNQUOTED (raw), the schema stamp leads.
    std::string cfg = xi::json_source::Config().data(R"({"tag":"keep"})");
    XI_EXPECT(cfg == R"({"_schema":1,"data":{"tag":"keep"}})");
    // Default document is {} (the defaulted raw member).
    XI_EXPECT(xi::json_source::Config().dump() == R"({"_schema":1,"data":{}})");
}

XI_TEST(gen_json_source_commands_match_call_sites) {
    // set_data carries arbitrary JSON text spliced raw under "value".
    XI_EXPECT(xi::json_source::Command::set_data(R"({"n":5})")
              == R"({"command":"set_data","value":{"n":5}})");
    auto reset = xi::Json::parse(xi::json_source::Command::reset());
    XI_EXPECT(reset[xi::json_source::keys::kCommand].as_string()
              == std::string(xi::json_source::keys::kReset));
    auto status = xi::Json::parse(xi::json_source::Command::get_status());
    XI_EXPECT(status[xi::json_source::keys::kCommand].as_string()
              == std::string(xi::json_source::keys::kGetStatus));
}

XI_TEST(gen_json_source_patch_builder_single_and_batch) {
    // Same call site as json_source_process_patch: Patch::single(".n", "7").
    XI_EXPECT(xi::json_source::Patch::single(".n", "7")
              == R"({"key":".n","value":7})");
    XI_EXPECT(xi::json_source::Patch::batch({ {".n", "7"}, {".tag", "\"hi\""} })
              == R"({"patches":[{"key":".n","value":7},{"key":".tag","value":"hi"}]})");
}

// ===========================================================================
// synced_stereo (source) -- defaulted fire(), composite has_both().
// ===========================================================================

XI_TEST(gen_synced_stereo_config_and_command_match_call_sites) {
    std::string cfg = xi::synced_stereo::Config().fps(30);
    auto j = xi::Json::parse(cfg);
    XI_EXPECT(j[xi::synced_stereo::keys::kFps].as_int() == 30);
    XI_EXPECT(j[xi::contract::kSchemaKey].as_int() == xi::synced_stereo::kSchemaVersion);

    auto fps = xi::Json::parse(xi::synced_stereo::Command::set_fps(25));
    XI_EXPECT(fps[xi::synced_stereo::keys::kCommand].as_string()
              == std::string(xi::synced_stereo::keys::kSetFps));
    XI_EXPECT(fps[xi::synced_stereo::keys::kValue].as_int() == 25);

    // fire(int n = 1): the DEFAULTED param (codegen gap #2). fire() == fire(1)
    // is the hand-written contract, proven on the produced bytes.
    XI_EXPECT(xi::synced_stereo::Command::fire() == xi::synced_stereo::Command::fire(1));
    auto fire4 = xi::Json::parse(xi::synced_stereo::Command::fire(4));
    XI_EXPECT(fire4[xi::synced_stereo::keys::kCommand].as_string()
              == std::string(xi::synced_stereo::keys::kFire));
    XI_EXPECT(fire4[xi::synced_stereo::keys::kN].as_int() == 4);

    auto start = xi::Json::parse(xi::synced_stereo::Command::start());
    XI_EXPECT(start[xi::synced_stereo::keys::kCommand].as_string()
              == std::string(xi::synced_stereo::keys::kStart));
    auto stop = xi::Json::parse(xi::synced_stereo::Command::stop());
    XI_EXPECT(stop[xi::synced_stereo::keys::kCommand].as_string()
              == std::string(xi::synced_stereo::keys::kStop));
}

// The composite has_both() (frame_composites family) and the per-image
// accessors exist and behave; an image round-trip needs a live pool, so the
// empty-frame verdict is the API proof, exactly like mock_camera's.
XI_TEST(gen_synced_stereo_frame_extractor_composite_has_both) {
    xi::synced_stereo::Frame f{ xi::Record() };
    XI_EXPECT(!f.has_left());
    XI_EXPECT(!f.has_right());
    XI_EXPECT(!f.has_both());
}

namespace {
using SS = xi::synced_stereo::SyncedStereoSchema;
static_assert(SS::slot_count() == 3, "generated synced_stereo schema slot count (left+right+seq)");
static_assert(SS::slot_of("left") == SS::kLeft, "slot_of maps the left image key");
static_assert(SS::slot_of("right") == SS::kRight, "slot_of maps the right image key");
static_assert(SS::slot_of("seq") == SS::kSeq, "slot_of maps the pack-mode seq entry");
}  // namespace

int main() {
    auto results = xi::test::run_all();
    for (auto& r : results) if (!r.passed) return 1;
    return 0;
}
