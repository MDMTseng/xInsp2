#pragma once
//
// xi.hpp — umbrella include for xInsp2 inspection routines.
//
// User scripts do:
//
//   #include <xi/xi.hpp>
//
//   void inspect(Image frame) { ... }
//
// This pulls in the async primitives, tunable params, and cross-frame state.
// It does NOT pull in xi_instance.hpp: reaching a HOST-registered instance is
// the normal path and uses xi::use("name") (xi_use.hpp). A SCRIPT-OWNED
// xi::Instance<T> is a distinct, advanced capability you opt into explicitly:
//
//   #include <xi/xi_instance.hpp>
//
// It is deliberately OpenCV-free: a routine that does no CV
// compiles with OpenCV OFF the include path. xInsp2's own op library
// (xi::ops::*) was removed; scripts and plugins that want cv:: opt in with
//
//   #include <xi/xi_cv.hpp>   // pulls OpenCV + xi::as_cv_mat / from_cv_mat
//
// and call cv:: directly on xi::as_cv_mat(img) / Image::create_in_pool().
// Script output goes through the `expose` plugin
// (xi::use("expose").process(rec)), not a core macro — the old
// VAR()/value-store path was removed in v9.
//

#include "xi_async.hpp"
#include "xi_parallel.hpp"
#include "xi_image.hpp"
#include "xi_io.hpp"
#include "xi_param.hpp"
#include "xi_kv.hpp"     // xi::kv() — cross-frame script state (doc 16)
