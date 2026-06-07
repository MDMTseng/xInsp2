#include <xi/xi.hpp>
XI_SCRIPT_EXPORT void xi_inspect_entry(int){
    volatile double z = 0.0;
    double nan_v = z / z;     // NaN at runtime
    double inf_v = 1.0 / z;   // +Inf at runtime
    VAR(nan_val, nan_v);
    VAR(inf_val, inf_v);
    VAR(ok_val, 42.5);
}
