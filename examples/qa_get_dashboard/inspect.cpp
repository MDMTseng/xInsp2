#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
XI_SCRIPT_EXPORT void xi_inspect_entry(int){ xi::use("expose").process(xi::Record().set("$channel","qa").set("ok",true)); }
