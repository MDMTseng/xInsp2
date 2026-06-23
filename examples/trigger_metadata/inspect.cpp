// trigger_metadata — read the routing/context metadata a source attached to the
// trigger (ABI v5). meta_source emits each frame with {command, recipe, seq}
// via xi::emit_record; here we read it back with current_trigger().meta() — a
// zero-copy read-only view over the metadata doc the bus carried by pointer.
// No side-channel, no FIFO: the metadata is correlated to the frame by the bus.
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    auto t = xi::current_trigger();
    if (!t.is_active()) return;

    auto m = t.meta();                                  // borrowed metadata view
    VAR(meta_command, m["command"].as_string());        // routing key
    VAR(meta_recipe,  m["recipe"].as_int(-1));
    VAR(meta_seq,     m["seq"].as_int(-1));
    VAR(source,       t.primary_source());
}
