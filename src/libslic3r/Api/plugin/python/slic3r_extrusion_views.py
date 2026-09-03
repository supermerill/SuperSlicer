#/|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
#/|/
#/|/ SuperSlicer is released under the terms of the AGPLv3 or higher
#/|/

"""
Python extrusion helpers over the SuperSlicer plugin C ABI.

Extrusion entities form a tree. One entity may be empty, may own one local
polyline, or may own ordered children. Properties stored on an entity describe
how that entity and its descendants should be interpreted.

Typical use
-----------

Read a borrowed entity:

    entity = api.extrusion(entity_handle)
    print(entity.length(), entity.flags())
    for child in entity.children():
        ...

Edit a mutable entity received from a step payload:

    entity = api.mutable_extrusion(entity_handle)
    entity.disable_sort()
    entity.set_z_offset(0, 1200)

Create an owned temporary entity:

    with api.new_extrusion(storage_handle) as entity:
        entity.set_points([make_point(0, 0), make_point(1000000, 0)])
        entity.property(CExtrusionPropertyAttributes).role = RAW_EXTRUSION_ROLE_PERIMETER

Build medial-axis extrusion from an ExPolygon:

    thin_wall = api.medial_axis_thin_wall(flow) \
        .medial_widths(min_width_scaled, max_width_scaled) \
        .can_reverse(False) \
        .build(storage_handle, expolygon)

Const-correctness model
-----------------------
- ExtrusionEntity is a borrowed read-only view. It returns property payloads as
  copies, so editing the returned object never modifies the host.
- MutableExtrusionEntity is a borrowed mutable view. Use it only when the host
  explicitly gives the plugin a mutable handle.
- StoredExtrusionEntity owns a mutable handle allocated in a storage_handle and
  releases it with storage_free().

Property guide: [Using Plugin Properties](/doc/plugins/properties.md)

Extrusion guide: [Using Unified Extrusion Entities](/doc/plugins/extrusions.md)
"""

from __future__ import annotations

import ctypes
from typing import TYPE_CHECKING, Iterator, NamedTuple, Sequence

from slic3r_api_generated import (
    CFlow,
    CMedialAxisExtrusionParams,
    CExtrusionPropertyAttributes,
    CExtrusionPropertyCustomGcode,
    CExtrusionPropertyExtrusionAxis,
    CExtrusionPropertyInfill,
    CExtrusionPropertyModifier,
    CExtrusionPropertyOverhang,
    CExtrusionPropertyPerimeter,
    CExtrusionPropertySpecialCommand,
    CExtrusionPropertySpeed,
    CExtrusionPropertyZOffset,
    CExtrusionSegment,
    CPoint,
    C_EXTRUSION_CUSTOM_GCODE_GCODE,
    C_EXTRUSION_CUSTOM_GCODE_SCRIPT,
    EXTRUSION_DATA_ID_INVALID,
    EXTRUSION_INDEX_INVALID,
    GCODE_SCRIPT_PROCESSING_EXTRUDER_INVALID,
    GCODE_SCRIPT_TYPE_EXTRUSION_CUSTOM,
    GCODE_SCRIPT_TYPE_INVALID,
    EXTRUSION_PROPERTY_TYPE_ATTRIBUTES,
    EXTRUSION_PROPERTY_TYPE_CUSTOM_GCODE,
    EXTRUSION_PROPERTY_TYPE_INFILL,
    EXTRUSION_PROPERTY_TYPE_EXTRUSION_AXIS,
    EXTRUSION_PROPERTY_TYPE_INVALID,
    EXTRUSION_PROPERTY_TYPE_MODIFIER,
    EXTRUSION_PROPERTY_TYPE_OVERHANG,
    EXTRUSION_PROPERTY_TYPE_PERIMETER,
    EXTRUSION_PROPERTY_TYPE_SPECIAL_COMMAND,
    EXTRUSION_PROPERTY_TYPE_SPEED,
    EXTRUSION_PROPERTY_TYPE_Z_OFFSET,
    EXTRUSION_SPLIT_FRAGMENT,
    MEDIAL_AXIS_EXTRUSION_CAN_REVERSE,
    MEDIAL_AXIS_EXTRUSION_CONSTANT_WIDTH,
    MEDIAL_AXIS_EXTRUSION_KEEP_EMPTY_ROOT,
    MEDIAL_AXIS_EXTRUSION_TRIM_THIN_ENDPOINTS,
    RAW_EXTRUSION_FLAG_REVERSIBLE,
    RAW_EXTRUSION_FLAG_SORTABLE,
    RAW_EXTRUSION_EXISTING_PROPERTIES_KEEP_ON_PARENT,
    RAW_EXTRUSION_EXISTING_PROPERTIES_MOVE_WITH_CONTENT,
    RAW_EXTRUSION_ORDERED_LEAF_AFTER,
    RAW_EXTRUSION_ORDERED_LEAF_BEFORE,
    RAW_EXTRUSION_SPLIT_STATUS_CLIPPING_FAILED,
    RAW_EXTRUSION_SPLIT_STATUS_INTERNAL_ERROR,
    RAW_EXTRUSION_SPLIT_STATUS_INVALID_ARGUMENT,
    RAW_EXTRUSION_SPLIT_STATUS_INVALID_GEOMETRY,
    RAW_EXTRUSION_SPLIT_STATUS_NOT_A_LEAF,
    RAW_EXTRUSION_SPLIT_STATUS_SUCCESS,
    RAW_EXTRUSION_ROLE_GAP_FILL,
    RAW_EXTRUSION_ROLE_THIN_WALL,
    SCALED_EPSILON,
)

from slic3r_geometry_views import as_point, make_point, point_tuple

if TYPE_CHECKING:
    # Data-tree views import extrusion views at runtime, so this import must
    # remain type-only to avoid a circular module initialization.
    from slic3r_datatree_views import Config


CExtrusionPropertyAttributes.property_type = EXTRUSION_PROPERTY_TYPE_ATTRIBUTES
CExtrusionPropertySpeed.property_type = EXTRUSION_PROPERTY_TYPE_SPEED
CExtrusionPropertyModifier.property_type = EXTRUSION_PROPERTY_TYPE_MODIFIER
CExtrusionPropertyCustomGcode.property_type = EXTRUSION_PROPERTY_TYPE_CUSTOM_GCODE
CExtrusionPropertySpecialCommand.property_type = EXTRUSION_PROPERTY_TYPE_SPECIAL_COMMAND
CExtrusionPropertyOverhang.property_type = EXTRUSION_PROPERTY_TYPE_OVERHANG
CExtrusionPropertyZOffset.property_type = EXTRUSION_PROPERTY_TYPE_Z_OFFSET
CExtrusionPropertyPerimeter.property_type = EXTRUSION_PROPERTY_TYPE_PERIMETER
CExtrusionPropertyInfill.property_type = EXTRUSION_PROPERTY_TYPE_INFILL
CExtrusionPropertyExtrusionAxis.property_type = EXTRUSION_PROPERTY_TYPE_EXTRUSION_AXIS

EPropertyAttributes = CExtrusionPropertyAttributes
EPropertySpeed = CExtrusionPropertySpeed
EPropertyModifier = CExtrusionPropertyModifier
EPropertyCustomGcode = CExtrusionPropertyCustomGcode
EPropertySpecialCommand = CExtrusionPropertySpecialCommand
EPropertyExtrusionAxis = CExtrusionPropertyExtrusionAxis
EPropertyOverhang = CExtrusionPropertyOverhang
EPropertyZOffset = CExtrusionPropertyZOffset
EPropertyPerimeter = CExtrusionPropertyPerimeter
EPropertyInfill = CExtrusionPropertyInfill


def _address(handle) -> int:
    if handle is None:
        return 0
    if isinstance(handle, ctypes.c_void_p):
        return int(handle.value or 0)
    return int(handle)


def _void_p(handle) -> ctypes.c_void_p:
    return ctypes.c_void_p(_address(handle))


def _require_handle(handle, name: str = "handle") -> int:
    address = _address(handle)
    if not address:
        raise ValueError(f"{name} must not be null")
    return address


def _is_invalid_index(idx: int) -> bool:
    return int(idx) == EXTRUSION_INDEX_INVALID


def _points_equal(lhs: CPoint, rhs: CPoint) -> bool:
    return lhs.x == rhs.x and lhs.y == rhs.y


def _payload_type(payload_cls_or_type) -> int:
    if isinstance(payload_cls_or_type, int):
        return int(payload_cls_or_type)
    return int(payload_cls_or_type.property_type)


def _payload_copy(payload_cls, ptr: int):
    if not ptr:
        return None
    return payload_cls.from_buffer_copy(ctypes.string_at(_void_p(ptr), ctypes.sizeof(payload_cls)))


def _payload_mutable(payload_cls, ptr: int):
    if not ptr:
        return None
    return ctypes.cast(_void_p(ptr), ctypes.POINTER(payload_cls)).contents


def _data_buffer(data):
    if data is None:
        return None, None, 0
    if isinstance(data, str):
        return _data_buffer(data.encode("utf-8") + b"\0")
    if isinstance(data, bytes):
        buffer = ctypes.create_string_buffer(data, len(data))
        return buffer, ctypes.cast(buffer, ctypes.c_void_p), len(data)
    if isinstance(data, bytearray):
        return _data_buffer(bytes(data))
    if isinstance(data, ctypes.Array):
        return data, ctypes.cast(data, ctypes.c_void_p), ctypes.sizeof(data)
    if isinstance(data, ctypes.Structure):
        return data, ctypes.cast(ctypes.pointer(data), ctypes.c_void_p), ctypes.sizeof(data)
    raise TypeError(f"unsupported data buffer type: {type(data)!r}")


def _array_from_points(points: Sequence[CPoint]):
    converted = [as_point(point) for point in points]
    if not converted:
        return None, 0
    return (CPoint * len(converted))(*converted), len(converted)


def _array_from_segments(segments: Sequence[CExtrusionSegment]):
    converted = list(segments)
    if not converted:
        return None, 0
    return (CExtrusionSegment * len(converted))(*converted), len(converted)


def _field_pointer(payload, field_name: str):
    offset = getattr(type(payload), field_name).offset
    return ctypes.cast(ctypes.byref(payload, offset), ctypes.POINTER(ctypes.c_uint32))


class ExtrusionAreaFragment(NamedTuple):
    """Borrowed mutable fragment returned by split_leaf_by_areas().

    The entity view remains valid only until a later structural mutation of
    the source leaf.
    """

    entity: "MutableExtrusionEntity"
    area_index: int


# Shared read-only property API. Read access returns copies for built-in payload
# structures, preserving Python-level const-correctness.
class ExtrusionPropertyReadMixin:
    def property_count(self) -> int:
        return int(self.api.host.extrusion_property_count(self.c_handle()))

    def property_type_at(self, idx: int) -> int:
        return int(self.api.host.extrusion_property_type_at(self.c_handle(), int(idx)))

    def property_types(self) -> list[int]:
        return [self.property_type_at(idx) for idx in range(self.property_count())]

    def has_property(self, payload_cls_or_type) -> bool:
        return bool(self.api.host.extrusion_property_has(self.c_handle(), _payload_type(payload_cls_or_type)))

    def property_data_address(self, payload_cls_or_type) -> int:
        return _address(self.api.host.extrusion_property_data(self.c_handle(), _payload_type(payload_cls_or_type)))

    def property(self, payload_cls):
        return _payload_copy(payload_cls, self.property_data_address(payload_cls))

    def property_or(self, payload_cls, fallback):
        payload = self.property(payload_cls)
        return fallback if payload is None else payload

    def stored_data(self, data_id: int) -> bytes | None:
        byte_size = ctypes.c_uint32(0)
        ptr = self.api.host.extrusion_data(self.c_handle(), int(data_id), ctypes.byref(byte_size))
        if not ptr and byte_size.value == 0:
            return None
        return ctypes.string_at(ptr, byte_size.value)

    def stored_string(self, data_id: int) -> str:
        data = self.stored_data(data_id)
        if not data:
            return ""
        if data[-1:] == b"\0":
            data = data[:-1]
        return data.decode("utf-8", errors="replace")


# Mutable property API. The returned ctypes payload object is a live view over
# host memory; only use it on MutableExtrusionEntity or StoredExtrusionEntity.
class ExtrusionPropertyMutableMixin:
    def property_data_mutable_address(self, payload_cls_or_type) -> int:
        return _address(self.api.host.extrusion_property_data_mutable(
            self.mutable_c_handle(), _payload_type(payload_cls_or_type)
        ))

    def property_mutable(self, payload_cls):
        return _payload_mutable(payload_cls, self.property_data_mutable_address(payload_cls))

    def get_or_add_property_data_mutable_address(self, payload_cls_or_type, orchestrator=None) -> int:
        orch = self.api.orchestrator if orchestrator is None else _void_p(orchestrator)
        return _address(self.api.host.extrusion_property_get_or_add_data_mutable(
            orch, self.mutable_c_handle(), _payload_type(payload_cls_or_type)
        ))

    def property(self, payload_cls, orchestrator=None):
        return _payload_mutable(payload_cls, self.get_or_add_property_data_mutable_address(payload_cls, orchestrator))

    def remove_property(self, payload_cls_or_type) -> bool:
        return bool(self.api.host.extrusion_property_remove(self.mutable_c_handle(), _payload_type(payload_cls_or_type)))

    def store_data_aligned(self, data, alignment: int = 1) -> int:
        keepalive, ptr, byte_size = _data_buffer(data)
        if byte_size == 0:
            return int(self.api.host.extrusion_store_data_aligned(self.mutable_c_handle(), None, 0, int(alignment)))
        return int(self.api.host.extrusion_store_data_aligned(self.mutable_c_handle(), ptr, byte_size, int(alignment)))

    def store_property_data_aligned(self, owner_payload_cls_or_type, payload, field_name: str, data, alignment: int = 1) -> int:
        field = _field_pointer(payload, field_name)
        keepalive, ptr, byte_size = _data_buffer(data)
        if byte_size == 0:
            return int(self.api.host.extrusion_property_store_data_aligned(
                self.mutable_c_handle(), _payload_type(owner_payload_cls_or_type), field, None, 0, int(alignment)
            ))
        return int(self.api.host.extrusion_property_store_data_aligned(
            self.mutable_c_handle(), _payload_type(owner_payload_cls_or_type), field, ptr, byte_size, int(alignment)
        ))

    def store_string(self, text: str) -> int:
        return self.store_data_aligned(text, ctypes.alignment(ctypes.c_char))

    def store_property_string(self, owner_payload_cls_or_type, payload, field_name: str, text: str) -> int:
        return self.store_property_data_aligned(owner_payload_cls_or_type, payload, field_name, text, ctypes.alignment(ctypes.c_char))

    def free_data(self, data_id: int) -> bool:
        return bool(self.api.host.extrusion_free_data(self.mutable_c_handle(), int(data_id)))

    def custom_gcode(self, text: str, kind: int = C_EXTRUSION_CUSTOM_GCODE_GCODE):
        payload = self.property(CExtrusionPropertyCustomGcode)
        if payload.config_id != EXTRUSION_DATA_ID_INVALID:
            self.free_data(payload.config_id)
        payload.kind = int(kind)
        payload.script_type = int(
            GCODE_SCRIPT_TYPE_EXTRUSION_CUSTOM
            if kind == C_EXTRUSION_CUSTOM_GCODE_SCRIPT
            else GCODE_SCRIPT_TYPE_INVALID
        )
        payload.config_id = EXTRUSION_DATA_ID_INVALID
        payload.processing_extruder_id = GCODE_SCRIPT_PROCESSING_EXTRUDER_INVALID
        text_id = self.store_property_string(CExtrusionPropertyCustomGcode, payload, "text_id", text)
        if text_id == EXTRUSION_DATA_ID_INVALID:
            self.remove_property(CExtrusionPropertyCustomGcode)
            raise RuntimeError("The custom G-code text could not be stored")
        return payload

    def script_gcode(
        self,
        text: str,
        script_type: int,
        config: Config | None = None,
        processing_extruder_id: int | None = None,
    ):
        # Serialize before touching the extrusion so a configuration error
        # cannot replace an existing custom G-code property halfway through.
        serialized_config = None if config is None else config.serialize_all()
        payload = self.custom_gcode(text, C_EXTRUSION_CUSTOM_GCODE_SCRIPT)
        payload.script_type = int(script_type)
        if processing_extruder_id is not None:
            if processing_extruder_id < 0 or processing_extruder_id >= GCODE_SCRIPT_PROCESSING_EXTRUDER_INVALID:
                raise ValueError("A script processing extruder must fit in uint16_t.")
            payload.processing_extruder_id = int(processing_extruder_id)

        if serialized_config is not None:
            config_id = self.store_property_string(
                CExtrusionPropertyCustomGcode, payload, "config_id", serialized_config)
            if config_id == EXTRUSION_DATA_ID_INVALID:
                self.remove_property(CExtrusionPropertyCustomGcode)
                raise RuntimeError("The G-code script configuration could not be stored")
        return payload


# Read-only local polyline API. It only touches points/segments stored directly
# on this entity; tree-wide helpers such as front(), back() and length() are on
# ExtrusionEntityReadMixin.
class ExtrusionPolylineReadMixin:
    def has_polyline(self) -> bool:
        return bool(self.api.host.extrusion_has_polyline(self.c_handle()))

    def point_count(self) -> int:
        return int(self.api.host.extrusion_polyline_point_count(self.c_handle()))

    def segment_count(self) -> int:
        return int(self.api.host.extrusion_polyline_segment_count(self.c_handle()))

    def has_local_points(self) -> bool:
        return self.point_count() > 0

    def point(self, idx: int) -> CPoint:
        return self.api.host.extrusion_polyline_point(self.c_handle(), int(idx))

    def local_front(self) -> CPoint:
        if self.point_count() == 0:
            raise IndexError("entity has no local points")
        return self.point(0)

    def local_back(self) -> CPoint:
        if self.point_count() == 0:
            raise IndexError("entity has no local points")
        return self.point(self.point_count() - 1)

    def local_middle(self) -> CPoint:
        if self.point_count() == 0:
            raise IndexError("entity has no local points")
        return self.point(self.point_count() // 2)

    def local_length(self) -> float:
        return float(self.api.host.extrusion_polyline_length(self.c_handle()))

    def local_is_closed(self) -> bool:
        return self.point_count() > 1 and _points_equal(self.local_front(), self.local_back())

    def find_point(self, point, max_distance: int) -> int:
        return int(self.api.host.extrusion_polyline_find_point(self.c_handle(), as_point(point), int(max_distance)))

    def point_from_end(self, distance: float) -> CPoint:
        return self.api.host.extrusion_polyline_point_from_end(self.c_handle(), float(distance))

    def point_from_begin(self, distance: float) -> CPoint:
        length = self.local_length()
        return self.point_from_end(0.0 if distance >= length else length - float(distance))

    def point_from_start(self, distance: float) -> CPoint:
        return self.point_from_begin(distance)

    def segment(self, idx: int) -> CExtrusionSegment:
        out = CExtrusionSegment()
        ok = self.api.host.extrusion_polyline_segment(self.c_handle(), int(idx), ctypes.byref(out))
        if not ok:
            raise IndexError(idx)
        return out

    def points(self) -> list[CPoint]:
        needed = int(self.api.host.extrusion_polyline_copy_points(self.c_handle(), None, 0))
        if needed == 0:
            return []
        out = (CPoint * needed)()
        written = int(self.api.host.extrusion_polyline_copy_points(self.c_handle(), out, needed))
        return [out[idx] for idx in range(written)]

    def segments(self) -> list[CExtrusionSegment]:
        needed = int(self.api.host.extrusion_polyline_copy_segments(self.c_handle(), None, 0))
        if needed == 0:
            return []
        out = (CExtrusionSegment * needed)()
        written = int(self.api.host.extrusion_polyline_copy_segments(self.c_handle(), out, needed))
        return [out[idx] for idx in range(written)]

    def has_z_offsets(self) -> bool:
        return bool(self.api.host.extrusion_polyline_has_z_offsets(self.c_handle()))

    def z_offset(self, idx: int) -> int:
        out = ctypes.c_int64(0)
        ok = self.api.host.extrusion_polyline_z_offset(self.c_handle(), int(idx), ctypes.byref(out))
        if not ok:
            raise IndexError(idx)
        return int(out.value)


# Mutable local polyline API. It cannot create a local polyline on an entity that
# already has children; clear_content() first if that conversion is intentional.
class ExtrusionPolylineMutableMixin:
    def clear_polyline(self) -> bool:
        return bool(self.api.host.extrusion_polyline_clear(self.mutable_c_handle()))

    def set_point(self, idx: int, point) -> bool:
        return bool(self.api.host.extrusion_polyline_set_point(self.mutable_c_handle(), int(idx), as_point(point)))

    def insert_point(self, idx: int, point) -> int:
        return int(self.api.host.extrusion_polyline_insert_point(self.mutable_c_handle(), int(idx), as_point(point)))

    def remove_point(self, idx: int) -> bool:
        return bool(self.api.host.extrusion_polyline_remove_point(self.mutable_c_handle(), int(idx)))

    def set_points(self, points: Sequence[CPoint]) -> bool:
        array, count = _array_from_points(points)
        return bool(self.api.host.extrusion_polyline_set_points(self.mutable_c_handle(), array, count))

    def set_segment(self, idx: int, segment: CExtrusionSegment) -> bool:
        return bool(self.api.host.extrusion_polyline_set_segment(self.mutable_c_handle(), int(idx), ctypes.byref(segment)))

    def set_segments(self, segments: Sequence[CExtrusionSegment]) -> bool:
        array, count = _array_from_segments(segments)
        return bool(self.api.host.extrusion_polyline_set_segments(self.mutable_c_handle(), array, count))

    def split_at_point(self, point, out_first, out_second) -> bool:
        return bool(self.api.host.extrusion_polyline_split_at_point(
            self.c_handle(), as_point(point), out_first.mutable_c_handle(), out_second.mutable_c_handle()
        ))

    def split_at_distance(self, distance: float, out_first, out_second) -> bool:
        return bool(self.api.host.extrusion_polyline_split_at_distance(
            self.c_handle(), float(distance), out_first.mutable_c_handle(), out_second.mutable_c_handle()
        ))

    def split_at_index(self, point_idx: int, out_first, out_second) -> bool:
        return bool(self.api.host.extrusion_polyline_split_at_index(
            self.c_handle(), int(point_idx), out_first.mutable_c_handle(), out_second.mutable_c_handle()
        ))

    def split_at_point_new(self, storage, point) -> tuple["StoredExtrusionEntity", "StoredExtrusionEntity"]:
        first = StoredExtrusionEntity(self.api, storage)
        second = StoredExtrusionEntity(self.api, storage)
        if not self.split_at_point(point, first, second):
            raise ValueError("failed to split extrusion polyline at point")
        return first, second

    def split_at_distance_new(self, storage, distance: float) -> tuple["StoredExtrusionEntity", "StoredExtrusionEntity"]:
        first = StoredExtrusionEntity(self.api, storage)
        second = StoredExtrusionEntity(self.api, storage)
        if not self.split_at_distance(distance, first, second):
            raise ValueError("failed to split extrusion polyline at distance")
        return first, second

    def split_at_index_new(self, storage, point_idx: int) -> tuple["StoredExtrusionEntity", "StoredExtrusionEntity"]:
        first = StoredExtrusionEntity(self.api, storage)
        second = StoredExtrusionEntity(self.api, storage)
        if not self.split_at_index(point_idx, first, second):
            raise ValueError("failed to split extrusion polyline at index")
        return first, second

    def clip_end(self, distance: float) -> bool:
        return bool(self.api.host.extrusion_polyline_clip_end(self.mutable_c_handle(), float(distance)))

    def clip_start(self, distance: float) -> bool:
        if not self.reverse():
            return False
        clipped = self.clip_end(distance)
        restored = self.reverse()
        return clipped and restored

    def translate(self, offset) -> bool:
        return bool(self.api.host.extrusion_polyline_translate(self.mutable_c_handle(), as_point(offset)))

    def rotate(self, angle: float) -> bool:
        return bool(self.api.host.extrusion_polyline_rotate(self.mutable_c_handle(), float(angle)))

    def reverse(self) -> bool:
        return bool(self.api.host.extrusion_polyline_reverse(self.mutable_c_handle()))

    def set_z_offset(self, idx: int, z_offset: int) -> bool:
        return bool(self.api.host.extrusion_polyline_set_z_offset(self.mutable_c_handle(), int(idx), int(z_offset)))

    def clear_z_offsets(self) -> bool:
        return bool(self.api.host.extrusion_polyline_clear_z_offsets(self.mutable_c_handle()))


# Read-only tree API. It exposes direct children and higher-level helpers that
# walk through children when the current entity has no local polyline.
class ExtrusionEntityReadMixin:
    def valid(self) -> bool:
        return self.address != 0

    def flags(self) -> int:
        return int(self.api.host.extrusion_flags(self.c_handle()))

    def reversible(self) -> bool:
        return (self.flags() & RAW_EXTRUSION_FLAG_REVERSIBLE) != 0

    def sortable(self) -> bool:
        return (self.flags() & RAW_EXTRUSION_FLAG_SORTABLE) != 0

    def continuous(self) -> bool:
        """Return True when current children form one ordered path.

        Continuity is computed by the host. Python plugins cannot force it with
        a flag; build children in order and disable sorting instead.
        """
        return bool(self.api.host.extrusion_is_continuous(self.c_handle()))

    def is_leaf(self) -> bool:
        return not bool(self.api.host.extrusion_has_children(self.c_handle()))

    def child_count(self) -> int:
        return int(self.api.host.extrusion_child_count(self.c_handle()))

    def child(self, idx: int) -> "ExtrusionEntity":
        handle = self.api.host.extrusion_child(self.c_handle(), int(idx))
        return ExtrusionEntity(self.api, handle)

    def children(self) -> Iterator["ExtrusionEntity"]:
        for idx in range(self.child_count()):
            yield self.child(idx)

    def empty(self) -> bool:
        if self.point_count() > 0:
            return False
        return all(child.empty() for child in self.children())

    def front(self) -> CPoint:
        if self.point_count() > 0:
            return self.local_front()
        for child in self.children():
            if not child.empty():
                return child.front()
        raise ValueError("empty extrusion entity")

    def back(self) -> CPoint:
        if self.point_count() > 0:
            return self.local_back()
        for idx in range(self.child_count() - 1, -1, -1):
            child = self.child(idx)
            if not child.empty():
                return child.back()
        raise ValueError("empty extrusion entity")

    def middle(self) -> CPoint:
        if self.point_count() > 0:
            return self.local_middle()
        for idx in range(self.child_count() // 2, self.child_count()):
            child = self.child(idx)
            if not child.empty():
                return child.middle()
        return self.front()

    def length(self) -> float:
        if self.point_count() > 0:
            return self.local_length()
        return sum(child.length() for child in self.children())

    def is_closed(self) -> bool:
        return not self.empty() and _points_equal(self.front(), self.back())

    def collect_points(self) -> list[CPoint]:
        out = list(self.points())
        for child in self.children():
            out.extend(child.collect_points())
        return out

    def clone(self, storage) -> "StoredExtrusionEntity":
        return StoredExtrusionEntity(self.api, storage, self)


# Mutable tree API. Mutating children may invalidate previously borrowed child
# views, just like mutating a Python list invalidates assumptions about indices.
class ExtrusionEntityMutableMixin:
    def child_mutable(self, idx: int) -> "MutableExtrusionEntity":
        handle = self.api.host.extrusion_child_mutable(self.mutable_c_handle(), int(idx))
        return MutableExtrusionEntity(self.api, handle)

    def set_flags(self, flags: int) -> bool:
        return bool(self.api.host.extrusion_set_flags(self.mutable_c_handle(), int(flags)))

    def set_reversible(self, enabled: bool = True):
        flags = self.flags()
        flags = flags | RAW_EXTRUSION_FLAG_REVERSIBLE if enabled else flags & ~RAW_EXTRUSION_FLAG_REVERSIBLE
        self.set_flags(flags)
        return self

    def enable_reverse(self):
        return self.set_reversible(True)

    def disable_reverse(self):
        return self.set_reversible(False)

    def set_sortable(self, enabled: bool = True):
        flags = self.flags()
        flags = flags | RAW_EXTRUSION_FLAG_SORTABLE if enabled else flags & ~RAW_EXTRUSION_FLAG_SORTABLE
        self.set_flags(flags)
        return self

    def enable_sort(self):
        return self.set_sortable(True)

    def disable_sort(self):
        return self.set_sortable(False)

    def clear_content(self) -> bool:
        return bool(self.api.host.extrusion_clear_content(self.mutable_c_handle()))

    def insert_child_copy(self, idx: int, child: "ExtrusionEntity") -> int:
        return int(self.api.host.extrusion_insert_child_copy(self.mutable_c_handle(), int(idx), child.c_handle()))

    def insert_child_move(self, idx: int, child: "MutableExtrusionEntity") -> int:
        return int(self.api.host.extrusion_insert_child_move(self.mutable_c_handle(), int(idx), child.mutable_c_handle()))

    def emplace_ordered_leaf(
        self,
        position: int,
        property_placement: int,
    ) -> "MutableExtrusionEntity | None":
        """Create a fixed empty leaf at one boundary of the current content.

        Use ``RAW_EXTRUSION_ORDERED_LEAF_BEFORE`` or ``AFTER`` for the
        position. The property-placement constants choose whether direct
        properties stay inherited from this parent or move with the previous
        content. The returned view is borrowed from this tree.
        """
        handle = self.api.host.extrusion_emplace_ordered_leaf(
            self.mutable_c_handle(), int(position), int(property_placement)
        )
        if not handle:
            return None
        return MutableExtrusionEntity(self.api, handle)

    def add_child_copy(self, child: "ExtrusionEntity") -> int:
        return self.insert_child_copy(self.child_count(), child)

    def add_child_move(self, child: "MutableExtrusionEntity") -> int:
        return self.insert_child_move(self.child_count(), child)

    def remove_child(self, idx: int) -> bool:
        return bool(self.api.host.extrusion_remove_child(self.mutable_c_handle(), int(idx)))

    def move_child_from(self, dst_idx: int, src_parent: "MutableExtrusionEntity", src_idx: int) -> int:
        return int(self.api.host.extrusion_move_child(
            self.mutable_c_handle(), int(dst_idx), src_parent.mutable_c_handle(), int(src_idx)
        ))

    def split_leaf_by_areas(
        self,
        areas: Sequence,
        max_deviation: int = SCALED_EPSILON,
    ) -> list[ExtrusionAreaFragment]:
        """Split this leaf with a complete, disjoint area partition.

        Empty collections may be present in ``areas``. The caller is
        responsible for ensuring that all collections together are disjoint
        and cover the complete extrusion path.
        """
        area_addresses = [
            _address(area.c_handle() if hasattr(area, "c_handle") else area)
            for area in areas
        ]
        area_array = None
        if area_addresses:
            area_array = (ctypes.c_void_p * len(area_addresses))(
                *(ctypes.c_void_p(address) for address in area_addresses)
            )

        fragments: list[ExtrusionAreaFragment] = []
        callback_errors: list[BaseException] = []

        def collect_fragment(fragment_handle, area_index, _user_data) -> None:
            try:
                fragments.append(ExtrusionAreaFragment(
                    MutableExtrusionEntity(self.api, fragment_handle),
                    int(area_index),
                ))
            except BaseException as exception:
                # ctypes must never observe a Python exception escaping its C
                # callback. Raise it after the synchronous host call returns.
                callback_errors.append(exception)

        callback = EXTRUSION_SPLIT_FRAGMENT(collect_fragment)
        status = int(self.api.host.extrusion_split_leaf_by_areas(
            self.mutable_c_handle(),
            area_array,
            len(area_addresses),
            int(max_deviation),
            callback,
            None,
        ))

        if callback_errors:
            raise callback_errors[0]
        if status == RAW_EXTRUSION_SPLIT_STATUS_SUCCESS:
            return fragments
        if status == RAW_EXTRUSION_SPLIT_STATUS_INVALID_ARGUMENT:
            raise ValueError("extrusion_split_leaf_by_areas: invalid argument")
        if status == RAW_EXTRUSION_SPLIT_STATUS_NOT_A_LEAF:
            raise ValueError("extrusion_split_leaf_by_areas: entity is not a leaf")
        if status == RAW_EXTRUSION_SPLIT_STATUS_INVALID_GEOMETRY:
            raise ValueError("extrusion_split_leaf_by_areas: invalid leaf or area geometry")
        if status == RAW_EXTRUSION_SPLIT_STATUS_CLIPPING_FAILED:
            raise RuntimeError("extrusion_split_leaf_by_areas: clipping failed")
        if status == RAW_EXTRUSION_SPLIT_STATUS_INTERNAL_ERROR:
            raise RuntimeError("extrusion_split_leaf_by_areas: internal error")
        raise RuntimeError(f"extrusion_split_leaf_by_areas: unknown status {status}")


# Borrowed read-only extrusion entity. Use this for input handles or child views
# when the plugin must inspect but not modify the tree.
class ExtrusionEntity(
    ExtrusionPropertyReadMixin,
    ExtrusionPolylineReadMixin,
    ExtrusionEntityReadMixin,
):
    def __init__(self, api, handle) -> None:
        self.api = api
        self.address = _require_handle(handle)

    def c_handle(self) -> ctypes.c_void_p:
        return _void_p(self.address)

    def same_handle(self, other: "ExtrusionEntity") -> bool:
        return self.address == other.address

    def __bool__(self) -> bool:
        return self.valid()


# Borrowed mutable extrusion entity. Use this only when the step payload or host
# documentation says the handle may be modified but is not owned by the plugin.
class MutableExtrusionEntity(
    ExtrusionPropertyReadMixin,
    ExtrusionPropertyMutableMixin,
    ExtrusionPolylineReadMixin,
    ExtrusionPolylineMutableMixin,
    ExtrusionEntityReadMixin,
    ExtrusionEntityMutableMixin,
):
    def __init__(self, api, handle) -> None:
        self.api = api
        self.address = _require_handle(handle)

    def c_handle(self) -> ctypes.c_void_p:
        return _void_p(self.address)

    def mutable_c_handle(self) -> ctypes.c_void_p:
        return _void_p(self.address)

    def readonly(self) -> ExtrusionEntity:
        return ExtrusionEntity(self.api, self.address)

    def property(self, payload_cls, orchestrator=None):
        return ExtrusionPropertyMutableMixin.property(self, payload_cls, orchestrator)

    def copy_from(self, src: ExtrusionEntity) -> bool:
        return bool(self.api.host.extrusion_copy_from(self.mutable_c_handle(), src.c_handle()))

    def move_from(self, src: "MutableExtrusionEntity") -> bool:
        return bool(self.api.host.extrusion_move_from(self.mutable_c_handle(), src.mutable_c_handle()))

    def __bool__(self) -> bool:
        return self.valid()


# Owned mutable extrusion entity allocated in a storage_handle. This is the
# normal way for a Python plugin to build a new extrusion tree.
class StoredExtrusionEntity(MutableExtrusionEntity):
    def __init__(self, api, storage, src: ExtrusionEntity | MutableExtrusionEntity | Sequence[CPoint] | None = None) -> None:
        self._storage = _require_handle(storage, "storage")
        if src is None:
            handle = api.host.extrusion_create_empty(_void_p(self._storage))
        elif isinstance(src, (ExtrusionEntity, MutableExtrusionEntity)):
            handle = api.host.extrusion_clone(_void_p(self._storage), src.c_handle())
        else:
            handle = api.host.extrusion_create_empty(_void_p(self._storage))
        super().__init__(api, handle)
        self._owns_handle = True
        if src is not None and not isinstance(src, (ExtrusionEntity, MutableExtrusionEntity)):
            self.set_points(src)

    @classmethod
    def adopt_owned(cls, api, storage, handle) -> "StoredExtrusionEntity":
        """Low-level helper: take ownership of a mutable extrusion handle."""
        out = cls.__new__(cls)
        out._storage = _require_handle(storage, "storage")
        MutableExtrusionEntity.__init__(out, api, handle)
        out._owns_handle = True
        return out

    def storage(self) -> int:
        return self._storage

    def release(self) -> int:
        handle = self.address
        self.address = 0
        self._owns_handle = False
        return handle

    def free_from_storage(self) -> bool:
        if not getattr(self, "_owns_handle", False) or not self.address:
            return False
        freed = bool(self.api.host.storage_free(_void_p(self._storage), self.mutable_c_handle()))
        if freed:
            self.address = 0
            self._owns_handle = False
        return freed

    def emplace_child(self) -> MutableExtrusionEntity:
        child = StoredExtrusionEntity(self.api, self._storage)
        idx = self.add_child_move(child)
        if _is_invalid_index(idx):
            child.free_from_storage()
            raise ValueError("failed to emplace extrusion child")
        return self.child_mutable(idx)

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.free_from_storage()

    def __del__(self) -> None:
        try:
            self.free_from_storage()
        except Exception:
            pass


class MedialAxisExtrusionFactory:
    """
    Fluent builder for expolygon_medial_axis_extrusion().

    The C ABI takes one large struct because medial-axis detection and extrusion
    conversion share the same physical assumptions. Python plugins should start
    from this factory, change only the fields they need, then call build() to
    receive a StoredExtrusionEntity.
    """

    def __init__(self, api, role: int, flow: CFlow) -> None:
        self.api = api
        self.params = CMedialAxisExtrusionParams()
        self.params.role = int(role)
        self.params.flow = flow
        self.params.min_medial_width = int(flow.width)
        self.params.max_medial_width = max(int(flow.width), int(flow.spacing))
        self.params.flags = MEDIAL_AXIS_EXTRUSION_TRIM_THIN_ENDPOINTS

    def medial_widths(self, min_width: int, max_width: int):
        self.params.min_medial_width = int(min_width)
        self.params.max_medial_width = int(max_width)
        return self

    def extrusion_widths(self, min_width: int, max_width: int):
        self.params.min_extrusion_width = int(min_width)
        self.params.max_extrusion_width = int(max_width)
        return self

    def min_centerline_length(self, value: int):
        self.params.min_centerline_length = int(value)
        return self

    def extension_area(self, expolygon):
        self.params.extension_area = expolygon.c_handle() if hasattr(expolygon, "c_handle") else _void_p(expolygon)
        return self

    def endpoint_extension(self, length: int):
        self.params.endpoint_extension_length = int(length)
        return self

    def endpoint_taper(self, length: int):
        self.params.endpoint_taper_length = int(length)
        return self

    def role(self, value: int):
        self.params.role = int(value)
        return self

    def flow(self, value: CFlow):
        self.params.flow = value
        return self

    def variable_width_resolution(self, value: int):
        self.params.variable_width_resolution = int(value)
        return self

    def width_change_tolerance(self, value: int):
        self.params.width_change_tolerance = int(value)
        return self

    def min_extrusion_length(self, value: int):
        self.params.min_extrusion_length = int(value)
        return self

    def trim_thin_endpoints(self, enabled: bool = True):
        return self._set_flag(MEDIAL_AXIS_EXTRUSION_TRIM_THIN_ENDPOINTS, enabled)

    def can_reverse(self, enabled: bool = True):
        return self._set_flag(MEDIAL_AXIS_EXTRUSION_CAN_REVERSE, enabled)

    def constant_width(self, enabled: bool = True):
        return self._set_flag(MEDIAL_AXIS_EXTRUSION_CONSTANT_WIDTH, enabled)

    def keep_empty_root(self, enabled: bool = True):
        return self._set_flag(MEDIAL_AXIS_EXTRUSION_KEEP_EMPTY_ROOT, enabled)

    def _build_handle(self, storage, expolygon) -> int:
        expolygon_handle = expolygon.c_handle() if hasattr(expolygon, "c_handle") else _void_p(expolygon)
        return _address(self.api.host.expolygon_medial_axis_extrusion(
            _void_p(storage), expolygon_handle, ctypes.byref(self.params)
        ))

    def build(self, storage, expolygon) -> StoredExtrusionEntity:
        """Return a valid entity, even when the medial axis finds no paths.

        This mirrors the C++ helper: callers that do not care whether the work
        produced geometry can keep using the returned entity as a normal empty
        container.
        """
        entity = self.try_build(storage, expolygon)
        if entity is not None:
            return entity
        return StoredExtrusionEntity(self.api, storage)

    def try_build(self, storage, expolygon) -> StoredExtrusionEntity | None:
        """Return None when the input area does not produce printable paths."""
        handle = self._build_handle(storage, expolygon)
        if not handle:
            return None
        return StoredExtrusionEntity.adopt_owned(self.api, storage, handle)

    def _set_flag(self, flag: int, enabled: bool):
        if enabled:
            self.params.flags |= int(flag)
        else:
            self.params.flags &= ~int(flag)
        return self


def medial_axis_extrusion(api, role: int, flow: CFlow) -> MedialAxisExtrusionFactory:
    return MedialAxisExtrusionFactory(api, role, flow)


def medial_axis_thin_wall(api, flow: CFlow) -> MedialAxisExtrusionFactory:
    return MedialAxisExtrusionFactory(api, RAW_EXTRUSION_ROLE_THIN_WALL, flow)


def medial_axis_gap_fill(api, flow: CFlow) -> MedialAxisExtrusionFactory:
    return MedialAxisExtrusionFactory(api, RAW_EXTRUSION_ROLE_GAP_FILL, flow)


def register_extrusion_property_type(api, namespaced_name: str, payload_cls) -> int:
    return int(api.host.extrusion_property_register_type(
        api.orchestrator,
        namespaced_name.encode("utf-8"),
        ctypes.sizeof(payload_cls),
        ctypes.alignment(payload_cls),
    ))


__all__ = [
    "CExtrusionPropertyAttributes",
    "CExtrusionPropertyCustomGcode",
    "CExtrusionPropertyExtrusionAxis",
    "CExtrusionPropertyInfill",
    "CExtrusionPropertyModifier",
    "CExtrusionPropertyOverhang",
    "CExtrusionPropertyPerimeter",
    "CExtrusionPropertySpecialCommand",
    "CExtrusionPropertySpeed",
    "CExtrusionPropertyZOffset",
    "CExtrusionSegment",
    "CFlow",
    "CMedialAxisExtrusionParams",
    "EPropertyAttributes",
    "EPropertyCustomGcode",
    "EPropertyExtrusionAxis",
    "EPropertyInfill",
    "EPropertyModifier",
    "EPropertyOverhang",
    "EPropertyPerimeter",
    "EPropertySpecialCommand",
    "EPropertySpeed",
    "EPropertyZOffset",
    "EXTRUSION_DATA_ID_INVALID",
    "EXTRUSION_INDEX_INVALID",
    "EXTRUSION_PROPERTY_TYPE_ATTRIBUTES",
    "EXTRUSION_PROPERTY_TYPE_CUSTOM_GCODE",
    "EXTRUSION_PROPERTY_TYPE_EXTRUSION_AXIS",
    "EXTRUSION_PROPERTY_TYPE_INFILL",
    "EXTRUSION_PROPERTY_TYPE_INVALID",
    "EXTRUSION_PROPERTY_TYPE_MODIFIER",
    "EXTRUSION_PROPERTY_TYPE_OVERHANG",
    "EXTRUSION_PROPERTY_TYPE_PERIMETER",
    "EXTRUSION_PROPERTY_TYPE_SPECIAL_COMMAND",
    "EXTRUSION_PROPERTY_TYPE_SPEED",
    "EXTRUSION_PROPERTY_TYPE_Z_OFFSET",
    "ExtrusionAreaFragment",
    "ExtrusionEntity",
    "MEDIAL_AXIS_EXTRUSION_CAN_REVERSE",
    "MEDIAL_AXIS_EXTRUSION_CONSTANT_WIDTH",
    "MEDIAL_AXIS_EXTRUSION_KEEP_EMPTY_ROOT",
    "MEDIAL_AXIS_EXTRUSION_TRIM_THIN_ENDPOINTS",
    "MedialAxisExtrusionFactory",
    "MutableExtrusionEntity",
    "RAW_EXTRUSION_FLAG_REVERSIBLE",
    "RAW_EXTRUSION_FLAG_SORTABLE",
    "RAW_EXTRUSION_EXISTING_PROPERTIES_KEEP_ON_PARENT",
    "RAW_EXTRUSION_EXISTING_PROPERTIES_MOVE_WITH_CONTENT",
    "RAW_EXTRUSION_ORDERED_LEAF_AFTER",
    "RAW_EXTRUSION_ORDERED_LEAF_BEFORE",
    "StoredExtrusionEntity",
    "medial_axis_extrusion",
    "medial_axis_gap_fill",
    "medial_axis_thin_wall",
    "point_tuple",
    "register_extrusion_property_type",
]
