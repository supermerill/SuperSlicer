///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// This shared-library fixture exposes only the symbols needed to exercise the
// native package loader. CMake builds it with different definitions to create
// a library with incompatible header versions, missing exports and a
// registration exception.

#include <stdexcept>

#ifdef SLIC3R_TEST_PLUGIN_LEGACY_ABI
// Deliberately retain the old signature without including its new declaration.
#include <cstdint>
struct orchestrator_handle;
#ifdef _WIN32
#define SLIC3R_PLUGIN_API __declspec(dllexport)
#else
#define SLIC3R_PLUGIN_API
#endif
extern "C" SLIC3R_PLUGIN_API uint32_t slic3r_plugin_abi_version(void) { return 51u; }
#else
#include "libslic3r/Api/plugin/c/slic3r_plugin.h"
static_assert(SLIC3R_PLUGIN_API_PLUGIN_TYPES == 0, "Vtable version must be first.");

#if defined(SLIC3R_TEST_PLUGIN_BAD_API_MAJOR) || defined(SLIC3R_TEST_PLUGIN_BAD_API_MINOR) || \
    defined(SLIC3R_TEST_PLUGIN_SHORT_API_TABLE) || defined(SLIC3R_TEST_PLUGIN_LONG_API_TABLE) || \
    defined(SLIC3R_TEST_PLUGIN_UNKNOWN_API) || defined(SLIC3R_TEST_PLUGIN_ZERO_API_TABLE) || \
    defined(SLIC3R_TEST_PLUGIN_MISSING_VTABLE) || defined(SLIC3R_TEST_PLUGIN_INVALID_API_SIZE) || \
    defined(SLIC3R_TEST_PLUGIN_CHANGING_API_SIZE)
extern "C" SLIC3R_PLUGIN_API int32_t slic3r_plugin_abi_version(
    slic3r_major_minor_version *requirements)
{
    uint32_t count = SLIC3R_PLUGIN_API_COUNT;
#ifdef SLIC3R_TEST_PLUGIN_SHORT_API_TABLE
    count = 1;
#endif
#if defined(SLIC3R_TEST_PLUGIN_LONG_API_TABLE) || defined(SLIC3R_TEST_PLUGIN_UNKNOWN_API)
    count = SLIC3R_PLUGIN_API_COUNT + 1;
#endif
#ifdef SLIC3R_TEST_PLUGIN_INVALID_API_SIZE
    return 0;
#endif
    if (requirements == nullptr)
        return int32_t(count);

    for (uint32_t idx = 0; idx < count; ++idx) {
        requirements[idx].major = 0;
        requirements[idx].minor = 0;
    }

#if !defined(SLIC3R_TEST_PLUGIN_ZERO_API_TABLE) && !defined(SLIC3R_TEST_PLUGIN_MISSING_VTABLE)
    requirements[SLIC3R_PLUGIN_API_PLUGIN_TYPES].major = SLIC3R_PLUGIN_API_PLUGIN_TYPES_MAJOR;
    requirements[SLIC3R_PLUGIN_API_PLUGIN_TYPES].minor = SLIC3R_PLUGIN_API_PLUGIN_TYPES_MINOR;
#endif
#ifdef SLIC3R_TEST_PLUGIN_MISSING_VTABLE
    requirements[SLIC3R_PLUGIN_API_PLUGIN].major = SLIC3R_PLUGIN_API_PLUGIN_MAJOR;
#endif
#ifdef SLIC3R_TEST_PLUGIN_UNKNOWN_API
    requirements[SLIC3R_PLUGIN_API_COUNT].major = 1;
#endif
#ifdef SLIC3R_TEST_PLUGIN_BAD_API_MAJOR
    requirements[SLIC3R_PLUGIN_API_PLUGIN_TYPES].major = SLIC3R_PLUGIN_API_PLUGIN_TYPES_MAJOR + 1;
#endif
#ifdef SLIC3R_TEST_PLUGIN_BAD_API_MINOR
    requirements[SLIC3R_PLUGIN_API_PLUGIN_TYPES].minor = SLIC3R_PLUGIN_API_PLUGIN_TYPES_MINOR + 1;
#endif
#ifdef SLIC3R_TEST_PLUGIN_CHANGING_API_SIZE
    return int32_t(count - 1);
#endif
    return int32_t(count);
}
#elif !defined(SLIC3R_TEST_PLUGIN_MISSING_ABI)
#include "libslic3r/Api/plugin/c/slic3r_plugin_register_version.h"
#endif
#endif // SLIC3R_TEST_PLUGIN_LEGACY_ABI

#ifndef SLIC3R_TEST_PLUGIN_MISSING_REGISTER
extern "C" SLIC3R_PLUGIN_API void register_plugin(orchestrator_handle *)
{
#ifdef SLIC3R_TEST_PLUGIN_THROW_REGISTER
    throw std::runtime_error("Registration fixture failure.");
#endif
}
#endif
