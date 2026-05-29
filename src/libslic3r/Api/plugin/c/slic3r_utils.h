///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_utils_h_
#define slic3r_utils_h_

#include <stdint.h>

#if defined(_WIN32) && defined(SLIC3R_HOST_EXPORTS)
#define SLIC3R_HOST_API __declspec(dllexport)
#else
#define SLIC3R_HOST_API
#endif

extern "C" {

// handle type for temporary storage
typedef struct storage_handle storage_handle;

/* release all objects that were created in the storage */
SLIC3R_HOST_API void storage_clear(storage_handle *me);
/* return 1 if the handle_to_check is an element of the storage, 0 otherwise */
SLIC3R_HOST_API int32_t is_local_storage(storage_handle *me, void* handle_to_check);
/* release an element with handle_to_free to check if it's correct */
SLIC3R_HOST_API int32_t storage_free(storage_handle *me, void* handle_to_free);
/* for debugging purposes, return the number of elements currently stored in the storage */
SLIC3R_HOST_API int32_t storage_size(storage_handle *me);



/* const Array of strings */
typedef struct const_strings_t {
    const char * const *items;
    uint32_t size;
} const_strings_t;


/* tbb */
SLIC3R_HOST_API void slic3r_parallel_for(uint32_t begin, uint32_t end, void *user_data, void (*fn)(uint32_t index, void *user_data));

/*
Parallel loop with one temporary storage per job.

Use this function when each loop item needs to create temporary geometry,
Clipper operands, extrusion entities, or other objects that normally require a
storage_handle. The usual plugin storage is shared by the whole plugin run and
should not be written from several threads. This function gives each job its
own scratch_storage so the temporary allocations made by one job cannot race
with the allocations made by another job.

Typical use:
1. Put read-only shared inputs and any mutex needed for final publication in
   user_data.
2. In fn(), use scratch_storage for all temporary objects created while
   processing index.
3. If the job has to publish a result into the host data tree, call the
   appropriate step callback before fn() returns. Protect that publication with
   a mutex if the callback or destination data tree is not documented as
   thread-safe.

Important lifetime rule:
scratch_storage belongs only to the current fn() call. The host clears and
destroys it as soon as fn() returns for that item. Therefore, never keep a
handle allocated from scratch_storage in user_data, in a global/static variable,
or in the host data tree unless a callback has copied or moved the data
immediately.
*/
typedef void (*slic3r_parallel_for_storage_fn)(uint32_t index, storage_handle *scratch_storage, void *user_data);
SLIC3R_HOST_API void slic3r_parallel_for_storage(uint32_t begin,
                                                 uint32_t end,
                                                 void *user_data,
                                                 slic3r_parallel_for_storage_fn fn);

}
#endif // slic3r_utils_h_
