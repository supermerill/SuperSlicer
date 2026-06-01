#/|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
#/|/
#/|/ SuperSlicer is released under the terms of the AGPLv3 or higher
#/|/

"""
Small Python helper layer for SuperSlicer plugins.

Python plugins are loaded by python_plugin_loader.dll. The loader imports every
script from plugins/python/plugins and calls:

    register_plugin(api: Slic3rAPI)

The function may return one PluginBase instance, a list/tuple of PluginBase
instances, or None if the script only registers settings/UI fragments.

The C ABI vtable is built by the loader DLL, not by Python. This avoids fragile
callback signatures while keeping Python plugin code simple. Slic3rAPI binds the
public host C functions with ctypes; raw functions are available as
api.host.function_name(...), and a smaller set of Python-friendly helpers is
provided for common tasks. Handles and run contexts are exposed as integer
addresses.

Plugins should import this module from the Python runtime directory prepared by
the loader:

    from slic3r_api import PluginBase, STEP_POST_SLICING

Geometry helpers are re-exported from slic3r_geometry_views.py. They wrap raw
handles in Python objects while leaving the raw C ABI available:

    polygon = api.polygon(polygon_handle)
    owned_polyline = api.new_polyline(storage_handle)
    owned_polyline.push_back(make_point(0, 0))

Extrusion helpers follow the same pattern:

    entity = api.extrusion(entity_handle)
    owned_entity = api.new_extrusion(storage_handle)
    owned_entity.set_points([make_point(0, 0), make_point(1000000, 0)])
    thin_wall = api.medial_axis_thin_wall(flow).medial_widths(min_w, max_w).build(storage_handle, expolygon)

Clipper helpers wrap temporary Clipper operands bound to a storage_handle:

    clip = api.clipper(storage_handle)
    result = clip.diff(clip(subject_polygons), clip(mask_expolygons))
    result.write_expolygons_to(output_expolygons)

RegionSettings helpers let a plugin process a whole LayerIsland while still
splitting work where region/modifier settings differ:

    settings = RegionSettings(api, storage_handle, island, [["fuzzy_skin"]])
    settings.segregate(island.slice())
    for value, area_clip in settings.get_areas("fuzzy_skin"):
        selected = area_clip.intersections(candidate_areas)

Do not copy this file, or slic3r_api_generated.py, into each plugin. The loader
adds both plugins/python and plugins/python/plugins to sys.path before loading
plugin scripts, so the shared helper module is available to all plugins.

slic3r_api_generated.py is generated from Api/plugin/c headers by
generate_slic3r_api.py. It is copied next to this file in the Python runtime
root prepared by the loader. In a development build, build python_plugin_loader
to regenerate and copy the current definitions into that runtime directory:

    cmake --build build --config Debug --target python_plugin_loader -- /m:8

For a quick Python-only import test during development, use the generated
runtime directory:

    python -c "import sys; sys.path.insert(0, r'build/src/Debug/plugins/python'); import slic3r_api"
"""

from __future__ import annotations

import ctypes
import math
from typing import Iterable, Optional

try:
    from slic3r_api_generated import *
    from slic3r_api_generated import _bind_c_function
except ModuleNotFoundError as exc:
    if exc.name != "slic3r_api_generated":
        raise
    raise ModuleNotFoundError(
        "slic3r_api_generated.py is a build artifact generated from the C plugin ABI headers. "
        "Import slic3r_api from the Python runtime root prepared by python_plugin_loader "
        "(for example build/src/<Config>/plugins/python in a development build), or run "
        "generate_slic3r_api.py with an output path next to this file for local experiments."
    ) from exc

from slic3r_geometry_views import *
from slic3r_extrusion_views import *
from slic3r_datatree_views import *
from slic3r_clipper_views import *
from slic3r_region_settings_views import *
from steps.layer_height import *
from steps.slicing import *
from steps.post_slicing import *
from steps.perimeter import *
from steps.perimeter_module import *
from steps.post_perimeter import *
from steps.post_infill import *
from steps.surface_generation import *


_registered_apis = []


def _as_bytes(text: Optional[str]) -> Optional[bytes]:
    return None if text is None else text.encode("utf-8")


def round_coord(value: float) -> int:
    return math.floor(value + 0.5) if value >= 0.0 else math.ceil(value - 0.5)


def scale_i(value: float) -> int:
    return round_coord(value * UNSCALING_FACTOR)


def unscaled(value: float) -> float:
    return float(value) * SCALING_FACTOR


def run_context(run_ctx_address: int) -> Optional[PluginRunContext]:
    if not run_ctx_address:
        return None
    return ctypes.cast(ctypes.c_void_p(run_ctx_address), ctypes.POINTER(PluginRunContext)).contents


def post_slicing_context(run_ctx_address: int) -> Optional[RunCtxPostSlicing]:
    ctx = run_context(run_ctx_address)
    if ctx is None or ctx.step != STEP_POST_SLICING or not ctx.data:
        return None
    return ctypes.cast(ctx.data, ctypes.POINTER(RunCtxPostSlicing)).contents


def is_cancelled(run_ctx_address: int) -> bool:
    ctx = run_context(run_ctx_address)
    if ctx is None or not ctx.is_cancelled:
        return False
    fn = PLUGIN_IS_CANCELLED(ctx.is_cancelled)
    return bool(fn(ctx.host_context))


def report_warning(run_ctx_address: int, message: str) -> None:
    ctx = run_context(run_ctx_address)
    if ctx is not None and ctx.report_warning:
        PLUGIN_REPORT(ctx.report_warning)(ctx.host_context, _as_bytes(message))


def report_error(run_ctx_address: int, message: str) -> None:
    ctx = run_context(run_ctx_address)
    if ctx is not None and ctx.report_error:
        PLUGIN_REPORT(ctx.report_error)(ctx.host_context, _as_bytes(message))


def report_progress(run_ctx_address: int, progress: float, message: Optional[str] = None) -> None:
    ctx = run_context(run_ctx_address)
    if ctx is not None and ctx.report_progress:
        PLUGIN_REPORT_PROGRESS(ctx.report_progress)(ctx.host_context, progress, _as_bytes(message))


def used_config_key(
    key: str,
    type: int,
    container_type: int = RAW_CONTAINER_TYPE_NONE,
    option_preset_type: int = RAW_PRESET_TYPE_NONE,
) -> tuple[str, int, int, int]:
    """
    Declare one setting read by a Python plugin.

    The type is mandatory because the host validates that the setting provider
    and the reader agree on the ConfigOption representation. container_type and
    option_preset_type are optional filters; leave them at *_NONE when the key
    may legitimately come from several places.
    """
    return (key, int(type), int(container_type), int(option_preset_type))


class PluginBase:
    """
    Base class for Python plugins.

    Set plugin_id, name, description, step, priority, dependencies and
    used_config_keys in __init__ by calling the base constructor. Entries in
    used_config_keys should be created with used_config_key(...), so the host
    can validate the expected option type. Every plugin has an exclusive group:
    when exclusive_group is not supplied, it defaults to plugin_id and behaves as
    a singleton group. If several plugins are alternatives for the same work,
    give them the same exclusive_group so the host can expose a selector and run
    only one of them. Override initialize/setup/setup_run/run as needed.

    Callback arguments are raw C pointer addresses represented as Python int:
    - initialize(storage_address)
    - setup(run_ctx_address, run_count)
    - setup_run(run_ctx_address)
    - run(run_ctx_address)

    Use run_context(), is_cancelled(), report_warning(), report_error() and
    report_progress() to inspect or use the common plugin_run_context callbacks.
    """

    def __init__(
        self,
        plugin_id: str,
        step: int,
        name: Optional[str] = None,
        description: str = "",
        priority: int = 0,
        dependencies: Iterable[str] = (),
        used_config_keys: Iterable[object] = (),
        defined_config_keys: Iterable[str] = (),
        exclusive_group: str = "",
        exclusive_group_label: str = "",
        exclusive_group_tooltip: str = "",
    ) -> None:
        self.plugin_id = plugin_id
        self.name = name or plugin_id
        self.description = description
        self.exclusive_group = exclusive_group or plugin_id
        self.exclusive_group_label = exclusive_group_label
        self.exclusive_group_tooltip = exclusive_group_tooltip
        self.step = int(step)
        self.priority = int(priority)
        self.dependencies = list(dependencies)
        self.used_config_keys = list(used_config_keys)
        self.defined_config_keys = list(defined_config_keys)

    def initialize(self, storage_address: int) -> None:
        pass

    def setup(self, run_ctx_address: int, run_count: int) -> None:
        pass

    def setup_run(self, run_ctx_address: int) -> None:
        pass

    def run(self, run_ctx_address: int) -> None:
        pass


class Slic3rAPI:
    """
    Thin wrapper around exported host C functions.

    host_library_path is usually the Slic3r.dll path provided by the Python
    loader. The wrapper keeps the ctypes.CDLL object alive, so C calls remain
    valid after register_plugin() returns.
    """

    def __init__(self, orchestrator_address: int, host_library_path: str) -> None:
        self.orchestrator = ctypes.c_void_p(orchestrator_address)
        self.host = ctypes.CDLL(host_library_path)
        self._bind_host_functions()
        _registered_apis.append(self)

    def _bind_host_functions(self) -> None:
        for name, restype, argtypes in C_FUNCTION_SIGNATURES:
            _bind_c_function(self.host, name, restype, argtypes)

    def add_ui_fragment(self, target_file: str, fragment_id: str, ui_fragment: str, priority: int = 0) -> int:
        return self.host.orchestrator_add_ui_fragment(
            self.orchestrator,
            _as_bytes(target_file),
            _as_bytes(fragment_id),
            _as_bytes(ui_fragment),
            int(priority),
        )

    def add_gui_rule(
        self,
        *,
        target_key: str,
        condition_key: str,
        action: int = RAW_GUI_RULE_ACTION_ENABLE,
        condition: int = RAW_GUI_RULE_CONDITION_BOOL_TRUE,
        target_index: int = RAW_GUI_RULE_INDEX_ALL,
        condition_index: int = RAW_GUI_RULE_INDEX_ALL,
        condition_int_value: int = 0,
    ) -> int:
        rule = RawGuiRule(
            int(action),
            int(condition),
            _as_bytes(target_key),
            _as_bytes(condition_key),
            int(target_index),
            int(condition_index),
            int(condition_int_value),
        )
        return self.host.orchestrator_add_gui_rule(self.orchestrator, ctypes.byref(rule))

    def create_option_def(self, **kwargs) -> int:
        defn = RawConfigOptionDef()
        defn.height = -1
        defn.width = -1
        defn.label_width = -1
        defn.sidetext_width = -1
        defn.invalidates_step = STEP_ANY

        byte_refs = []
        for key, value in kwargs.items():
            if isinstance(value, str):
                encoded = _as_bytes(value)
                byte_refs.append(encoded)
                setattr(defn, key, encoded)
            else:
                setattr(defn, key, value)

        result = self.host.orchestrator_create_option_def(self.orchestrator, ctypes.byref(defn))
        if result != OPTION_DEF_ERROR_OK:
            option_key = kwargs.get("opt_key", "<unknown>")
            raise RuntimeError(f"Cannot register config option '{option_key}': error {result}")
        return result

    def register_generic_facets_annotation(
        self,
        *,
        key: str,
        label: str,
        enforce_label: str,
        block_label: str,
        icon_svg: str,
    ) -> int:
        defn = RawGenericFacetsAnnotationDef(
            _as_bytes(key),
            _as_bytes(label),
            _as_bytes(enforce_label),
            _as_bytes(block_label),
            _as_bytes(icon_svg),
        )
        return int(self.host.orchestrator_register_generic_facets_annotation(self.orchestrator, ctypes.byref(defn)))

    def storage_new_polygon(self, storage_address: int) -> int:
        return int(self.host.storage_new_polygon(ctypes.c_void_p(storage_address)) or 0)

    def multipoint(self, handle: int) -> MultiPoint:
        return MultiPoint(self, handle)

    def polygon(self, handle: int) -> Polygon:
        return Polygon(self, handle)

    def mutable_polygon(self, handle: int) -> MutablePolygon:
        return MutablePolygon(self, handle)

    def polyline(self, handle: int) -> Polyline:
        return Polyline(self, handle)

    def mutable_polyline(self, handle: int) -> MutablePolyline:
        return MutablePolyline(self, handle)

    def polygon_collection(self, handle: int) -> PolygonCollection:
        return PolygonCollection(self, handle)

    def polyline_collection(self, handle: int) -> PolylineCollection:
        return PolylineCollection(self, handle)

    def expolygon(self, handle: int) -> ExPolygon:
        return ExPolygon(self, handle)

    def expolygon_collection(self, handle: int) -> ExPolygonCollection:
        return ExPolygonCollection(self, handle)

    def new_polygon(self, storage_address: int) -> StoredPolygon:
        return StoredPolygon(self, storage_address)

    def new_polyline(self, storage_address: int) -> StoredPolyline:
        return StoredPolyline(self, storage_address)

    def new_polygon_collection(self, storage_address: int) -> StoredPolygonCollection:
        return StoredPolygonCollection(self, storage_address)

    def new_polyline_collection(self, storage_address: int) -> StoredPolylineCollection:
        return StoredPolylineCollection(self, storage_address)

    def new_expolygon(self, storage_address: int) -> StoredExPolygon:
        return StoredExPolygon(self, storage_address)

    def new_expolygon_collection(self, storage_address: int) -> StoredExPolygonCollection:
        return StoredExPolygonCollection(self, storage_address)

    def extrusion(self, handle: int) -> ExtrusionEntity:
        return ExtrusionEntity(self, handle)

    def mutable_extrusion(self, handle: int) -> MutableExtrusionEntity:
        return MutableExtrusionEntity(self, handle)

    def new_extrusion(self, storage_address: int, src=None) -> StoredExtrusionEntity:
        return StoredExtrusionEntity(self, storage_address, src)

    def medial_axis_extrusion(self, role: int, flow) -> MedialAxisExtrusionFactory:
        return MedialAxisExtrusionFactory(self, role, flow)

    def medial_axis_thin_wall(self, flow) -> MedialAxisExtrusionFactory:
        return medial_axis_thin_wall(self, flow)

    def medial_axis_gap_fill(self, flow) -> MedialAxisExtrusionFactory:
        return medial_axis_gap_fill(self, flow)

    def register_extrusion_property_type(self, namespaced_name: str, payload_cls) -> int:
        return register_extrusion_property_type(self, namespaced_name, payload_cls)

    def clipper(self, storage_address: int) -> ClipperContext:
        return ClipperContext(self, storage_address)

    def clipper_operand(self, storage_address: int, source=None) -> ClipperOperand:
        return ClipperOperand(self, storage_address, source)

    def config(self, handle: int) -> Config:
        return Config(self, handle)

    def mutable_config(self, handle: int) -> MutableConfig:
        return MutableConfig(self, handle)

    def config_option(self, handle: int) -> ConfigOption:
        return ConfigOption(self, handle)

    def mutable_config_option(self, handle: int) -> MutableConfigOption:
        return MutableConfigOption(self, handle)

    def surface(self, handle: int) -> Surface:
        return Surface(self, handle)

    def surface_collection(self, handle: int) -> SurfaceCollection:
        return SurfaceCollection(self, handle)

    def print(self, handle: int) -> Print:
        return Print(self, handle)

    def mutable_print(self, handle: int) -> MutablePrint:
        return MutablePrint(self, handle)

    def object(self, handle: int) -> Object:
        return Object(self, handle)

    def mutable_object(self, handle: int) -> MutableObject:
        return MutableObject(self, handle)

    def layer(self, handle: int) -> Layer:
        return Layer(self, handle)

    def mutable_layer(self, handle: int) -> MutableLayer:
        return MutableLayer(self, handle)

    def layer_region(self, handle: int) -> LayerRegion:
        return LayerRegion(self, handle)

    def mutable_layer_region(self, handle: int) -> MutableLayerRegion:
        return MutableLayerRegion(self, handle)

    def layer_island(self, handle: int) -> LayerIsland:
        return LayerIsland(self, handle)

    def mutable_layer_island(self, handle: int) -> MutableLayerIsland:
        return MutableLayerIsland(self, handle)

    def layer_region_island(self, handle: int) -> LayerRegionIsland:
        return LayerRegionIsland(self, handle)

    def mutable_layer_region_island(self, handle: int) -> MutableLayerRegionIsland:
        return MutableLayerRegionIsland(self, handle)

    def print_region(self, handle: int) -> PrintRegion:
        return PrintRegion(self, handle)

    def mutable_print_region(self, handle: int) -> MutablePrintRegion:
        return MutablePrintRegion(self, handle)

    def volume(self, handle: int) -> Volume:
        return Volume(self, handle)

    def triangle_mesh(self, handle: int) -> TriangleMesh:
        return TriangleMesh(self, handle)

    def layer_height(self, run_ctx_address: int) -> LayerHeightContext | None:
        return LayerHeightContext.from_run_context(self, run_ctx_address)

    def slicing(self, run_ctx_address: int) -> SlicingContext | None:
        return SlicingContext.from_run_context(self, run_ctx_address)

    def post_slicing(self, run_ctx_address: int) -> PostSlicingContext | None:
        return PostSlicingContext.from_run_context(self, run_ctx_address)

    def post_perimeter(self, run_ctx_address: int) -> PostPerimeterContext | None:
        return PostPerimeterContext.from_run_context(self, run_ctx_address)

    def post_infill(self, run_ctx_address: int) -> PostInfillContext | None:
        return PostInfillContext.from_run_context(self, run_ctx_address)

    def perimeter(self, run_ctx_address: int) -> PerimeterContext | None:
        return PerimeterContext.from_run_context(self, run_ctx_address)

    def perimeter_module(self, run_ctx_address: int) -> PerimeterModuleContext | None:
        return PerimeterModuleContext.from_run_context(self, run_ctx_address)

    def surface_generation(self, run_ctx_address: int) -> SurfaceGenerationContext | None:
        return SurfaceGenerationContext.from_run_context(self, run_ctx_address)

    def storage_clear(self, storage_address: int) -> None:
        self.host.storage_clear(ctypes.c_void_p(storage_address))

    def storage_size(self, storage_address: int) -> int:
        return int(self.host.storage_size(ctypes.c_void_p(storage_address)))

    def config_get(self, config_address: int, key: str) -> int:
        return int(self.host.config_get(ctypes.c_void_p(config_address), _as_bytes(key)) or 0)

    def config_bool(self, config_address: int, key: str, idx: int = 0) -> bool:
        opt = self.config_get(config_address, key)
        return bool(opt and self.host.config_option_get_bool(ctypes.c_void_p(opt), int(idx)))

    def config_int(self, config_address: int, key: str, idx: int = 0) -> int:
        opt = self.config_get(config_address, key)
        return 0 if not opt else int(self.host.config_option_get_int(ctypes.c_void_p(opt), int(idx)))

    def config_float(self, config_address: int, key: str, idx: int = 0) -> float:
        opt = self.config_get(config_address, key)
        return 0.0 if not opt else float(self.host.config_option_get_float(ctypes.c_void_p(opt), int(idx)))

    def config_float_or_percent_effective(self, config_address: int, key: str, ratio: float, idx: int = 0) -> float:
        opt = self.config_get(config_address, key)
        if not opt:
            return 0.0
        value = self.host.config_option_get_float_or_percent(ctypes.c_void_p(opt), int(idx))
        return float(self.host.c_float_or_percent_get_effective_value(ctypes.byref(value), float(ratio)))

    def polygon_points(self, polygon_address: int) -> list[CPoint]:
        multipoint = self.host.polygon_as_multipoint_const(ctypes.c_void_p(polygon_address))
        count = int(self.host.multipoint_size(multipoint))
        return [self.host.multipoint_get(multipoint, idx) for idx in range(count)]

    def polygon_point_count(self, polygon_address: int) -> int:
        multipoint = self.host.polygon_as_multipoint_const(ctypes.c_void_p(polygon_address))
        return int(self.host.multipoint_size(multipoint))

    def polygon_convex_point_count(self, polygon_address: int, min_angle: float, max_angle: float) -> int:
        count = self.polygon_point_count(polygon_address)
        array = (ctypes.c_uint32 * count)()
        return int(self.host.polygon_convex_points_idx(
            ctypes.c_void_p(polygon_address),
            float(min_angle),
            float(max_angle),
            array,
            count,
        ))

    def polygon_centroid(self, polygon_address: int) -> CPoint:
        return self.host.polygon_centroid(ctypes.c_void_p(polygon_address))

    def polygon_replace_points(self, polygon_address: int, replacement_polygon_address: int) -> None:
        dst = self.host.polygon_as_multipoint(ctypes.c_void_p(polygon_address))
        src = self.host.polygon_as_multipoint_const(ctypes.c_void_p(replacement_polygon_address))
        self.host.multipoint_copy(dst, src)

    def polygon_push_back(self, polygon_address: int, point: CPoint) -> None:
        multipoint = self.host.polygon_as_multipoint(ctypes.c_void_p(polygon_address))
        self.host.multipoint_push_back(multipoint, point)

    def polygon_make_clockwise(self, polygon_address: int) -> None:
        self.host.polygon_make_clockwise(ctypes.c_void_p(polygon_address))

    def expolygon_holes(self, expolygon_address: int, mutable: bool = False) -> list[int]:
        count = int(self.host.expolygon_hole_size(ctypes.c_void_p(expolygon_address)))
        if mutable:
            return [
                int(self.host.expolygon_hole_at(ctypes.c_void_p(expolygon_address), idx) or 0)
                for idx in range(count)
            ]
        return [
            int(self.host.expolygon_hole_at_const(ctypes.c_void_p(expolygon_address), idx) or 0)
            for idx in range(count)
        ]

    def expolygons(self, collection_address: int, mutable: bool = False) -> list[int]:
        count = int(self.host.expolygons_size(ctypes.c_void_p(collection_address)))
        if mutable:
            return [
                int(self.host.expolygons_at(ctypes.c_void_p(collection_address), idx) or 0)
                for idx in range(count)
            ]
        return [
            int(self.host.expolygons_at_const(ctypes.c_void_p(collection_address), idx) or 0)
            for idx in range(count)
        ]
