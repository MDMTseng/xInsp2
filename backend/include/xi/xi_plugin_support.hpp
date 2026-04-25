#pragma once
//
// xi_plugin_support.hpp — convenience header force-included by the
// project-plugin compile path (CompileMode::PluginDev / PluginExport).
//
// Pulls in the C ABI macros (XI_PLUGIN_IMPL, etc.) and the common
// image / op headers so plugin authors can write a plain class
// without remembering which xi/* headers to include — same DX as
// inspection scripts get from xi_script_support.hpp.
//
// Plugin authors still write their class and call:
//
//     XI_PLUGIN_IMPL(MyPlugin)
//
// at the bottom of the .cpp; everything else is provided here.
//
// Hand-written plugins that already manually include xi_abi.hpp etc.
// are unaffected — these headers are #pragma once and idempotent.
//

#include "xi_abi.hpp"
#include "xi_image.hpp"
#include "xi_ops.hpp"
#include "xi_record.hpp"
