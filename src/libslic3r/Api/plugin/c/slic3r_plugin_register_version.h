///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifdef slic3r_plugin_register_version_h_
#error "Only Include slic3r_plugin_register_version.h once in your package, in a source file that contains all the headers used by your plugins."
#endif

#ifndef slic3r_plugin_register_version_h_
#define slic3r_plugin_register_version_h_

#include <stddef.h>

/*
Package API version export
==========================

Include this header exactly once in the shared-library entry translation unit,
after every public C API header used by the package. It defines the exported
slic3r_plugin_abi_version() function. The function reports every API
header visible at this point, including transitive dependencies, so a package
cannot accidentally use a newer header without declaring that requirement.

This is intentionally not included by slic3r_plugin.h or any API header. The
preprocessor must see the package's final include set before it evaluates the
conditional entries below.
*/

#ifndef slic3r_plugin_h_
#error "Include slic3r_plugin.h before slic3r_plugin_register_version.h."
#endif

#ifdef __cplusplus
extern "C" {
#endif

SLIC3R_PLUGIN_API int32_t slic3r_plugin_abi_version(
    slic3r_major_minor_version *versions)
{
    uint32_t idx;
    if (versions == NULL)
        return SLIC3R_PLUGIN_API_COUNT;

    for (idx = 0; idx < SLIC3R_PLUGIN_API_COUNT; ++idx) {
        versions[idx].major = 0;
        versions[idx].minor = 0;
    }

#define SLIC3R_PLUGIN_API_VERSION_ENTRY(name) \
    versions[SLIC3R_PLUGIN_API_##name].major = (uint16_t)SLIC3R_PLUGIN_API_##name##_MAJOR; \
    versions[SLIC3R_PLUGIN_API_##name].minor = (uint16_t)SLIC3R_PLUGIN_API_##name##_MINOR;
#include "slic3r_plugin_api_version_entries.inc"
#undef SLIC3R_PLUGIN_API_VERSION_ENTRY

    return SLIC3R_PLUGIN_API_COUNT;
}

#ifdef __cplusplus
}
#endif

#endif // slic3r_plugin_register_version_h_
