#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

// Stage ONE record to TWO sink instances. Both use() calls share_out the SAME
// underlying doc, so both staged items point at one doc — the case the host's
// staged flush must NOT double-stamp $seq into.
XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    xi::Record rec;
    rec.set("measurement", frame);
    xi::use("sinkA").process(rec);
    xi::use("sinkB").process(rec);
}
