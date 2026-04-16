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
// This pulls in the async primitives, the value-tracking macro, tunable
// params, and the instance registry. Plus the op library once it exists
// (expected layer above this header, not shipped yet).
//

#include "xi_async.hpp"
#include "xi_image.hpp"
#include "xi_var.hpp"
#include "xi_param.hpp"
#include "xi_instance.hpp"
#include "xi_state.hpp"
