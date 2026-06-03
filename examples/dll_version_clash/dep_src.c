/* The shared dependency. Built twice with /D VER=1 and /D VER=2 into the two
 * plugin folders (see run_experiment.py). dep_version() reports which build it is
 * so we can see which copy actually got loaded into the process. */
#ifndef VER
#define VER 0
#endif
__declspec(dllexport) int dep_version(void) { return VER; }
