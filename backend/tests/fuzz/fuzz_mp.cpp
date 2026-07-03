// fuzz_mp.cpp — coverage-guided fuzz target for the canonical msgpack reader.
//
// Boundary under test: xi::mp::Reader in xi/xi_mp.hpp — the BOUNDED, VALIDATING
// decoder that is the sole ingress path for the v3 pack plane. This is the code
// a malicious/broken foreign producer's bytes hit head-on: arbitrary input goes
// straight into validate() and canonicalize(), the exact adversary the edge must
// survive. The reader walks the full standard msgpack width families with its
// own big-endian length reads and recursive container descent — prime territory
// for OOB reads on truncated lengths, and unbounded recursion on a depth bomb.
//
// The reader is header-only and dependency-free, so this target links nothing.
// A crash here (ASan/UBSan) is a genuine finding in the codec itself — save the
// reproducer and REPORT. Header included READ-ONLY.
//
#include <cstddef>
#include <cstdint>

#include "xi/xi_mp.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // 1) Structural validation under the default (reject-ext) policy and bound.
    {
        xi::mp::Reader r(data, size);
        xi::mp::Status s = r.validate();
        (void)s;
    }

    // 2) Canonicalize under a permissive ext policy (accept a type + strip the
    //    rest), driving the re-encoder path — the read-any->write-canonical core.
    //    If the input validated, the canonical output MUST itself re-validate:
    //    that invariant is the whole point of the profile, so assert it here and
    //    let the fuzzer hunt for a counterexample.
    {
        xi::mp::ExtPolicy pol = xi::mp::ExtPolicy::accept_types({1, 2, 3});
        pol.on_foreign = xi::mp::ExtPolicy::OnForeign::StripToBin;

        xi::mp::Writer out;
        xi::mp::Reader r(data, size);
        if (r.canonicalize(out, xi::mp::kDefaultMaxDepth, pol) == xi::mp::Status::Ok) {
            const xi::mp::Bytes& c = out.bytes();
            if (xi::mp::validate(c.data(), c.size(), xi::mp::kDefaultMaxDepth, pol)
                    != xi::mp::Status::Ok) {
                __builtin_trap();  // canonical output failed to re-validate
            }
            // Idempotence: canonicalizing the canonical output reproduces it.
            xi::mp::Writer out2;
            if (xi::mp::Reader(c).canonicalize(out2, xi::mp::kDefaultMaxDepth, pol)
                    == xi::mp::Status::Ok &&
                out2.bytes() != c) {
                __builtin_trap();  // canonicalize is not idempotent
            }
        }
    }
    return 0;
}
