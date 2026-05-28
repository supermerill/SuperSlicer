#/|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
#/|/
#/|/ SuperSlicer is released under the terms of the AGPLv3 or higher
#/|/

"""
HairyObject: a playful STEP_POST_PERIMETER Python plugin.

The plugin registers a generic facet painting tool named "Hairy". Painted
facets are projected to layers, intersected with each island contour, and then
turned into tiny one-segment thin-wall extrusions that point away from the
island. The hairs are appended after the normal perimeter bucket so perimeter
printing stays unchanged and the decorative strokes are emitted last.
"""

from __future__ import annotations

import ctypes
import math
from dataclasses import dataclass

from slic3r_api import (
    EPropertyAttributes,
    PluginBase,
    RAW_CO_FLOAT,
    RAW_CO_INT,
    RAW_CO_VECTOR_FLOAT,
    RAW_CONFIG_OPTION_MODE_EXPERT,
    RAW_CONFIG_OPTION_MODE_SUSI,
    RAW_CONTAINER_TYPE_REGION,
    RAW_EXTRUSION_FLAG_SORTABLE,
    RAW_EXTRUSION_ROLE_EXTERNAL_PERIMETER,
    RAW_EXTRUSION_ROLE_PERIMETER,
    RAW_EXTRUSION_ROLE_THIN_WALL,
    RAW_FACET_PAINTING_ENFORCER,
    RAW_OPTION_CATEGORY_FUZZY_SKIN,
    RAW_PRESET_TYPE_FFF_PRINT,
    RAW_PT_FFF,
    STEP_POST_PERIMETER,
    StoredExtrusionEntity,
    StoredPolylineCollection,
    make_point,
    norm,
    scale_i,
    unscaled,
    used_config_key,
)


PLUGIN_ID = "python.perimeter.post_process.hairy_object"
PAINT_KEY = "perimeter.post_process.hairy_object.painting"
HAIR_LENGTH_KEY = "hairy_object_hair_length"
HAIR_DENSITY_KEY = "hairy_object_hair_density"
PAINT_MASK_MARGIN_SCALED = scale_i(0.05)

HAIRY_ICON_SVG = (
    "<svg xmlns='http://www.w3.org/2000/svg' width='128' height='128' viewBox='0 0 128 128'>"
    "<rect x='34' y='18' width='12' height='92' fill='none' stroke='#FFFFFF' stroke-width='3' "
    "rx='1.5' ry='1.5'/>"
    "<path d='M46 36c23-2 43-2 66-1M46 64c22-2 43-2 66-4M46 92c23 0 45 0 67-2' "
    "fill='none' stroke='#2172eb' stroke-width='3' stroke-linecap='round' stroke-linejoin='round'/>"
    "</svg>"
)


@dataclass
class HairSettings:
    length_scaled: int
    density_per_mm2: float
    width_mm: float
    height_mm: float
    mm3_per_mm: float


def _points_equal(lhs, rhs) -> bool:
    return int(lhs.x) == int(rhs.x) and int(lhs.y) == int(rhs.y)


def _point_lerp(a, b, ratio: float):
    return make_point(
        int(round(float(a.x) + float(b.x - a.x) * ratio)),
        int(round(float(a.y) + float(b.y - a.y) * ratio)),
    )


def _normal_endpoint(island_area, start, dx: float, dy: float, length_scaled: int, ccw: bool):
    seg_len = math.hypot(dx, dy)
    if seg_len <= 0.0:
        return None

    # For a CCW contour, the island interior is on the left of each edge, so
    # the right-hand normal points outward. If a clipped polyline came back in
    # the opposite direction, the containment check below flips it back out.
    if ccw:
        nx = dy / seg_len
        ny = -dx / seg_len
    else:
        nx = -dy / seg_len
        ny = dx / seg_len

    end = make_point(
        int(round(float(start.x) + nx * float(length_scaled))),
        int(round(float(start.y) + ny * float(length_scaled))),
    )
    if island_area.contains(end):
        end = make_point(
            int(round(float(start.x) - nx * float(length_scaled))),
            int(round(float(start.y) - ny * float(length_scaled))),
        )
    return end


@dataclass
class LayerSampling:
    spacing_scaled: float
    start_offset_scaled: float


def _sampling_for_layer(layer_idx: int, layer_height_scaled: int, settings: HairSettings) -> LayerSampling | None:
    if layer_height_scaled <= 0 or settings.density_per_mm2 <= 0.0:
        return None

    layer_height_mm = max(unscaled(layer_height_scaled), 0.001)
    ideal_spacing_mm = math.sqrt(1.0 / settings.density_per_mm2)

    # Hair density is expressed per painted wall area. If the requested density
    # is low, placing hairs on every layer would force a very large horizontal
    # spacing. Instead, skip whole layers until the vertical distance roughly
    # matches the square-cell spacing, then compute the horizontal spacing that
    # preserves the requested hair/mm2 average.
    layer_stride = max(1, int(math.ceil(ideal_spacing_mm / layer_height_mm)))
    if layer_idx % layer_stride != 0:
        return None

    vertical_spacing_mm = layer_stride * layer_height_mm
    spacing_mm = 1.0 / (settings.density_per_mm2 * vertical_spacing_mm)
    spacing_scaled = max(1.0, float(scale_i(spacing_mm)))

    # A deterministic low-discrepancy offset avoids stacking every layer's hair
    # roots on the same X/Y columns while still keeping slicing reproducible.
    jitter = (0.5 + layer_idx * 0.6180339887498949) % 1.0
    return LayerSampling(spacing_scaled=spacing_scaled, start_offset_scaled=spacing_scaled * jitter)


def _sample_hairs_on_polyline(island_area, polyline, settings: HairSettings, sampling: LayerSampling, ccw: bool):
    points = polyline.points()
    if len(points) < 2:
        return []

    hairs = []
    next_distance = sampling.start_offset_scaled
    for idx in range(1, len(points)):
        a = points[idx - 1]
        b = points[idx]
        dx = float(b.x - a.x)
        dy = float(b.y - a.y)
        segment_length = math.hypot(dx, dy)
        if segment_length <= 0.0:
            continue

        while next_distance <= segment_length:
            start = _point_lerp(a, b, next_distance / segment_length)
            end = _normal_endpoint(island_area, start, dx, dy, settings.length_scaled, ccw)
            if end is not None and not _points_equal(start, end):
                hairs.append((start, end))
            next_distance += sampling.spacing_scaled
        next_distance -= segment_length
    return hairs


def _nozzle_diameter_for_region(print_config, region_config) -> float:
    extruder_idx = max(0, region_config.int("perimeter_extruder", default=1) - 1)
    nozzle_option = print_config.get("nozzle_diameter")
    if nozzle_option is None or nozzle_option.size() == 0:
        return 0.4
    if extruder_idx >= nozzle_option.size():
        extruder_idx = 0
    nozzle_diameter = nozzle_option.get_float(extruder_idx)
    return nozzle_diameter if nozzle_diameter > 0.0 else 0.4


def _hair_settings_from_region(print_view, region) -> HairSettings | None:
    region_config = region.print_region().config()
    length_mm = region_config.float(HAIR_LENGTH_KEY, default=0.0)
    density_per_mm2 = region_config.float(HAIR_DENSITY_KEY, default=0.0)
    if length_mm <= 0.0 or density_per_mm2 <= 0.0:
        return None

    # Hairs are deliberately modeled as round extrusions: their visual diameter
    # is the nozzle diameter and the volume per millimeter is the circle area.
    nozzle_diameter = _nozzle_diameter_for_region(print_view.config(), region_config)
    radius = 0.5 * nozzle_diameter

    return HairSettings(
        length_scaled=scale_i(length_mm),
        density_per_mm2=density_per_mm2,
        width_mm=nozzle_diameter,
        height_mm=nozzle_diameter,
        mm3_per_mm=math.pi * radius * radius,
    )


def _paint_polygons_to_expolygons(api, storage: int, polygons):
    if polygons.empty():
        return None
    clip = api.clipper(storage)
    operand = clip(polygons)
    try:
        # Facet painting on a vertical wall projects to a very narrow polygon,
        # often right on the island boundary. Expand the mask a little before
        # converting it to ExPolygons so clipping the island contour is stable
        # and does not depend on exact coincident-edge behavior in Clipper.
        expanded = clip.offset(operand, PAINT_MASK_MARGIN_SCALED)
        try:
            merged = clip.union(expanded)
            try:
                return merged.to_expolygon_collection()
            finally:
                merged.free_from_storage()
        finally:
            expanded.free_from_storage()
    finally:
        operand.free_from_storage()


def _contour_piece_collection(api, storage: int, contour, paint_expolygons):
    points = contour.points()
    if len(points) < 2:
        return None

    # The contour polygon does not repeat the first point. Clipping as an open
    # polyline needs that explicit closing segment, otherwise the last edge of
    # the island would never receive hairs.
    line = api.new_polyline(storage)
    try:
        for point in points:
            line.push_back(point)
        if not _points_equal(points[0], points[-1]):
            line.push_back(points[0])
        handle = api.host.clipper_intersection_polyline_expolygons(
            ctypes.c_void_p(storage),
            line.mutable_c_handle(),
            paint_expolygons.c_handle(),
        )
        return StoredPolylineCollection.adopt_owned(api, storage, handle)
    finally:
        line.free_from_storage()


def _build_hair_group(api, storage: int, hairs, settings: HairSettings):
    group = StoredExtrusionEntity(api, storage)
    group.set_flags(RAW_EXTRUSION_FLAG_SORTABLE)

    attributes = group.property(EPropertyAttributes)
    attributes.role = RAW_EXTRUSION_ROLE_THIN_WALL
    attributes.mm3_per_mm = settings.mm3_per_mm
    attributes.width = float(settings.width_mm)
    attributes.height = float(settings.height_mm)
    attributes.no_seam = 1

    for start, end in hairs:
        hair = StoredExtrusionEntity(api, storage, [start, end])
        hair.set_flags(0)
        hair_attributes = hair.property(EPropertyAttributes)
        hair_attributes.role = RAW_EXTRUSION_ROLE_THIN_WALL
        hair_attributes.mm3_per_mm = settings.mm3_per_mm
        hair_attributes.width = float(settings.width_mm)
        hair_attributes.height = float(settings.height_mm)
        hair_attributes.no_seam = 1
        group.add_child_move(hair)
    return group


def _make_root_append_only(api, storage: int, root) -> None:
    if root.empty():
        root.disable_sort()
        root.set_continuous(False)
        return

    must_wrap_existing_content = root.has_polyline() or root.sortable() or root.continuous()
    if must_wrap_existing_content:
        existing = api.new_extrusion(storage, root.readonly())
        try:
            root.clear_content()
            root.disable_sort()
            root.set_continuous(False)
            root.add_child_move(existing)
        finally:
            existing.free_from_storage()
    else:
        root.disable_sort()
        root.set_continuous(False)


class HairyObjectPlugin(PluginBase):
    def __init__(self, api):
        super().__init__(
            PLUGIN_ID,
            STEP_POST_PERIMETER,
            name="HairyObject",
            description="Adds painted decorative hair strokes after normal perimeter extrusion.",
            priority=90,
            used_config_keys=[
                used_config_key(HAIR_LENGTH_KEY, RAW_CO_FLOAT, RAW_CONTAINER_TYPE_REGION, RAW_PRESET_TYPE_FFF_PRINT),
                used_config_key(HAIR_DENSITY_KEY, RAW_CO_FLOAT, RAW_CONTAINER_TYPE_REGION, RAW_PRESET_TYPE_FFF_PRINT),
                used_config_key("perimeter_extruder", RAW_CO_INT),
                used_config_key("nozzle_diameter", RAW_CO_VECTOR_FLOAT),
            ],
            defined_config_keys=[HAIR_LENGTH_KEY, HAIR_DENSITY_KEY],
        )
        self.api = api

    def initialize(self, storage_address: int) -> None:
        self.api.register_generic_facets_annotation(
            key=PAINT_KEY,
            label="Hairy",
            enforce_label="Add hairs",
            block_label="Erase hairs",
            icon_svg=HAIRY_ICON_SVG,
        )
        self.api.create_option_def(
            opt_key=HAIR_LENGTH_KEY,
            type=RAW_CO_FLOAT,
            container_type=RAW_CONTAINER_TYPE_REGION,
            option_preset_type=RAW_PRESET_TYPE_FFF_PRINT,
            printer_technology=RAW_PT_FFF,
            label="Hair length",
            full_label="Hair length",
            category=RAW_OPTION_CATEGORY_FUZZY_SKIN,
            invalidates_step=STEP_POST_PERIMETER,
            tooltip="Length of the thin-wall hairs generated on painted model facets.",
            sidetext="mm",
            has_min=1,
            min_value=0.0,
            mode=RAW_CONFIG_OPTION_MODE_EXPERT | RAW_CONFIG_OPTION_MODE_SUSI,
            default_serialized_value="1",
        )
        self.api.create_option_def(
            opt_key=HAIR_DENSITY_KEY,
            type=RAW_CO_FLOAT,
            container_type=RAW_CONTAINER_TYPE_REGION,
            option_preset_type=RAW_PRESET_TYPE_FFF_PRINT,
            printer_technology=RAW_PT_FFF,
            label="Hair density",
            full_label="Hair density",
            category=RAW_OPTION_CATEGORY_FUZZY_SKIN,
            invalidates_step=STEP_POST_PERIMETER,
            tooltip="Average number of hairs generated per square millimeter of painted vertical wall.",
            sidetext="hair/mm2",
            has_min=1,
            min_value=0.0,
            mode=RAW_CONFIG_OPTION_MODE_EXPERT | RAW_CONFIG_OPTION_MODE_SUSI,
            default_serialized_value="1",
        )
        self.api.add_ui_fragment(
            "print.ui",
            "hairy_object_settings",
            "page:Perimeters & Shell\n"
            "group:Advanced\n"
            "line:insert$afterline$Fuzzy skin (experimental):Hairy object\n"
            f"setting:width$6:sidetext_width$4:{HAIR_LENGTH_KEY}\n"
            f"setting:width$6:sidetext_width$8:{HAIR_DENSITY_KEY}\n"
            "end_line\n",
            priority=50,
        )

    def run(self, run_ctx_address: int) -> None:
        ctx = self.api.post_perimeter(run_ctx_address)
        if ctx is None:
            return

        try:
            self._run(ctx)
        except Exception as exc:
            ctx.report_error(f"HairyObject failed: {exc}")

    def _run(self, ctx) -> None:
        storage = ctx.plugin_storage()
        obj = ctx.object()
        painted_by_layer = ctx.project_painting_to_polygons(PAINT_KEY, RAW_FACET_PAINTING_ENFORCER)
        try:
            for layer_idx, layer in enumerate(obj.layers()):
                if ctx.is_cancelled():
                    return
                if layer_idx >= len(painted_by_layer) or painted_by_layer[layer_idx].empty():
                    continue

                paint_expolygons = _paint_polygons_to_expolygons(self.api, storage, painted_by_layer[layer_idx])
                if paint_expolygons is None:
                    continue
                try:
                    for island in layer.islands():
                        self._process_island(ctx, storage, layer_idx, layer, island, paint_expolygons)
                finally:
                    paint_expolygons.free_from_storage()
        finally:
            for polygons in painted_by_layer:
                polygons.free_from_storage()

    def _process_island(self, ctx, storage: int, layer_idx: int, layer, island, paint_expolygons) -> None:
        regions = list(island.regions())
        if not regions:
            return
        settings = _hair_settings_from_region(ctx.print(), regions[0])
        if settings is None or settings.length_scaled <= 0 or settings.density_per_mm2 <= 0.0:
            return

        sampling = _sampling_for_layer(layer_idx, layer.height(), settings)
        if sampling is None:
            return

        contour = island.slice().contour()
        clipped = _contour_piece_collection(self.api, storage, contour, paint_expolygons)
        if clipped is None:
            return
        try:
            if clipped.empty():
                return

            hairs = []
            ccw = contour.is_counter_clockwise()
            island_area = island.slice()
            for piece in clipped:
                hairs.extend(_sample_hairs_on_polyline(island_area, piece, settings, sampling, ccw))
            if not hairs:
                return

            root = None
            for region_island in island.region_islands():
                root = ctx.mutable_extrusion(region_island, RAW_EXTRUSION_ROLE_PERIMETER)
                if root is not None:
                    break
            if root is None:
                return

            group = _build_hair_group(self.api, storage, hairs, settings)
            try:
                _make_root_append_only(self.api, storage, root)
                root.add_child_move(group)
            finally:
                group.free_from_storage()
        finally:
            clipped.free_from_storage()


def register_plugin(api):
    return HairyObjectPlugin(api)
