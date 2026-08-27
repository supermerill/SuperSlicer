///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_config_h_
#define slic3r_config_h_

#include <stdint.h>

#include "slic3r_config_option.h"
#include "slic3r_utils.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
Temporary dynamic configurations
================================

These functions let a plugin build a typed configuration without depending on
the host C++ Config classes. The configuration belongs to storage and follows
the same lifetime and threading rules as temporary geometry: release it with
storage_free(), or let storage_clear() release it with the other temporaries.
PluginStorage is not thread-safe; parallel callbacks must allocate temporary
configs in the scratch storage supplied to that callback.
*/

/* Return all keys as a borrowed array of borrowed null-terminated strings. */
SLIC3R_HOST_API const_strings_t config_keys(const config_handle *config);

/*
Look up one option by its null-terminated key.

The returned option is borrowed from the configuration and remains valid only
while that option is not replaced or removed. A missing key returns null.
*/
SLIC3R_HOST_API const config_option_handle *config_get(
    const config_handle *config,
    const char *key);
SLIC3R_HOST_API config_option_handle *config_get_mutable(
    config_handle *config,
    const char *key);

/* Create an empty mutable configuration owned by storage. */
SLIC3R_HOST_API config_handle *storage_new_config(storage_handle *storage);

/*
Return an existing mutable option or create an empty option of the requested
type. A type mismatch never replaces the existing option and returns null.
*/
SLIC3R_HOST_API config_option_handle *config_get_or_add_mutable(
    config_handle *config,
    const char *key,
    config_option_type type);

/* Remove every option from a mutable dynamic configuration. */
SLIC3R_HOST_API int32_t config_clear(config_handle *config);

/*
Serialize every option into the versioned SCFG text format.

The return value is the required byte count excluding the trailing null byte.
Call once with output == null to size a buffer, then again with capacity at
least return_value + 1. A smaller buffer receives a terminated prefix, matching
the other string-returning functions in this API.
*/
SLIC3R_HOST_API uint32_t config_serialize_all(
    const config_handle *config,
    char *output,
    uint32_t capacity);

/*
Atomically merge one SCFG document into a mutable dynamic configuration.
Serialized keys replace matching keys; keys absent from the document remain.
On failure, the destination is unchanged and zero is returned.
*/
SLIC3R_HOST_API int32_t config_deserialize_all(
    config_handle *destination,
    const char *serialized);

#ifdef __cplusplus
}
#endif

#endif // slic3r_config_h_
