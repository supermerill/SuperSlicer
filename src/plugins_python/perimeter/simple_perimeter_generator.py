#/|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
#/|/
#/|/ SuperSlicer is released under the terms of the AGPLv3 or higher
#/|/

"""
Simple STEP_PERIMETER generator written with the high-level Python helpers.

This mirrors the temporary native SimplePerimeterGenerator: for each perimeter
node it offsets the current area to create one closed perimeter loop, then
returns the inner area used by the host to create the next node.
"""

from __future__ import annotations

from dataclasses import dataclass

from slic3r_api import (
    CFlow,
    EPropertyAttributes,
    EPropertyPerimeter,
    PluginBase,
    RAW_EXTRUSION_FLAG_REVERSIBLE,
    RAW_EXTRUSION_ROLE_EXTERNAL_PERIMETER,
    STEP_PERIMETER,
    StoredExtrusionEntity,
    unscaled,
)


PLUGIN_ID = "python.perimeter.generator.simple"
LOOP_ROLE_DEFAULT = 1 << 0
LOOP_ROLE_HOLE = 1 << 3


@dataclass
class SimpleGeneratorState:
    flow: CFlow
    perimeter_count: int


def _offset_area(api, storage: int, area, delta: float):
    clip = api.clipper(storage)
    result = clip.offset(clip(area), delta).to_expolygon_collection()
    result.ensure_valid()
    return result


def _append_perimeter_loop(dst: StoredExtrusionEntity, polygon, flow: CFlow, perimeter_idx: int, loop_role: int) -> None:
    if not polygon.valid_polygon() or polygon.empty():
        return

    points = polygon.points()
    if not points:
        return
    points.append(points[0])

    path = StoredExtrusionEntity(dst.api, dst.storage(), points)
    attributes = path.property(EPropertyAttributes)
    attributes.role = RAW_EXTRUSION_ROLE_EXTERNAL_PERIMETER
    attributes.mm3_per_mm = flow.mm3_per_mm
    attributes.width = float(unscaled(flow.width))
    attributes.height = float(unscaled(flow.height))

    loop = StoredExtrusionEntity(dst.api, dst.storage())
    loop.set_flags(RAW_EXTRUSION_FLAG_REVERSIBLE)
    perimeter = loop.property(EPropertyPerimeter)
    perimeter.perimeter_idx = int(min(max(perimeter_idx, 0), 0xFFFF))
    perimeter.loop_role = int(loop_role)
    loop.add_child_move(path)
    dst.add_child_move(loop)


def _make_perimeter_extrusion(api, storage: int, area, flow: CFlow, perimeter_idx: int, line_offset: float) -> StoredExtrusionEntity:
    extrusion = StoredExtrusionEntity(api, storage)
    extrusion.disable_reverse().disable_sort()

    loops = _offset_area(api, storage, area, line_offset)
    try:
        for loop in loops:
            _append_perimeter_loop(extrusion, loop.contour(), flow, perimeter_idx, LOOP_ROLE_DEFAULT)
            for hole in loop.holes():
                _append_perimeter_loop(extrusion, hole, flow, perimeter_idx, LOOP_ROLE_HOLE)
    finally:
        loops.free_from_storage()
    return extrusion


class PythonSimplePerimeterGeneratorPlugin(PluginBase):
    def __init__(self, api):
        super().__init__(PLUGIN_ID, STEP_PERIMETER, priority=0)
        self.api = api

    def run(self, run_ctx_address: int) -> None:
        ctx = self.api.perimeter(run_ctx_address)
        if ctx is None:
            return

        try:
            self._run_perimeter(ctx)
        except Exception as exc:
            ctx.report_error(f"Python simple perimeter generator failed: {exc}")

    def _run_perimeter(self, ctx) -> None:
        island = ctx.island()
        regions = list(island.regions())
        if not regions:
            return

        region_config = regions[0].print_region().config()
        perimeter_count = max(0, region_config.int("perimeters"))
        state = SimpleGeneratorState(
            regions[0].flow(RAW_EXTRUSION_ROLE_EXTERNAL_PERIMETER),
            perimeter_count,
        )
        ctx.run_region_group(regions, island.slice(), state, self._generate_node)

    def _generate_node(self, state: SimpleGeneratorState, generation, node, inner_areas, inner_fill_areas) -> bool:
        storage = generation.plugin_storage()
        flow = state.flow

        if state.perimeter_count > 0:
            node.set_perimeter_needed(max(node.perimeter_needed(), state.perimeter_count))

        if node.perimeter_idx() >= node.perimeter_needed():
            inner = self.api.new_expolygon_collection(storage)
            try:
                inner.push_back(node.area())
                self.api.host.expolygons_move(inner_areas.mutable_c_handle(), inner.mutable_c_handle())
            finally:
                inner.free_from_storage()

            inner_fill = self.api.new_expolygon_collection(storage)
            try:
                inner_fill.push_back(node.fill_area())
                self.api.host.expolygons_move(inner_fill_areas.mutable_c_handle(), inner_fill.mutable_c_handle())
            finally:
                inner_fill.free_from_storage()
            return True

        first_perimeter = node.perimeter_idx() == 0
        perimeter_idx = node.perimeter_idx()
        line_offset = -0.5 * float(flow.width if first_perimeter else flow.spacing)
        extrusion = _make_perimeter_extrusion(self.api, storage, node.area(), flow, perimeter_idx, line_offset)
        try:
            node.extrusions().move_from(extrusion)
        finally:
            extrusion.free_from_storage()

        inner_offset = -0.5 * float(flow.width + flow.spacing) if first_perimeter else -float(flow.spacing)
        inner = _offset_area(self.api, storage, node.area(), inner_offset)
        try:
            self.api.host.expolygons_move(inner_areas.mutable_c_handle(), inner.mutable_c_handle())
        finally:
            inner.free_from_storage()

        # Keep the fill/anchor area slightly larger than the next perimeter
        # area, matching the native simple generator.
        fill_offset = inner_offset + 0.25 * float(flow.spacing)
        inner_fill = _offset_area(self.api, storage, node.area(), fill_offset)
        try:
            self.api.host.expolygons_move(inner_fill_areas.mutable_c_handle(), inner_fill.mutable_c_handle())
        finally:
            inner_fill.free_from_storage()

        return True


def register_plugin(api):
    return PythonSimplePerimeterGeneratorPlugin(api)
