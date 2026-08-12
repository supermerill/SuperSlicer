///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// This shared-library fixture exposes only the symbols needed to exercise the
// native package loader. CMake builds it with different definitions to create
// an incompatible ABI library, missing exports and a registration exception.

#include <stdexcept>

#include "libslic3r/Api/plugin/c/slic3r_plugin.h"

#ifndef SLIC3R_TEST_PLUGIN_MISSING_ABI
#ifdef SLIC3R_TEST_PLUGIN_BAD_ABI
extern "C" SLIC3R_PLUGIN_API uint32_t slic3r_plugin_abi_version()
{
    return SLIC3R_PLUGIN_ABI_VERSION + 1;
}
#else
SLIC3R_PLUGIN_DECLARE_ABI_VERSION()
#endif
#endif

#ifndef SLIC3R_TEST_PLUGIN_MISSING_REGISTER
extern "C" SLIC3R_PLUGIN_API void register_plugin(orchestrator_handle *)
{
#ifdef SLIC3R_TEST_PLUGIN_THROW_REGISTER
    throw std::runtime_error("Registration fixture failure.");
#endif
}
#endif
