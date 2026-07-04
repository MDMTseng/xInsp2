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

// Note on operators: xInsp2 used to ship its own xi::ops library
// (gaussian, erode, threshold, ...). That has been removed; plugins
// call OpenCV directly via the free xi::as_cv_mat(img) (a non-owning
// view over pool memory) and xi::Image::create_in_pool(host(), w, h, c)
// for outputs. See docs/guides/write-a-plugin.md.
//
// This convenience header still hands plugin authors OpenCV — but via the
// opt-in <xi/xi_cv.hpp> (which brings both OpenCV and xi::as_cv_mat /
// xi::from_cv_mat). The mandatory umbrella xi.hpp is itself CV-free; only
// this plugin-dev force-include and xi_cv.hpp pull OpenCV in.

#include "xi_abi.hpp"
#include "xi_image.hpp"

#include "xi_cv.hpp"
