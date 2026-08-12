#/|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
#/|/
#/|/ SuperSlicer is released under the terms of the AGPLv3 or higher
#/|/

"""
Polyholes plugin written against the high-level Python helpers.

This file intentionally avoids raw api.host calls and raw geometry handles. It
is meant as a readable example of how a Python plugin can walk the data tree,
inspect configs, edit mutable raw slices and keep host caches consistent through
the view layer. It shares the normal Polyholes settings and exclusive group so
the host selector can choose between the C++, low-level Python and high-level
Python implementations.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

from slic3r_api import (
    CPoint,
    PluginBase,
    RAW_CO_BOOL,
    RAW_CO_FLOAT_OR_PERCENT,
    RAW_CONFIG_OPTION_MODE_ADV_EXP,
    RAW_CONFIG_OPTION_MODE_EXPERT,
    RAW_CONFIG_OPTION_MODE_SUSI,
    RAW_CONTAINER_TYPE_REGION,
    RAW_GUI_RULE_ACTION_ENABLE,
    RAW_GUI_RULE_CONDITION_BOOL_TRUE,
    RAW_OPTION_CATEGORY_SLICING,
    RAW_PRESET_TYPE_FFF_PRINT,
    RAW_PT_FFF,
    SCALED_EPSILON,
    STEP_POST_SLICING,
    STEP_SLICING,
    StoredPolygon,
    make_point,
    round_coord,
    scale_i,
    unscaled,
    used_config_key,
)


POLYHOLES_KEY = "hole_to_polyhole"
POLYHOLES_THRESHOLD_KEY = "hole_to_polyhole_threshold"
POLYHOLES_TWISTED_KEY = "hole_to_polyhole_twisted"
POLYHOLES_EXCLUSIVE_GROUP = "polyholes"
POLYHOLES_SELECTOR_KEY = "exclusive_group_300_polyholes_plugin"
POLYHOLES_SETTINGS_FRAGMENT_ID = "polyholes_settings"


@dataclass
class HoleData:
    center_scaled: CPoint
    max_radius_scaled: float
    extruder_id: int
    max_deviation_scaled: int
    twist: bool
    points_scaled: list[CPoint]
    layer_region_idx: int


@dataclass
class LayerHole:
    points_scaled: list[CPoint]
    layer_idx: int
    layer_region_idx: int


@dataclass
class ThroughHole:
    hole_data: HoleData
    layers: list[LayerHole]


def _point_distance_scaled(lhs: CPoint, rhs: CPoint) -> float:
    return math.hypot(float(lhs.x - rhs.x), float(lhs.y - rhs.y))


def _point_mid_scaled(lhs: CPoint, rhs: CPoint) -> CPoint:
    return make_point((lhs.x + rhs.x) // 2, (lhs.y + rhs.y) // 2)


def _points_equal(lhs: list[CPoint], rhs: list[CPoint]) -> bool:
    if len(lhs) != len(rhs):
        return False
    return all(a.x == b.x and a.y == b.y for a, b in zip(lhs, rhs))


def _create_polyholes(api, storage: int, center_scaled: CPoint, radius_scaled: float, nozzle_diameter_scaled: int, twist: bool) -> list[StoredPolygon]:
    if nozzle_diameter_scaled <= 0:
        nozzle_diameter_scaled = 1
    radius_mm = unscaled(radius_scaled)
    nozzle_diameter_mm = unscaled(nozzle_diameter_scaled)
    edge_count = max(3, round_coord(4.0 * radius_mm * 0.4 / nozzle_diameter_mm))
    polyhole_count = 5 if twist else 1
    rotation_rad = 2.0 * math.pi / (edge_count * polyhole_count) if twist else 0.0
    new_radius_scaled = radius_scaled / math.cos(math.pi / edge_count)

    polygons = [api.new_polygon(storage) for _ in range(polyhole_count)]
    for poly_idx in range(polyhole_count):
        polygon_idx = poly_idx // 2 if poly_idx % 2 == 0 else (polyhole_count + 1) // 2 + poly_idx // 2
        polygon = polygons[polygon_idx]
        for edge_idx in range(edge_count):
            angle_rad = rotation_rad * poly_idx + 2.0 * math.pi * edge_idx / edge_count
            x_scaled = round_coord(float(center_scaled.x) + new_radius_scaled * math.cos(angle_rad))
            y_scaled = round_coord(float(center_scaled.y) + new_radius_scaled * math.sin(angle_rad))
            polygon.push_back(make_point(x_scaled, y_scaled))
        polygon.make_clockwise()
    return polygons


def _replace_matching_hole(expolygon, points_to_replace_scaled: list[CPoint], replacement: StoredPolygon) -> bool:
    for hole in expolygon.holes_mutable():
        if _points_equal(hole.points(), points_to_replace_scaled):
            hole.copy_from(replacement)
            return True
    return False


class PythonPolyholesHighLevelPlugin(PluginBase):
    def __init__(self, api):
        super().__init__(
            "python.polyholes.high_level",
            STEP_POST_SLICING,
            name="Python polyholes high-level",
            description="High-level Python version of the polyholes post-slicing plugin.",
            priority=20,
            exclusive_group=POLYHOLES_EXCLUSIVE_GROUP,
            exclusive_group_label="Polyholes plugin",
            exclusive_group_tooltip="Choose which active plugin converts round vertical holes to polyholes.",
            used_config_keys=[
                used_config_key(POLYHOLES_KEY, RAW_CO_BOOL, RAW_CONTAINER_TYPE_REGION, RAW_PRESET_TYPE_FFF_PRINT),
                used_config_key(POLYHOLES_THRESHOLD_KEY, RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_REGION, RAW_PRESET_TYPE_FFF_PRINT),
                used_config_key(POLYHOLES_TWISTED_KEY, RAW_CO_BOOL, RAW_CONTAINER_TYPE_REGION, RAW_PRESET_TYPE_FFF_PRINT),
            ],
            defined_config_keys=[
                POLYHOLES_KEY,
                POLYHOLES_THRESHOLD_KEY,
                POLYHOLES_TWISTED_KEY,
            ],
        )
        self.api = api

    def initialize(self, storage_address: int) -> None:
        self.api.create_option_def(
            opt_key=POLYHOLES_KEY,
            type=RAW_CO_BOOL,
            container_type=RAW_CONTAINER_TYPE_REGION,
            option_preset_type=RAW_PRESET_TYPE_FFF_PRINT,
            printer_technology=RAW_PT_FFF,
            label="Convert round holes to polyholes",
            full_label="Convert round holes to polyholes",
            category=RAW_OPTION_CATEGORY_SLICING,
            invalidates_step=STEP_SLICING,
            translation_domain="Slic3r",
            tooltip=(
                "Search for almost-circular holes that span more than one layer and convert the geometry to polyholes."
                " Use the nozzle size and the (biggest) diameter to compute the polyhole."
                "\nSee http://hydraraptor.blogspot.com/2011/02/polyholes.html"
            ),
            mode=RAW_CONFIG_OPTION_MODE_ADV_EXP | RAW_CONFIG_OPTION_MODE_SUSI,
            default_serialized_value="0",
        )
        self.api.create_option_def(
            opt_key=POLYHOLES_THRESHOLD_KEY,
            type=RAW_CO_FLOAT_OR_PERCENT,
            container_type=RAW_CONTAINER_TYPE_REGION,
            option_preset_type=RAW_PRESET_TYPE_FFF_PRINT,
            printer_technology=RAW_PT_FFF,
            label="Roundness margin",
            full_label="Polyhole detection margin",
            category=RAW_OPTION_CATEGORY_SLICING,
            invalidates_step=STEP_SLICING,
            translation_domain="Slic3r",
            tooltip=(
                "Maximum deflection of a point to the estimated radius of the circle."
                "\nAs cylinders are often exported as triangles of varying size, points may not be on the circle circumference."
                " This setting allows you some leeway to broaden the detection."
                "\nIn mm or in % of the radius."
            ),
            sidetext="mm or %",
            has_max_literal=1,
            max_literal_value=10.0,
            max_literal_is_percent=0,
            mode=RAW_CONFIG_OPTION_MODE_EXPERT | RAW_CONFIG_OPTION_MODE_SUSI,
            default_serialized_value="0.01",
        )
        self.api.create_option_def(
            opt_key=POLYHOLES_TWISTED_KEY,
            type=RAW_CO_BOOL,
            container_type=RAW_CONTAINER_TYPE_REGION,
            option_preset_type=RAW_PRESET_TYPE_FFF_PRINT,
            printer_technology=RAW_PT_FFF,
            label="Twisting",
            full_label="Polyhole twist",
            category=RAW_OPTION_CATEGORY_SLICING,
            invalidates_step=STEP_SLICING,
            translation_domain="Slic3r",
            tooltip="Rotate the polyhole every layer.",
            mode=RAW_CONFIG_OPTION_MODE_EXPERT | RAW_CONFIG_OPTION_MODE_SUSI,
            default_serialized_value="1",
        )

        self.api.add_ui_fragment(
            "print.ui",
            POLYHOLES_EXCLUSIVE_GROUP,
            "page:Slicing\n"
            "group:Modifying slices\n"
            "line:insert$beforeline$Convert round vertical holes to polyholes:Polyholes plugin\n"
            f"setting:{POLYHOLES_SELECTOR_KEY}\n"
            "end_line\n",
            priority=1,
        )
        self.api.add_ui_fragment(
            "print.ui",
            POLYHOLES_SETTINGS_FRAGMENT_ID,
            "page:Slicing\n"
            "group:Modifying slices\n"
            "line:insert$afterline$Vertical Hole shrinking compensation:Convert round vertical holes to polyholes\n"
            f"setting:label$_:{POLYHOLES_KEY}\n"
            f"setting:sidetext_width$5:{POLYHOLES_THRESHOLD_KEY}\n"
            f"setting:{POLYHOLES_TWISTED_KEY}\n"
            "end_line\n",
            priority=0,
        )
        self.api.add_gui_rule(
            target_key=POLYHOLES_THRESHOLD_KEY,
            condition_key=POLYHOLES_KEY,
            action=RAW_GUI_RULE_ACTION_ENABLE,
            condition=RAW_GUI_RULE_CONDITION_BOOL_TRUE,
        )
        self.api.add_gui_rule(
            target_key=POLYHOLES_TWISTED_KEY,
            condition_key=POLYHOLES_KEY,
            action=RAW_GUI_RULE_ACTION_ENABLE,
            condition=RAW_GUI_RULE_CONDITION_BOOL_TRUE,
        )

    def run(self, run_ctx_address: int) -> None:
        ctx = self.api.post_slicing(run_ctx_address)
        if ctx is None:
            return

        try:
            self._run_polyholes(ctx)
        except Exception as exc:
            ctx.report_error(f"Python Polyholes high level failed: {exc}")

    def _run_polyholes(self, ctx) -> None:
        obj = ctx.object()
        layer_count = obj.layer_count()
        layer_holes: list[list[HoleData]] = [[] for _ in range(layer_count)]

        for layer_idx, layer in enumerate(obj.layers()):
            if ctx.is_cancelled():
                return
            for region_idx, layer_region in enumerate(layer.regions()):
                region_config = layer_region.print_region().config()
                if not region_config.bool(POLYHOLES_KEY):
                    continue

                twist = region_config.bool(POLYHOLES_TWISTED_KEY)
                perimeter_extruder = region_config.int("perimeter_extruder") - 1
                threshold = region_config.option(POLYHOLES_THRESHOLD_KEY)

                for expolygon in layer_region.slices():
                    for hole in expolygon.holes():
                        points_scaled = hole.points()
                        if len(points_scaled) <= 8:
                            continue
                        if hole.convex_points_idx(0.0, math.pi):
                            continue

                        center_scaled = hole.centroid()
                        min_radius_scaled = float("inf")
                        max_radius_scaled = 0.0
                        radius_sum_scaled = 0.0
                        for point in points_scaled:
                            distance_scaled = _point_distance_scaled(point, center_scaled)
                            min_radius_scaled = min(min_radius_scaled, distance_scaled)
                            max_radius_scaled = max(max_radius_scaled, distance_scaled)
                            radius_sum_scaled += distance_scaled

                        min_line_radius_scaled = float("inf")
                        max_line_radius_scaled = 0.0
                        previous = points_scaled[-1]
                        for point in points_scaled:
                            midline_scaled = _point_mid_scaled(previous, point)
                            distance_scaled = _point_distance_scaled(center_scaled, midline_scaled)
                            min_line_radius_scaled = min(min_line_radius_scaled, distance_scaled)
                            max_line_radius_scaled = max(max_line_radius_scaled, distance_scaled)
                            previous = point

                        reference_radius_mm = unscaled(radius_sum_scaled / len(points_scaled))
                        max_variation_scaled = scale_i(threshold.get_effective_value(reference_radius_mm))
                        max_variation_scaled = max(SCALED_EPSILON, max_variation_scaled)
                        if max_radius_scaled - min_radius_scaled < max_variation_scaled * 2 and max_line_radius_scaled - min_line_radius_scaled < max_variation_scaled * 2:
                            layer_holes[layer_idx].append(HoleData(
                                center_scaled=center_scaled,
                                max_radius_scaled=max_radius_scaled,
                                extruder_id=perimeter_extruder,
                                max_deviation_scaled=max_variation_scaled,
                                twist=twist,
                                points_scaled=points_scaled,
                                layer_region_idx=region_idx,
                            ))
            ctx.report_progress((layer_idx + 1) / max(1, layer_count), "Python Polyholes high level: searching holes")

        through_holes = self._group_holes(obj, layer_holes)
        print_config = ctx.print().config()
        modified_layers: set[int] = set()

        for hole_idx, through_hole in enumerate(through_holes):
            nozzle_diameter_scaled = scale_i(print_config.float(
                "nozzle_diameter",
                max(0, through_hole.hole_data.extruder_id),
            ))
            replacements = _create_polyholes(
                self.api,
                ctx.plugin_storage(),
                through_hole.hole_data.center_scaled,
                through_hole.hole_data.max_radius_scaled,
                nozzle_diameter_scaled,
                through_hole.hole_data.twist,
            )
            for layer_hole in through_hole.layers:
                mutable_layer = obj.layer_mutable(layer_hole.layer_idx)
                mutable_region = mutable_layer.region_mutable(layer_hole.layer_region_idx)
                mutable_slices = ctx.borrow_layer_region_slices(mutable_region)
                replacement = replacements[layer_hole.layer_idx % len(replacements)]
                modified = 0
                for expolygon in mutable_slices.mutable_items():
                    if _replace_matching_hole(expolygon, layer_hole.points_scaled, replacement):
                        modified += 1
                if modified:
                    modified_layers.add(layer_hole.layer_idx)
            for polygon in replacements:
                polygon.free_from_storage()
            ctx.report_progress(
                (hole_idx + 1) / max(1, len(through_holes)),
                "Python Polyholes high level: converting holes",
            )

        for layer_idx in sorted(modified_layers):
            ctx.recompute_slices_and_islands_from_layer_region(obj.layer_mutable(layer_idx))

        if ctx.storage_size() != 0:
            ctx.clear_storage()

    def _group_holes(self, obj, layer_holes: list[list[HoleData]]) -> list[ThroughHole]:
        through_holes: list[ThroughHole] = []
        min_layer_count = 2
        layer_count = len(layer_holes)

        for layer_idx in range(layer_count):
            for main_hole in list(layer_holes[layer_idx]):
                max_z_scaled = obj.layer(layer_idx).print_z()
                holes = [LayerHole(main_hole.points_scaled, layer_idx, main_hole.layer_region_idx)]
                for search_layer_idx in range(layer_idx + 1, layer_count):
                    search_layer = obj.layer(search_layer_idx)
                    if search_layer.print_z() - search_layer.height() - max_z_scaled > 0:
                        break

                    candidates = layer_holes[search_layer_idx]
                    for search_hole_idx, search_hole in enumerate(candidates):
                        if (
                            main_hole.extruder_id == search_hole.extruder_id
                            and _point_distance_scaled(main_hole.center_scaled, search_hole.center_scaled) < main_hole.max_deviation_scaled
                            and abs(main_hole.max_radius_scaled - search_hole.max_radius_scaled) < main_hole.max_deviation_scaled
                        ):
                            max_z_scaled = search_layer.print_z()
                            holes.append(LayerHole(search_hole.points_scaled, search_layer_idx, search_hole.layer_region_idx))
                            del candidates[search_hole_idx]
                            break

                if len(holes) >= min_layer_count or (len(holes) == 1 and holes[0].layer_idx == 0):
                    through_holes.append(ThroughHole(main_hole, holes))

        return through_holes


def register_plugin(api):
    return PythonPolyholesHighLevelPlugin(api)
