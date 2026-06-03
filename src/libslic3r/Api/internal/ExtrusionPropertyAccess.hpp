///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_internal_ExtrusionPropertyAccess_hpp_
#define slic3r_Api_internal_ExtrusionPropertyAccess_hpp_

#include <cstddef>
#include <cstdint>

#include "libslic3r/Api/plugin/c/slic3r_extrusion_property.h"

namespace Slic3r {

class ExtrusionPropertyContainer;

namespace ApiInternal {

struct ExtrusionPropertyAccess
{
    static size_t property_count(const ExtrusionPropertyContainer &container);
    static extrusion_property_type property_type_at(const ExtrusionPropertyContainer &container, size_t idx);
    static bool has_property(const ExtrusionPropertyContainer &container, extrusion_property_type type);
    static bool same_property_payload(const ExtrusionPropertyContainer &lhs,
                                      extrusion_property_type type,
                                      const ExtrusionPropertyContainer &rhs);
    static const void *property_data(const ExtrusionPropertyContainer &container, extrusion_property_type type);
    static void *property_data_mutable(ExtrusionPropertyContainer &container, extrusion_property_type type);
    static void *get_or_add_property_data_mutable(ExtrusionPropertyContainer &container,
                                                  extrusion_property_type type,
                                                  size_t byte_count,
                                                  size_t alignment);
    static bool remove_property(ExtrusionPropertyContainer &container, extrusion_property_type type);

    static uint32_t store_data_aligned(ExtrusionPropertyContainer &container,
                                       const void *data,
                                       size_t byte_count,
                                       size_t alignment);
    static uint32_t store_property_data_aligned(ExtrusionPropertyContainer &container,
                                                extrusion_property_type owner_type,
                                                extrusion_data_id *field,
                                                const void *data,
                                                size_t byte_count,
                                                size_t alignment);
    static const void *stored_data(const ExtrusionPropertyContainer &container,
                                   uint32_t data_id,
                                   uint32_t *byte_size_out);
    static bool free_data(ExtrusionPropertyContainer &container, uint32_t data_id);
};

} // namespace ApiInternal

} // namespace Slic3r

#endif // slic3r_Api_internal_ExtrusionPropertyAccess_hpp_
