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
// This pulls in the async primitives, tunable params, and the instance
// registry — plus OpenCV for image operators. xInsp2's own op library
// (xi::ops::*) was removed; scripts and plugins call cv:: directly with
// xi::Image::as_cv_mat() / create_in_pool(). Script output goes through the
// `expose` plugin (xi::use("expose").process(rec)), not a core macro — the
// old VAR()/value-store path was removed in v9.
//

#include "xi_async.hpp"
#include "xi_parallel.hpp"
#include "xi_image.hpp"
#include "xi_io.hpp"
#include "xi_param.hpp"
#include "xi_instance.hpp"
#include "xi_state.hpp"

#include <opencv2/opencv.hpp>
