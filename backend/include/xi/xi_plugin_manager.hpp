#pragma once
//
// xi_plugin_manager.hpp — discovers, loads, and manages plugins.
//
// A plugin is a folder under the plugins/ directory containing:
//   plugin.json  — manifest (name, description, factory symbol)
//   <name>.dll   — shared library exporting a factory function
//   ui/          — optional web UI bundle (index.html + assets)
//
// Plugin manifest format (plugin.json):
// {
//   "name":        "mock_camera",
//   "description": "Simulated camera for testing",
//   "dll":         "xi-mock_camera.dll",
//   "factory":     "xi_plugin_create",    // exported C function
//   "has_ui":      true
// }
//
// Factory signature:
//   extern "C" __declspec(dllexport)
//   xi::InstanceBase* xi_plugin_create(const char* instance_name);
//
// The returned object is owned by the caller (the PluginManager wraps
// it in a shared_ptr).
//
// --- Header layout (mechanical split, 2026-07) ------------------------------
// This header was a single ~2787-line file. To reduce merge contention (so
// future bug-fixes in different concerns land in different files) it is now a
// thin UMBRELLA over cohesive sub-headers, keeping the SAME header-only
// distribution model — every existing `#include <xi/xi_plugin_manager.hpp>`
// sees the exact same xi::PluginManager symbol as before.
//
//   xi_pm_manager_core.hpp — the PluginManager class declaration: nested types,
//                            data members, trivial inline accessors, and the
//                            member-function DECLARATIONS. Include this alone if
//                            you only need the type shell.
//   xi_pm_discovery.hpp    — scan / certify / register / resolve externals.
//   xi_pm_load.hpp         — DLL load / compile / hot-recompile / cmake rebuild
//                            / export / lookup.
//   xi_pm_project.hpp      — create/close/open project + working-copy commits.
//   xi_pm_instances.hpp    — instance CRUD, lifecycle state, JSON persistence.
//
// The concern headers hold out-of-class `inline PluginManager::method(...)`
// definitions; each #includes the core header, so it also compiles standalone.
// This umbrella includes the core first, then the four concern headers (order
// below), so the full class + all definitions are visible to any consumer.
//
// This split is BEHAVIOUR-PRESERVING: code was only MOVED between headers (and
// method bodies out of the class body). No logic, signature, ABI, or wire
// change; still header-only (no new .cpp, no CMake target changes).
//

#include "xi_pm_manager_core.hpp"   // class declaration (must come first)
#include "xi_pm_discovery.hpp"      // scan / certify / register / resolve
#include "xi_pm_load.hpp"           // load / compile / recompile / rebuild / export
#include "xi_pm_project.hpp"        // create / close / open project + working copy
#include "xi_pm_instances.hpp"      // instance CRUD / state / JSON persistence
