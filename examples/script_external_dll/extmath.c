/* The external "SDK". Built by run_experiment.py into extmath.dll (+ extmath.lib)
 * in the project folder. The script links extmath.lib (project.json link_libs) and
 * loads extmath.dll from the project folder at runtime (project dir is on the DLL
 * search path). */
__declspec(dllexport) int ext_add(int a, int b) { return a + b; }
