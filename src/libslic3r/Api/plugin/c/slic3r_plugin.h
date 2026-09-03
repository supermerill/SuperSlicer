///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_plugin_h_
#define slic3r_plugin_h_

#define SLIC3R_PLUGIN_API_PLUGIN_MAJOR 1u
#define SLIC3R_PLUGIN_API_PLUGIN_MINOR 0u

#include <stdint.h>

#include "slic3r_orchestrator.h"

#if defined(_WIN32) && defined(SLIC3R_PLUGIN_EXPORTS)
#define SLIC3R_PLUGIN_API __declspec(dllexport)
#else
#define SLIC3R_PLUGIN_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ========================= PLUGIN ENTRY ========================= */

/*
Entry point symbol to export from plugin shared library.

This function MUST be implemented by the plugin.
The orchestrator will call it after loading the plugin.

The plugin have to register its plugin_instance(s) using
orchestrator_register_plugin().

After that, the orchestrator will be able to call the run method defined in the plugin_instance passed by the
orchestrator_register_plugin()
*/
SLIC3R_PLUGIN_API void register_plugin(orchestrator_handle *orch);

/*
Return the C header versions used to build this shared-library package.

Call with versions == NULL to obtain the number of entries. The returned
table is dense and indexed by slic3r_plugin_api_id. The package defines this
export by including slic3r_plugin_register_version.h once, after all public C
headers used by its entry translation unit. The host validates this export
before calling register_plugin(), so the plugin_vtable layout is known to be
compatible before any vtable callback is used.

Entry zero (PLUGIN_TYPES) is mandatory. A longer table is accepted if every
unknown entry is unused (0.0); shorter tables omit optional contracts only.
The export name is retained from the former scalar ABI protocol. Requiring
entry zero rejects legacy exports that leave the table untouched on supported
platforms, but calling that old signature is not language-level ABI safe.
*/
SLIC3R_PLUGIN_API int32_t slic3r_plugin_abi_version(
    slic3r_major_minor_version *versions);

#ifdef __cplusplus
}
#endif

#endif // slic3r_plugin_h_
