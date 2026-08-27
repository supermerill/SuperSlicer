///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_extrusion_property_h_
#define slic3r_extrusion_property_h_

#include <stdint.h>

#include "slic3r_def.h"
#include "slic3r_gcode_script.h"
#include "slic3r_utils.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
Extrusion properties describe how an extrusion entity should be interpreted.

Each extrusion entity may contain at most one property of a given type. A
property inherited from a parent entity applies to its descendants until a child
defines another property of the same type.

Property payloads are plain C byte records. The built-in property structures
below are stable ABI records. Custom properties registered by plugins follow the
same rule: they must be trivially copyable raw data. If a property needs text or
larger binary data, store that data on the extrusion entity with
extrusion_property_store_data_aligned(), passing the address of the
extrusion_data_id field that will reference the data.
*/

typedef struct extrusion_entity_handle extrusion_entity_handle;
typedef struct orchestrator_handle orchestrator_handle;

typedef slic3r_property_type extrusion_property_type;

#define EXTRUSION_PROPERTY_TYPE_INVALID         ((extrusion_property_type)SLIC3R_PROPERTY_TYPE_INVALID)
#define EXTRUSION_PROPERTY_TYPE_ATTRIBUTES      ((extrusion_property_type)SLIC3R_PROPERTY_TYPE_EXTRUSION_ATTRIBUTES)
#define EXTRUSION_PROPERTY_TYPE_SPEED           ((extrusion_property_type)SLIC3R_PROPERTY_TYPE_EXTRUSION_SPEED)
#define EXTRUSION_PROPERTY_TYPE_MODIFIER        ((extrusion_property_type)SLIC3R_PROPERTY_TYPE_EXTRUSION_MODIFIER)
#define EXTRUSION_PROPERTY_TYPE_CUSTOM_GCODE    ((extrusion_property_type)SLIC3R_PROPERTY_TYPE_EXTRUSION_CUSTOM_GCODE)
#define EXTRUSION_PROPERTY_TYPE_SPECIAL_COMMAND ((extrusion_property_type)SLIC3R_PROPERTY_TYPE_EXTRUSION_SPECIAL_COMMAND)
#define EXTRUSION_PROPERTY_TYPE_OVERHANG        ((extrusion_property_type)SLIC3R_PROPERTY_TYPE_EXTRUSION_OVERHANG)
#define EXTRUSION_PROPERTY_TYPE_Z_OFFSET        ((extrusion_property_type)SLIC3R_PROPERTY_TYPE_EXTRUSION_Z_OFFSET)
#define EXTRUSION_PROPERTY_TYPE_PERIMETER       ((extrusion_property_type)SLIC3R_PROPERTY_TYPE_EXTRUSION_PERIMETER)
#define EXTRUSION_PROPERTY_TYPE_INFILL          ((extrusion_property_type)SLIC3R_PROPERTY_TYPE_EXTRUSION_INFILL)

typedef uint32_t extrusion_data_id;

#define EXTRUSION_DATA_ID_INVALID ((extrusion_data_id)UINT32_MAX)

/*
Register a plugin-defined property type on one orchestrator. orch must not be
NULL for custom properties.

namespaced_name should be stable and unique, for example:
    "com.example.plugin.property_name"

byte_count is the size of one property payload.
alignment is the required C alignment of that payload. Use _Alignof(T) in C11 or
alignof(T) in C++ wrappers.

The returned type id is valid for this orchestrator. It is not a file format
identifier and must not be serialized as a stable value. Keeping the registry on
the orchestrator lets multiple plugin configurations slice in parallel without
sharing custom type ids.
*/
SLIC3R_HOST_API extrusion_property_type extrusion_property_register_type(orchestrator_handle *orch,
                                                                         const char *namespaced_name,
                                                                         uint32_t byte_count,
                                                                         uint32_t alignment);

/* Return the registered byte size for one property payload, or 0 if unknown. */
SLIC3R_HOST_API uint32_t extrusion_property_byte_count(const orchestrator_handle *orch, extrusion_property_type type);

/* Return the registered alignment for one property payload, or 0 if unknown. */
SLIC3R_HOST_API uint32_t extrusion_property_alignment(const orchestrator_handle *orch, extrusion_property_type type);

/*
Return the property type name, or NULL if unknown.

For plugin-defined properties, this is the exact namespaced_name passed to
extrusion_property_register_type(). For built-in properties, this is a stable
host-defined name. The returned string is host-owned.
*/
SLIC3R_HOST_API const char *extrusion_property_name(const orchestrator_handle *orch, extrusion_property_type type);

/* Return the number of properties stored directly on this extrusion entity. */
SLIC3R_HOST_API uint32_t extrusion_property_count(const extrusion_entity_handle *entity);

/*
Return the property type stored at idx.

This is only for enumeration and discovery of the properties stored directly on
this entity. The returned type can be passed to extrusion_property_data(),
extrusion_property_byte_count(), extrusion_property_alignment(), and
extrusion_property_name(). Pass the same orchestrator used to register custom
properties to those introspection functions. This is useful for tools that need
to preserve or inspect custom properties without knowing them at compile time.

The order has no semantic meaning. Returns EXTRUSION_PROPERTY_TYPE_INVALID if
entity is NULL or idx is invalid.
*/
SLIC3R_HOST_API extrusion_property_type extrusion_property_type_at(const extrusion_entity_handle *entity, uint32_t idx);

/* Return non-zero if this extrusion entity directly stores a property of type. */
SLIC3R_HOST_API int32_t extrusion_property_has(const extrusion_entity_handle *entity, extrusion_property_type type);

/*
Return a read-only pointer to one property payload.

The type selects the property slot. Its byte size is
extrusion_property_byte_count(orch, type) when the property type is known to
the orchestrator. The pointer is owned by the extrusion entity. It remains valid
until the property is removed, replaced, or the owning entity is modified by
copy/move or destroyed. Store the data elsewhere if it needs to outlive the
current callback.

This is different from extrusion_data(): property data is the small typed
payload selected by extrusion_property_type; stored data is a larger auxiliary
buffer selected by extrusion_data_id.
*/
SLIC3R_HOST_API const void *extrusion_property_data(const extrusion_entity_handle *entity, extrusion_property_type type);

/*
Return a mutable pointer to an existing property payload.

This does not create the property. Returns NULL if the property is absent or the
type is unknown.
*/
SLIC3R_HOST_API void *extrusion_property_data_mutable(extrusion_entity_handle *entity, extrusion_property_type type);

/*
Return a mutable pointer to a property payload, creating it if needed.

If the property is created, its bytes are zero-initialized. Returns NULL if the
type is unknown in this orchestrator or the entity cannot be modified.
*/
SLIC3R_HOST_API void *extrusion_property_get_or_add_data_mutable(orchestrator_handle *orch,
                                                                 extrusion_entity_handle *entity,
                                                                 extrusion_property_type type);

/*
Remove one property from this entity. Returns non-zero if a property was removed.

Stored data created with extrusion_property_store_data_aligned() for fields in
this property type is released at the same time. Stored data created with
extrusion_store_data_aligned() is independent and is not released here.
*/
SLIC3R_HOST_API int32_t extrusion_property_remove(extrusion_entity_handle *entity, extrusion_property_type type);

/*
Store arbitrary byte data on this extrusion entity and return its id.

The host copies byte_size bytes from data immediately. The caller may reuse,
modify, or release its source buffer after this function returns. data must point
to byte_size readable bytes unless byte_size is 0.

The returned id can be placed inside a property payload, but it is not owned by
that property. The host owns the stored memory, clones it with the extrusion
entity, and releases it when the entity or data id is destroyed.

alignment must be a power of two and at least 1. Passing sizeof(T) bytes with
alignment alignof(T) allows the returned data pointer to be safely cast to T*.
*/
SLIC3R_HOST_API extrusion_data_id extrusion_store_data_aligned(extrusion_entity_handle *entity,
                                                               const void *data,
                                                               uint32_t byte_size,
                                                               uint32_t alignment);

/*
Store arbitrary byte data owned by one extrusion_data_id field inside a property.

Use this for text or binary data whose lifetime should follow a property field.
The host copies the bytes immediately, releases the old data referenced by this
same field, writes the new id into *field, and returns that id.

field must point inside the mutable payload of owner_type, usually obtained from
extrusion_property_get_or_add_data_mutable(). This lets the host verify that the
data id field really belongs to the selected property.

When extrusion_property_remove() removes owner_type, or when owner_type is
replaced, all data stored for fields in owner_type is released automatically.
*/
SLIC3R_HOST_API extrusion_data_id extrusion_property_store_data_aligned(extrusion_entity_handle *entity,
                                                                        extrusion_property_type owner_type,
                                                                        extrusion_data_id *field,
                                                                        const void *data,
                                                                        uint32_t byte_size,
                                                                        uint32_t alignment);

/*
Return a direct read-only pointer to one stored data buffer.

The returned pointer is owned by the extrusion entity. The plugin must not free
it or write through it. The pointer remains valid only while the owning entity is
not destroyed, copied over, moved over, or modified in a way that removes or
reallocates this data id. If the plugin needs the bytes after the current API
call, it must copy them into plugin-owned memory.

byte_size_out may be NULL. If it is not NULL, it receives the exact buffer size
in bytes. Returns NULL if entity is NULL or data_id is invalid. A valid stored
buffer of size 0 may also return NULL; use byte_size_out to distinguish that
case if zero-sized buffers are meaningful for the caller.
*/
SLIC3R_HOST_API const void *extrusion_data(const extrusion_entity_handle *entity,
                                           extrusion_data_id data_id,
                                           uint32_t *byte_size_out);

/*
Free one stored data buffer. Properties referencing this id are not modified.

This is different from extrusion_property_remove(): removing a property removes
the typed payload and the stored data owned by that property type; freeing
stored data releases exactly one auxiliary buffer referenced by id.
*/
SLIC3R_HOST_API int32_t extrusion_free_data(extrusion_entity_handle *entity, extrusion_data_id data_id);

/* Built-in property payloads. */

typedef struct c_extrusion_flow {
    double mm3_per_mm;
    float width;
    float height;
} c_extrusion_flow;

/* Property type: EXTRUSION_PROPERTY_TYPE_ATTRIBUTES. */
typedef struct c_extrusion_property_attributes {
    double mm3_per_mm;
    float width;
    float height;
    uint16_t role;
    uint8_t no_seam;
} c_extrusion_property_attributes;

/* Property type: EXTRUSION_PROPERTY_TYPE_SPEED. */
typedef struct c_extrusion_property_speed {
    float speed_mm_per_s;
    float accel_mm_per_s2;
    float pressure_adv;
    float fan_speed_percent;
    float temperature_C;
} c_extrusion_property_speed;

/* Property type: EXTRUSION_PROPERTY_TYPE_MODIFIER. */
typedef struct c_extrusion_property_modifier {
    uint8_t enforce_travel;
    uint8_t enforce_retraction;
    uint8_t enforce_unlift;
    uint8_t disable_retraction;
    uint8_t disable_lift;
    uint8_t toolchange_retraction;
} c_extrusion_property_modifier;

typedef enum c_extrusion_custom_gcode_kind {
    /* the property field contains raw gcode */
    C_EXTRUSION_CUSTOM_GCODE_GCODE = 0,
    /* the property field contains raw comment to place after a ";" if verbose */
    C_EXTRUSION_CUSTOM_GCODE_COMMENT = 1,
    /* the property field contains scripted gcode that needs to be executed to get the real gcode */
    C_EXTRUSION_CUSTOM_GCODE_SCRIPT = 2
} c_extrusion_custom_gcode_kind;

#define GCODE_SCRIPT_PROCESSING_EXTRUDER_INVALID UINT16_MAX

typedef enum raw_gcode_script_arguments_status {
    RAW_GCODE_SCRIPT_ARGUMENTS_SUCCESS = 0,
    RAW_GCODE_SCRIPT_ARGUMENTS_INVALID_ARGUMENT,
    RAW_GCODE_SCRIPT_ARGUMENTS_INVALID_KEY,
    RAW_GCODE_SCRIPT_ARGUMENTS_DUPLICATE_KEY,
    RAW_GCODE_SCRIPT_ARGUMENTS_INVALID_VALUE,
    RAW_GCODE_SCRIPT_ARGUMENTS_STORAGE_ERROR
} raw_gcode_script_arguments_status;

/* Property type: EXTRUSION_PROPERTY_TYPE_CUSTOM_GCODE. */
typedef struct c_extrusion_property_custom_gcode {
    c_extrusion_custom_gcode_kind kind;
    gcode_script_type script_type;
    extrusion_data_id text_id;
    /* Typed immutable PlaceholderParser inputs supplied by the script producer. */
    extrusion_data_id arguments_id;
    /* Tool whose runtime E state is exposed, or the invalid value for the current tool. */
    uint16_t processing_extruder_id;
} c_extrusion_property_custom_gcode;

/*
Validate and atomically replace the arguments owned by the direct custom
G-code property on entity. The host copies all keys and values immediately.
An empty list clears the stored arguments.
*/
SLIC3R_HOST_API raw_gcode_script_arguments_status extrusion_custom_gcode_set_arguments(
    extrusion_entity_handle *entity,
    const raw_gcode_script_argument *arguments,
    uint32_t argument_count);

/* Return the borrowed opaque arguments referenced by arguments_id. */
SLIC3R_HOST_API const raw_gcode_script_arguments *extrusion_custom_gcode_arguments(
    const extrusion_entity_handle *entity,
    extrusion_data_id arguments_id);

/*
Special commands are non-geometric events carried in the extrusion stream.
c_extrusion_property_special_command.extra_data stores the numeric parameter
described below; commands without an explicit parameter ignore it.
*/
typedef enum c_extrusion_special_command {
    /* Change to the tool index stored in extra_data. */
    C_EXTRUSION_SPECIAL_COMMAND_TOOLCHANGE = 0,
    /* Save the firmware speed override and set it to extra_data, where 1.0 is 100%. */
    C_EXTRUSION_SPECIAL_COMMAND_SAVE_AND_RESET_SPEED_RATIO = 1,
    /* Restore the previously saved firmware speed override, or fall back to 100%. */
    C_EXTRUSION_SPECIAL_COMMAND_RESTORE_SPEED_RATIO = 2,
    /* Flush/synchronize the motion planner with a zero-duration dwell. */
    C_EXTRUSION_SPECIAL_COMMAND_FLUSH_PLANNER_QUEUE = 3,
    /* Emit an E-only extrusion move of extra_data millimeters. */
    C_EXTRUSION_SPECIAL_COMMAND_EXTRUSION = 4,
    /* Emit an E-only retract/deretract move of extra_data millimeters. */
    C_EXTRUSION_SPECIAL_COMMAND_RETRACT = 5,
    /* Pause the print for extra_data milliseconds when no custom pause G-code is configured. */
    C_EXTRUSION_SPECIAL_COMMAND_PAUSE = 6,
    /* Wait for the active extruder temperature; the temperature is carried by speed metadata. */
    C_EXTRUSION_SPECIAL_COMMAND_WAIT_FOR_TEMP = 7,
    /* Stop emitting preview/G-code viewer tags for following generated G-code. */
    C_EXTRUSION_SPECIAL_COMMAND_DISABLE_PREVIEW = 8,
    /* Resume emitting preview/G-code viewer tags for following generated G-code. */
    C_EXTRUSION_SPECIAL_COMMAND_ENABLE_PREVIEW = 9,
    /* Set the extruder motor current/trimpot to extra_data where the firmware supports it. */
    C_EXTRUSION_SPECIAL_COMMAND_EXTRUDER_CURRENT = 10
} c_extrusion_special_command;

/* Property type: EXTRUSION_PROPERTY_TYPE_SPECIAL_COMMAND. */
typedef struct c_extrusion_property_special_command {
    c_extrusion_special_command code;
    double extra_data;
} c_extrusion_property_special_command;

/* Property type: EXTRUSION_PROPERTY_TYPE_OVERHANG. */
typedef struct c_extrusion_property_overhang {
    float start_distance_from_prev_layer;
    float end_distance_from_prev_layer;
    float proximity_to_curled_lines;
    uint8_t has_full_overhangs_flow;
    uint8_t has_full_overhangs_speed;
    uint8_t has_dynamic_overhangs_flow;
    uint8_t has_dynamic_overhangs_speed;
} c_extrusion_property_overhang;

/* Property type: EXTRUSION_PROPERTY_TYPE_Z_OFFSET. */
typedef struct c_extrusion_property_z_offset {
    coord_t z_offset;
} c_extrusion_property_z_offset;

/*
Bit flags stored in c_extrusion_property_perimeter::perimeter_flags.
*/
typedef enum c_extrusion_perimeter_flag {
    /* No perimeter flag is set; the entity is not tagged as a perimeter loop. */
    C_EXTRUSION_PERIMETER_FLAG_NONE = 0,
    /* Base flag for a regular perimeter loop. */
    C_EXTRUSION_PERIMETER_FLAG_LOOP = 1u << 0,
    /* The loop has no inner contour and is the most internal perimeter contour. */
    C_EXTRUSION_PERIMETER_FLAG_INTERNAL = 1u << 1,
    /* Skirt or brim loop. */
    C_EXTRUSION_PERIMETER_FLAG_SKIRT = 1u << 2,
    /* The loop surrounds a hole instead of an infill/material island. */
    C_EXTRUSION_PERIMETER_FLAG_HOLE = 1u << 3,
    /* The loop should be emitted as a vase/spiralized loop. */
    C_EXTRUSION_PERIMETER_FLAG_VASE = 1u << 4,
    /* The loop has no inner loop, used by seam placement logic. */
    C_EXTRUSION_PERIMETER_FLAG_FIRST_LOOP = 1u << 5,
    /* The loop shouldn't have any seam from here. */
    C_EXTRUSION_PERIMETER_FLAG_NO_SEAM = 1u << 6
} c_extrusion_perimeter_flag;

/* Property type: EXTRUSION_PROPERTY_TYPE_PERIMETER. */
typedef struct c_extrusion_property_perimeter {
    int16_t perimeter_idx;
    int16_t reserved;
    /* Bitmask of c_extrusion_perimeter_flag values. Stored as uint16_t to keep the ABI layout stable. */
    uint16_t perimeter_flags;
} c_extrusion_property_perimeter;

/*
Property type: EXTRUSION_PROPERTY_TYPE_INFILL.

Stored on the root extrusion tree generated for one Surface. Descendant
extrusions inherit it, so post-infill plugins can still recover the Surface
that produced a path after splitting or regrouping the tree.
*/
typedef struct c_extrusion_property_infill {
    uint64_t source_surface_id;
} c_extrusion_property_infill;

#ifdef __cplusplus
}
#endif

#endif /* slic3r_extrusion_property_h_ */
