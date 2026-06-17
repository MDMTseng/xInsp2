#pragma once
// mathx — a tiny stand-in for an "external dependency": a library that lives
// OUTSIDE the plugin and is linked in via the plugin's own CMakeLists. Swap this
// for a real vendor SDK (headers + .lib/.dll) and the wiring is identical.
namespace mathx {
double add(double a, double b);
}
