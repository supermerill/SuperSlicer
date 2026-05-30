#/|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
#/|/
#/|/ SuperSlicer is released under the terms of the AGPLv3 or higher
#/|/

"""
Python views over the SuperSlicer print data tree.

The data tree is the host-owned hierarchy a plugin usually receives through a
step payload:

    Print -> Object -> Layer -> LayerIsland / LayerRegion -> LayerRegionIsland

The wrappers here keep raw C handles out of normal plugin code. Use read-only
views for borrowed const handles, and Mutable* views only when a step payload or
host callback explicitly gives a mutable handle.

Typical use
-----------

Read layers and islands:

    obj = api.object(ctx.object)
    for layer in obj.layers():
        for island in layer.islands():
            bbox = island.bounding_box()

Read configuration:

    config = obj.config()
    density = config.float("fill_density")
    enabled = config.bool("support_material")

Inspect region-island extrusions:

    region_island = island.region_island(0)
    if region_island.has_extrusion(RAW_EXTRUSION_ROLE_PERIMETER):
        extrusion = region_island.extrusion(RAW_EXTRUSION_ROLE_PERIMETER)
        print(extrusion.length())
"""

from __future__ import annotations

import ctypes
from typing import Iterator

from slic3r_api_generated import (
    CFlow,
    CFloatOrPercent,
    CMatrix4d,
    CSurface,
    CTriangleIndices,
    RAW_SURFACE_TYPE_DENS_SOLID,
    RAW_SURFACE_TYPE_DENS_SPARSE,
    RAW_SURFACE_TYPE_DENS_VOID,
    RAW_SURFACE_TYPE_MOD_BRIDGE,
    RAW_SURFACE_TYPE_MOD_OVERBRIDGE,
    RAW_SURFACE_TYPE_NONE,
    RAW_SURFACE_TYPE_POS_BOTTOM,
    RAW_SURFACE_TYPE_POS_INTERNAL,
    RAW_SURFACE_TYPE_POS_PERIMETER,
    RAW_SURFACE_TYPE_POS_TOP,
    SCALING_FACTOR,
)

from slic3r_extrusion_views import ExtrusionEntity, MutableExtrusionEntity
from slic3r_geometry_views import CPoint, ExPolygon, ExPolygonCollection


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


def _as_bytes(text: str) -> bytes:
    return text.encode("utf-8")


def _optional(cls, api, handle):
    return None if not _address(handle) else cls(api, handle)


def _decode_const_string(ptr) -> str:
    if not ptr:
        return ""
    return ctypes.cast(ptr, ctypes.c_char_p).value.decode("utf-8", errors="replace")


def unscaled(value: int | float) -> float:
    return float(value) * SCALING_FACTOR


# Base for borrowed data-tree views. It never owns or frees the handle.
class DataTreeView:
    def __init__(self, api, handle) -> None:
        self.api = api
        self.address = _require_handle(handle)

    def c_handle(self) -> ctypes.c_void_p:
        return _void_p(self.address)

    def same_handle(self, other: "DataTreeView") -> bool:
        return self.address == other.address

    def __bool__(self) -> bool:
        return self.address != 0


# Read-only view over one ConfigOption. It covers common scalar/vector access
# without exposing raw config_option_handle values.
class ConfigOption(DataTreeView):
    def type(self) -> int:
        return int(self.api.host.config_option_type_get(self.c_handle()))

    def flags(self) -> int:
        return int(self.api.host.config_option_flags_get(self.c_handle()))

    def size(self) -> int:
        return int(self.api.host.config_option_size(self.c_handle()))

    def is_scalar(self) -> bool:
        return bool(self.api.host.config_option_is_scalar(self.c_handle()))

    def is_vector(self) -> bool:
        return bool(self.api.host.config_option_is_vector(self.c_handle()))

    def is_enabled(self, idx: int = 0) -> bool:
        return bool(self.api.host.config_option_is_enabled(self.c_handle(), int(idx)))

    def can_be_disabled(self) -> bool:
        return bool(self.api.host.config_option_can_be_disabled(self.c_handle()))

    def is_phony(self) -> bool:
        return bool(self.api.host.config_option_is_phony(self.c_handle()))

    def get_bool(self, idx: int = 0) -> bool:
        return bool(self.api.host.config_option_get_bool(self.c_handle(), int(idx)))

    def get_int(self, idx: int = 0) -> int:
        return int(self.api.host.config_option_get_int(self.c_handle(), int(idx)))

    def get_float(self, idx: int = 0) -> float:
        return float(self.api.host.config_option_get_float(self.c_handle(), int(idx)))

    def get_float_or_percent(self, idx: int = 0) -> CFloatOrPercent:
        return self.api.host.config_option_get_float_or_percent(self.c_handle(), int(idx))

    def is_percent(self, idx: int = 0) -> bool:
        return bool(self.get_float_or_percent(idx).percent)

    def get_effective_value(self, ratio_over: float, idx: int = 0) -> float:
        value = self.get_float_or_percent(idx)
        return float(self.api.host.c_float_or_percent_get_effective_value(ctypes.byref(value), float(ratio_over)))

    def serialize(self) -> str:
        needed = int(self.api.host.config_option_serialize(self.c_handle(), None, 0))
        if needed == 0:
            return ""
        out = ctypes.create_string_buffer(needed + 1)
        written = int(self.api.host.config_option_serialize(self.c_handle(), out, len(out)))
        return out.value[:written].decode("utf-8", errors="replace")

    def vector_empty(self) -> bool:
        vector = self.api.host.config_option_vector_cast(self.c_handle())
        return not vector or bool(self.api.host.config_option_vector_empty(vector))

    def vector_serialize_at(self, idx: int) -> str:
        vector = self.api.host.config_option_vector_cast(self.c_handle())
        if not vector:
            return ""
        needed = int(self.api.host.config_option_vector_serialize_at(vector, int(idx), None, 0))
        if needed == 0:
            return ""
        out = ctypes.create_string_buffer(needed + 1)
        written = int(self.api.host.config_option_vector_serialize_at(vector, int(idx), out, len(out)))
        return out.value[:written].decode("utf-8", errors="replace")


# Mutable config option. Use only on a MutableConfig result; changing options may
# require the host step to invalidate/recompute dependent data.
class MutableConfigOption(ConfigOption):
    def mutable_c_handle(self) -> ctypes.c_void_p:
        return self.c_handle()

    def set_enabled(self, enabled: bool, idx: int = 0) -> bool:
        return bool(self.api.host.config_option_set_enabled(self.mutable_c_handle(), int(enabled), int(idx)))

    def set_can_be_disabled(self, enabled: bool) -> bool:
        return bool(self.api.host.config_option_set_can_be_disabled(self.mutable_c_handle(), int(enabled)))

    def set_phony(self, phony: bool) -> bool:
        return bool(self.api.host.config_option_set_phony(self.mutable_c_handle(), int(phony)))

    def copy_from(self, src: ConfigOption) -> None:
        self.api.host.config_option_copy(self.mutable_c_handle(), src.c_handle())

    def deserialize(self, text: str, append: bool = False) -> bool:
        return bool(self.api.host.config_option_deserialize(self.mutable_c_handle(), _as_bytes(text), int(append)))

    def set_int(self, value: int, idx: int = 0) -> None:
        self.api.host.config_option_set_int(self.mutable_c_handle(), int(value), int(idx))

    def set_float(self, value: float, idx: int = 0) -> None:
        self.api.host.config_option_set_float(self.mutable_c_handle(), float(value), int(idx))

    def set_float_or_percent(self, value: CFloatOrPercent, idx: int = 0) -> None:
        self.api.host.config_option_set_float_or_percent(self.mutable_c_handle(), value, int(idx))

    def set_bool(self, value: bool, idx: int = 0) -> None:
        self.api.host.config_option_set_bool(self.mutable_c_handle(), int(value), int(idx))


# Borrowed read-only config view. get() returns None for an unknown key.
class Config(DataTreeView):
    def keys(self) -> list[str]:
        raw = self.api.host.config_keys(self.c_handle())
        out: list[str] = []
        for idx in range(raw.size):
            out.append(_decode_const_string(raw.items[idx]))
        return out

    def get(self, key: str) -> ConfigOption | None:
        return _optional(ConfigOption, self.api, self.api.host.config_get(self.c_handle(), _as_bytes(key)))

    def option(self, key: str) -> ConfigOption:
        option = self.get(key)
        if option is None:
            raise KeyError(key)
        return option

    def bool(self, key: str, idx: int = 0, default: bool = False) -> bool:
        option = self.get(key)
        return default if option is None else option.get_bool(idx)

    def int(self, key: str, idx: int = 0, default: int = 0) -> int:
        option = self.get(key)
        return default if option is None else option.get_int(idx)

    def float(self, key: str, idx: int = 0, default: float = 0.0) -> float:
        option = self.get(key)
        return default if option is None else option.get_float(idx)

    def float_or_percent(self, key: str, idx: int = 0) -> CFloatOrPercent | None:
        option = self.get(key)
        return None if option is None else option.get_float_or_percent(idx)


# Mutable config view. Only use it when the host explicitly exposes a mutable
# config handle to this plugin step.
class MutableConfig(Config):
    def mutable_c_handle(self) -> ctypes.c_void_p:
        return self.c_handle()

    def get_mutable(self, key: str) -> MutableConfigOption | None:
        return _optional(MutableConfigOption, self.api, self.api.host.config_get_mutable(self.mutable_c_handle(), _as_bytes(key)))

    def mutable_option(self, key: str) -> MutableConfigOption:
        option = self.get_mutable(key)
        if option is None:
            raise KeyError(key)
        return option


# Small builder for raw_surface_type. It mirrors the intended order:
# position -> density -> optional modifiers.
class SurfaceTypeBuilder:
    def __init__(self, value: int = RAW_SURFACE_TYPE_NONE) -> None:
        self.value = int(value)

    def top(self) -> "SurfaceTypeBuilder":
        return SurfaceTypeBuilder(self.value | RAW_SURFACE_TYPE_POS_TOP)

    def bottom(self) -> "SurfaceTypeBuilder":
        return SurfaceTypeBuilder(self.value | RAW_SURFACE_TYPE_POS_BOTTOM)

    def internal(self) -> "SurfaceTypeBuilder":
        return SurfaceTypeBuilder(self.value | RAW_SURFACE_TYPE_POS_INTERNAL)

    def perimeter(self) -> "SurfaceTypeBuilder":
        return SurfaceTypeBuilder(self.value | RAW_SURFACE_TYPE_POS_PERIMETER)

    def solid(self) -> "SurfaceTypeBuilder":
        return SurfaceTypeBuilder(self.value | RAW_SURFACE_TYPE_DENS_SOLID)

    def sparse(self) -> "SurfaceTypeBuilder":
        return SurfaceTypeBuilder(self.value | RAW_SURFACE_TYPE_DENS_SPARSE)

    def empty(self) -> "SurfaceTypeBuilder":
        return SurfaceTypeBuilder(self.value | RAW_SURFACE_TYPE_DENS_VOID)

    def bridge(self) -> "SurfaceTypeBuilder":
        return SurfaceTypeBuilder(self.value | RAW_SURFACE_TYPE_MOD_BRIDGE)

    def overbridge(self) -> "SurfaceTypeBuilder":
        return SurfaceTypeBuilder(self.value | RAW_SURFACE_TYPE_MOD_OVERBRIDGE)

    def raw(self) -> int:
        return self.value

    def __int__(self) -> int:
        return self.value


def srf_type() -> SurfaceTypeBuilder:
    return SurfaceTypeBuilder()


# Borrowed surface view. It can either wrap a host surface_handle or a c_surface
# snapshot returned by the ABI.
class Surface:
    def __init__(self, api, handle=None, surface: CSurface | None = None) -> None:
        if handle is None and surface is None:
            raise ValueError("surface handle or c_surface is required")
        self.api = api
        self.address = _address(handle)
        self._surface = surface

    @classmethod
    def from_c_surface(cls, api, surface: CSurface) -> "Surface":
        return cls(api, surface=surface)

    def has_handle(self) -> bool:
        return self.address != 0

    def c_handle(self) -> ctypes.c_void_p:
        return _void_p(self.address)

    def c_view(self) -> CSurface:
        if self.has_handle():
            return self.api.host.surface_c_view(self.c_handle())
        return self._surface

    def expolygon(self) -> ExPolygon:
        handle = self.api.host.surface_get_expolygon(self.c_handle()) if self.has_handle() else self._surface.expolygon
        return ExPolygon(self.api, handle)

    def type(self) -> int:
        return int(self.api.host.surface_get_type(self.c_handle())) if self.has_handle() else int(self._surface.type)

    def has_flag(self, flag: int) -> bool:
        if self.has_handle():
            return bool(self.api.host.surface_get_flag(self.c_handle(), int(flag)))
        return (self.type() & int(flag)) != 0


# Borrowed read-only surface collection.
class SurfaceCollection(DataTreeView):
    def size(self) -> int:
        return int(self.api.host.surface_collection_size(self.c_handle()))

    def empty(self) -> bool:
        return self.size() == 0

    def at(self, idx: int) -> Surface:
        return Surface(self.api, self.api.host.surface_collection_at(self.c_handle(), int(idx)))

    def front(self) -> Surface:
        if self.empty():
            raise IndexError("empty surface collection")
        return self.at(0)

    def back(self) -> Surface:
        if self.empty():
            raise IndexError("empty surface collection")
        return self.at(self.size() - 1)

    def __len__(self) -> int:
        return self.size()

    def __iter__(self) -> Iterator[Surface]:
        for idx in range(self.size()):
            yield self.at(idx)

    def __getitem__(self, idx: int) -> Surface:
        if idx < 0:
            idx += self.size()
        if idx < 0 or idx >= self.size():
            raise IndexError(idx)
        return self.at(idx)


class TriangleMesh(DataTreeView):
    """
    Borrowed read-only triangle mesh view.

    Mesh vertices use unscaled millimeters, unlike most 2D geometry in the
    slicing data tree. Slicing plugins usually get this through Volume.mesh().
    """

    def vertex_count(self) -> int:
        return int(self.api.host.triangle_mesh_vertex_count(self.c_handle()))

    def triangle_count(self) -> int:
        return int(self.api.host.triangle_mesh_triangle_count(self.c_handle()))

    def vertex_at(self, idx: int):
        return self.api.host.triangle_mesh_vertex_at(self.c_handle(), int(idx))

    def triangle_at(self, idx: int) -> CTriangleIndices:
        return self.api.host.triangle_mesh_triangle_at(self.c_handle(), int(idx))


class Volume(DataTreeView):
    """
    Borrowed read-only model volume view.

    Volumes are the raw model inputs used by STEP_SLICING. They expose the mesh,
    transform and volume-level config needed to decide how a slicing plugin
    should write layer-region polygons.
    """

    def type(self) -> int:
        return int(self.api.host.volume_get_type(self.c_handle()))

    def id(self) -> int:
        return int(self.api.host.volume_get_id(self.c_handle()))

    def config(self) -> Config:
        return Config(self.api, self.api.host.volume_get_config(self.c_handle()))

    def extruder_id(self) -> int:
        return int(self.api.host.volume_get_extruder_id(self.c_handle()))

    def matrix(self) -> CMatrix4d:
        return self.api.host.volume_get_matrix(self.c_handle())

    def matrix_no_offset(self) -> CMatrix4d:
        return self.api.host.volume_get_matrix_no_offset(self.c_handle())

    def has_painting(self, paint_key: str) -> bool:
        return bool(self.api.host.volume_has_painting(self.c_handle(), _as_bytes(paint_key)))

    def mesh(self) -> TriangleMesh:
        return TriangleMesh(self.api, self.api.host.volume_get_mesh(self.c_handle()))


# Borrowed print-region view.
class PrintRegion(DataTreeView):
    def config(self) -> Config:
        return Config(self.api, self.api.host.print_region_get_config(self.c_handle()))


class MutablePrintRegion(PrintRegion):
    def mutable_c_handle(self) -> ctypes.c_void_p:
        return self.c_handle()

    def config_mutable(self) -> MutableConfig:
        return MutableConfig(self.api, self.api.host.print_region_get_config_mutable(self.mutable_c_handle()))


# Borrowed layer-region view.
class LayerRegion(DataTreeView):
    def flow(self, role: int) -> CFlow:
        return self.api.host.layer_region_get_flow(self.c_handle(), int(role))

    def slices(self) -> ExPolygonCollection:
        return ExPolygonCollection(self.api, self.api.host.layer_region_get_slices(self.c_handle()))

    def bounding_box(self):
        return self.api.host.layer_region_get_bounding_box(self.c_handle())

    def print_region(self) -> PrintRegion:
        return PrintRegion(self.api, self.api.host.layer_region_get_print_region(self.c_handle()))

    def layer(self) -> "Layer":
        return Layer(self.api, self.api.host.layer_region_get_layer(self.c_handle()))


class MutableLayerRegion(LayerRegion):
    def mutable_c_handle(self) -> ctypes.c_void_p:
        return self.c_handle()


# Borrowed layer-region island view. It is the usual entry point for extrusion
# trees attached to one island/region pair.
class LayerRegionIsland(DataTreeView):
    def extruder_id(self) -> int:
        return int(self.api.host.layer_region_island_extruder_id(self.c_handle()))

    def has_extrusions(self) -> bool:
        return bool(self.api.host.layer_region_island_has_extrusions(self.c_handle()))

    def has_extrusion(self, role: int) -> bool:
        return bool(self.api.host.layer_region_island_has_extrusion(self.c_handle(), int(role)))

    def extrusion(self, role: int) -> ExtrusionEntity | None:
        return _optional(ExtrusionEntity, self.api, self.api.host.layer_region_island_get_extrusion(self.c_handle(), int(role)))

    def fill_surfaces_collection(self) -> SurfaceCollection:
        return SurfaceCollection(self.api, self.api.host.layer_region_island_get_fill_surfaces(self.c_handle()))

    def fill_surface_count(self) -> int:
        return int(self.api.host.layer_region_island_count_fill_surface(self.c_handle()))

    def fill_surface(self, idx: int) -> Surface:
        return Surface(self.api, self.api.host.layer_region_island_get_fill_surface(self.c_handle(), int(idx)))

    def fill_surfaces(self) -> list[Surface]:
        return [self.fill_surface(idx) for idx in range(self.fill_surface_count())]


class MutableLayerRegionIsland(LayerRegionIsland):
    def mutable_c_handle(self) -> ctypes.c_void_p:
        return self.c_handle()

    def extrusion_mutable(self, role: int) -> MutableExtrusionEntity | None:
        return _optional(
            MutableExtrusionEntity,
            self.api,
            self.api.host.layer_region_island_get_mutable_extrusion(self.mutable_c_handle(), int(role)),
        )


# Borrowed layer island view.
class LayerIsland(DataTreeView):
    def slice(self) -> ExPolygon:
        return ExPolygon(self.api, self.api.host.layer_island_get_slice(self.c_handle()))

    def bounding_box(self):
        return self.api.host.layer_island_get_bounding_box(self.c_handle())

    def infill_slice(self) -> ExPolygon:
        return ExPolygon(self.api, self.api.host.layer_island_get_infill_slice(self.c_handle()))

    def infill_areas(self) -> ExPolygonCollection:
        return ExPolygonCollection(self.api, self.api.host.layer_island_get_infill_areas(self.c_handle()))

    def infill_bounding_box(self):
        return self.api.host.layer_island_get_infill_bounding_box(self.c_handle())

    def infill_no_overlap_slice(self) -> ExPolygon:
        return ExPolygon(self.api, self.api.host.layer_island_get_infill_no_overlap_slice(self.c_handle()))

    def infill_no_overlap_areas(self) -> ExPolygonCollection:
        return ExPolygonCollection(self.api, self.api.host.layer_island_get_infill_no_overlap_areas(self.c_handle()))

    def region_count(self) -> int:
        return int(self.api.host.layer_island_count_region(self.c_handle()))

    def region(self, idx: int) -> LayerRegion:
        return LayerRegion(self.api, self.api.host.layer_island_get_region(self.c_handle(), int(idx)))

    def regions(self) -> Iterator[LayerRegion]:
        for idx in range(self.region_count()):
            yield self.region(idx)

    def region_island_count(self) -> int:
        return int(self.api.host.layer_island_count_region_island(self.c_handle()))

    def region_island(self, idx: int) -> LayerRegionIsland:
        return LayerRegionIsland(self.api, self.api.host.layer_island_get_region_island(self.c_handle(), int(idx)))

    def region_islands(self) -> Iterator[LayerRegionIsland]:
        for idx in range(self.region_island_count()):
            yield self.region_island(idx)

    def layer(self) -> "Layer":
        return Layer(self.api, self.api.host.layer_island_get_layer(self.c_handle()))

    def lower_island_count(self) -> int:
        return int(self.api.host.layer_island_count_lower_island(self.c_handle()))

    def lower_island(self, idx: int) -> "LayerIsland":
        return LayerIsland(self.api, self.api.host.layer_island_get_lower_island(self.c_handle(), int(idx)))

    def lower_islands(self) -> Iterator["LayerIsland"]:
        for idx in range(self.lower_island_count()):
            yield self.lower_island(idx)

    def upper_island_count(self) -> int:
        return int(self.api.host.layer_island_count_upper_island(self.c_handle()))

    def upper_island(self, idx: int) -> "LayerIsland":
        return LayerIsland(self.api, self.api.host.layer_island_get_upper_island(self.c_handle(), int(idx)))

    def upper_islands(self) -> Iterator["LayerIsland"]:
        for idx in range(self.upper_island_count()):
            yield self.upper_island(idx)


class MutableLayerIsland(LayerIsland):
    def mutable_c_handle(self) -> ctypes.c_void_p:
        return self.c_handle()

    def slice_mutable(self) -> ExPolygon:
        return ExPolygon(self.api, self.api.host.layer_island_get_slice_mutable(self.mutable_c_handle()))

    def region_mutable(self, idx: int) -> MutableLayerRegion:
        return MutableLayerRegion(self.api, self.api.host.layer_island_get_region_mutable(self.mutable_c_handle(), int(idx)))

    def region_island_mutable(self, idx: int) -> MutableLayerRegionIsland:
        return MutableLayerRegionIsland(
            self.api,
            self.api.host.layer_island_get_region_island_mutable(self.mutable_c_handle(), int(idx)),
        )


# Borrowed layer view.
class Layer(DataTreeView):
    def height(self) -> int:
        return int(self.api.host.layer_get_height(self.c_handle()))

    def print_z(self) -> int:
        return int(self.api.host.layer_get_print_z(self.c_handle()))

    def slice_z(self) -> int:
        return int(self.api.host.layer_get_slice_z(self.c_handle()))

    def support_id(self) -> int:
        return int(self.api.host.layer_get_support_id(self.c_handle()))

    def upper_layer(self) -> "Layer | None":
        return _optional(Layer, self.api, self.api.host.layer_get_upper_layer(self.c_handle()))

    def lower_layer(self) -> "Layer | None":
        return _optional(Layer, self.api, self.api.host.layer_get_lower_layer(self.c_handle()))

    def region_count(self) -> int:
        return int(self.api.host.layer_count_region(self.c_handle()))

    def island_count(self) -> int:
        return int(self.api.host.layer_count_island(self.c_handle()))

    def slices(self) -> ExPolygonCollection:
        return ExPolygonCollection(self.api, self.api.host.layer_get_slices(self.c_handle()))

    def region(self, idx: int) -> LayerRegion:
        return LayerRegion(self.api, self.api.host.layer_get_region(self.c_handle(), int(idx)))

    def regions(self) -> Iterator[LayerRegion]:
        for idx in range(self.region_count()):
            yield self.region(idx)

    def island(self, idx: int) -> LayerIsland:
        return LayerIsland(self.api, self.api.host.layer_get_island(self.c_handle(), int(idx)))

    def islands(self) -> Iterator[LayerIsland]:
        for idx in range(self.island_count()):
            yield self.island(idx)


class MutableLayer(Layer):
    def mutable_c_handle(self) -> ctypes.c_void_p:
        return self.c_handle()

    def upper_layer_mutable(self) -> "MutableLayer | None":
        return _optional(MutableLayer, self.api, self.api.host.layer_get_upper_layer_mutable(self.mutable_c_handle()))

    def lower_layer_mutable(self) -> "MutableLayer | None":
        return _optional(MutableLayer, self.api, self.api.host.layer_get_lower_layer_mutable(self.mutable_c_handle()))

    def region_mutable(self, idx: int) -> MutableLayerRegion:
        return MutableLayerRegion(self.api, self.api.host.layer_get_region_mutable(self.mutable_c_handle(), int(idx)))

    def island_mutable(self, idx: int) -> MutableLayerIsland:
        return MutableLayerIsland(self.api, self.api.host.layer_get_island_mutable(self.mutable_c_handle(), int(idx)))


# Borrowed print object view.
class Object(DataTreeView):
    def config(self) -> Config:
        return Config(self.api, self.api.host.object_get_config(self.c_handle()))

    def max_z(self) -> int:
        return int(self.api.host.object_get_max_z(self.c_handle()))

    def transform(self) -> CMatrix4d:
        return self.api.host.object_get_transform(self.c_handle())

    def center_offset(self) -> CPoint:
        return self.api.host.object_get_center_offset(self.c_handle())

    def transform_centered(self) -> CMatrix4d:
        offset = self.center_offset()
        translation = self.api.host.matrix4d_translation(-unscaled(offset.x), -unscaled(offset.y), 0.0)
        return self.api.host.matrix4d_mul(translation, self.transform())

    def layer_count(self) -> int:
        return int(self.api.host.object_count_layer(self.c_handle()))

    def layer(self, idx: int) -> Layer:
        return Layer(self.api, self.api.host.object_get_layer(self.c_handle(), int(idx)))

    def layers(self) -> Iterator[Layer]:
        for idx in range(self.layer_count()):
            yield self.layer(idx)

    def volume_count(self) -> int:
        return int(self.api.host.object_volume_count(self.c_handle()))

    def volume(self, idx: int) -> Volume:
        return Volume(self.api, self.api.host.object_volume_at(self.c_handle(), int(idx)))

    def volumes(self) -> Iterator[Volume]:
        for idx in range(self.volume_count()):
            yield self.volume(idx)

    def print_region_count(self) -> int:
        return int(self.api.host.object_count_region(self.c_handle()))

    def print_region(self, idx: int) -> PrintRegion:
        return PrintRegion(self.api, self.api.host.object_get_print_region(self.c_handle(), int(idx)))

    def print_regions(self) -> Iterator[PrintRegion]:
        for idx in range(self.print_region_count()):
            yield self.print_region(idx)


class MutableObject(Object):
    def mutable_c_handle(self) -> ctypes.c_void_p:
        return self.c_handle()

    def config_mutable(self) -> MutableConfig:
        return MutableConfig(self.api, self.api.host.object_get_config_mutable(self.mutable_c_handle()))

    def layer_mutable(self, idx: int) -> MutableLayer:
        return MutableLayer(self.api, self.api.host.object_get_layer_mutable(self.mutable_c_handle(), int(idx)))

    def print_region_mutable(self, idx: int) -> MutablePrintRegion:
        return MutablePrintRegion(self.api, self.api.host.object_get_print_region_mutable(self.mutable_c_handle(), int(idx)))


# Borrowed print view.
class Print(DataTreeView):
    def config(self) -> Config:
        return Config(self.api, self.api.host.print_get_config(self.c_handle()))

    def object_count(self) -> int:
        return int(self.api.host.print_count_object(self.c_handle()))

    def object(self, idx: int) -> Object:
        return Object(self.api, self.api.host.print_get_object(self.c_handle(), int(idx)))

    def objects(self) -> Iterator[Object]:
        for idx in range(self.object_count()):
            yield self.object(idx)


class MutablePrint(Print):
    def mutable_c_handle(self) -> ctypes.c_void_p:
        return self.c_handle()

    def config_mutable(self) -> MutableConfig:
        return MutableConfig(self.api, self.api.host.print_get_config_mutable(self.mutable_c_handle()))

    def object_mutable(self, idx: int) -> MutableObject:
        return MutableObject(self.api, self.api.host.print_get_object_mutable(self.mutable_c_handle(), int(idx)))


__all__ = [
    "Config",
    "ConfigOption",
    "DataTreeView",
    "Layer",
    "LayerIsland",
    "LayerRegion",
    "LayerRegionIsland",
    "MutableConfig",
    "MutableConfigOption",
    "MutableLayer",
    "MutableLayerIsland",
    "MutableLayerRegion",
    "MutableLayerRegionIsland",
    "MutableObject",
    "MutablePrint",
    "MutablePrintRegion",
    "Object",
    "Print",
    "PrintRegion",
    "Surface",
    "SurfaceCollection",
    "SurfaceTypeBuilder",
    "TriangleMesh",
    "Volume",
    "srf_type",
    "unscaled",
]
