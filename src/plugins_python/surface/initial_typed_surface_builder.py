#/|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
#/|/
#/|/ SuperSlicer is released under the terms of the AGPLv3 or higher
#/|/

"""
InitialTypedSurfaceBuilder STEP_SURFACE_GENERATION plugin implemented in Python.

This is intentionally close to the native InitialTypedSurfaceBuilder plugin.
It exists mostly as an integration example: when both this plugin and the native
one are active, the GUI exposes one exclusive-group selector for
STEP_SURFACE_GENERATION.
"""

from __future__ import annotations

from collections import defaultdict

from slic3r_api import (
    PluginBase,
    RAW_SURFACE_TYPE_DENS_SOLID,
    RAW_SURFACE_TYPE_DENS_SPARSE,
    RAW_SURFACE_TYPE_POS_BOTTOM,
    RAW_SURFACE_TYPE_POS_INTERNAL,
    RAW_SURFACE_TYPE_POS_TOP,
    STEP_SURFACE_GENERATION,
)
from slic3r_geometry_views import StoredExPolygonCollection


PLUGIN_ID = "python.surface.initial_typed_surface_builder"
SURFACE_GENERATION_GROUP = "step_surface_generation_plugin"
BOTTOM_SURFACE = RAW_SURFACE_TYPE_POS_BOTTOM | RAW_SURFACE_TYPE_DENS_SOLID
INTERNAL_SURFACE = RAW_SURFACE_TYPE_POS_INTERNAL | RAW_SURFACE_TYPE_DENS_SPARSE
TOP_SURFACE = RAW_SURFACE_TYPE_POS_TOP | RAW_SURFACE_TYPE_DENS_SOLID


def _region_infill_extruder_id(region) -> int:
    # SuperSlicer stores config extruders as 1-based values. LayerRegionIsland
    # stores the effective extruder as 0-based, matching the native data tree.
    return max(0, region.print_region().config().int("infill_extruder") - 1)


def _regions_by_infill_extruder(island) -> dict[int, list]:
    grouped = defaultdict(list)
    for region in island.regions():
        grouped[_region_infill_extruder_id(region)].append(region)
    return dict(grouped)


def _clip_infill_areas_to_regions(api, storage: int, island, regions: list):
    clip = api.clipper(storage)
    # When several infill extruders share one island, each LayerRegionIsland
    # receives only the part of the island infill areas covered by its regions.
    # Region raw slices are non-overlapping, so unioning them then intersecting
    # once is enough and keeps the plugin easy to read.
    with clip.empty() as region_paths:
        for region in regions:
            with clip(region.slices()) as region_operand:
                region_paths.concat_replace(region_operand)

        with clip.union(region_paths) as merged_regions:
            with clip(island.infill_areas()) as infill_operand:
                with clip.intersection(infill_operand, merged_regions) as clipped:
                    return clipped.to_expolygon_collection()


def _linked_island_slices(api, storage: int, linked_islands: list):
    slices = StoredExPolygonCollection(api, storage)
    for linked_island in linked_islands:
        slices.push_back(linked_island.slice())
    return slices


def _areas_without_linked_slices(api, storage: int, areas, linked_islands: list):
    # No linked island means this island is exposed on that side, so all input
    # areas belong to the requested surface class.
    if not linked_islands:
        return areas.clone(storage)

    slices = _linked_island_slices(api, storage, linked_islands)
    try:
        clip = api.clipper(storage)
        with clip(areas) as subject:
            with clip(slices) as clip_slices:
                with clip.diff(subject, clip_slices) as uncovered:
                    return uncovered.to_expolygon_collection()
    finally:
        slices.free_from_storage()


def _subtract_areas(api, storage: int, subject, clip_areas):
    if subject.empty():
        return StoredExPolygonCollection(api, storage)
    if clip_areas.empty():
        return subject.clone(storage)

    clip = api.clipper(storage)
    with clip(subject) as subject_operand:
        with clip(clip_areas) as clip_operand:
            with clip.diff(subject_operand, clip_operand) as result:
                return result.to_expolygon_collection()


def _occupied_union(api, storage: int, first, second):
    occupied = StoredExPolygonCollection(api, storage)
    occupied.append_copy_from(first)
    occupied.append_copy_from(second)
    if occupied.empty():
        return occupied

    try:
        clip = api.clipper(storage)
        with clip(occupied) as occupied_operand:
            with clip.union(occupied_operand) as result:
                return result.to_expolygon_collection()
    finally:
        occupied.free_from_storage()


def _classify_areas(api, storage: int, object_view, island, areas, is_first_layer: bool):
    # Bottom and top are detected independently. If the same area is both, the
    # native rule gives it to bottom, except on the first layer without raft.
    raw_bottom = _areas_without_linked_slices(api, storage, areas, list(island.lower_islands()))
    raw_top = _areas_without_linked_slices(api, storage, areas, list(island.upper_islands()))
    bottom = None
    top = None
    occupied = None
    internal = None
    try:
        first_layer_top_priority = is_first_layer and object_view.config().int("raft_layers", default=-1) == 0
        if first_layer_top_priority:
            bottom = _subtract_areas(api, storage, raw_bottom, raw_top)
            top = raw_top.clone(storage)
        else:
            bottom = raw_bottom.clone(storage)
            top = _subtract_areas(api, storage, raw_top, raw_bottom)

        occupied = _occupied_union(api, storage, bottom, top)
        internal = _subtract_areas(api, storage, areas, occupied)
        return [(bottom, BOTTOM_SURFACE), (internal, INTERNAL_SURFACE), (top, TOP_SURFACE)]
    finally:
        raw_bottom.free_from_storage()
        raw_top.free_from_storage()
        if occupied is not None:
            occupied.free_from_storage()


def _free_surface_groups(groups) -> None:
    for areas, _surface_type in groups:
        areas.free_from_storage()


class PythonInitialTypedSurfaceBuilderPlugin(PluginBase):
    def __init__(self, api):
        super().__init__(
            PLUGIN_ID,
            STEP_SURFACE_GENERATION,
            name="Python initial typed surface builder",
            description="Python example that creates initial bottom, internal, and top infill surfaces.",
            priority=10,
            exclusive_group=SURFACE_GENERATION_GROUP,
            exclusive_group_label="Surface generation plugin",
            exclusive_group_tooltip="Choose which active plugin converts perimeter fill areas into infill surfaces.",
        )
        self.api = api

    def run(self, run_ctx_address: int) -> None:
        ctx = self.api.surface_generation(run_ctx_address)
        if ctx is None:
            return

        try:
            self._run_surface_generation(ctx)
        except Exception as exc:
            ctx.report_error(f"Python initial typed surface builder failed: {exc}")

    def _run_surface_generation(self, ctx) -> None:
        storage = ctx.plugin_storage()
        object_view = ctx.object()
        for layer_idx, layer in enumerate(object_view.layers()):
            if ctx.is_cancelled():
                return
            for island in layer.islands():
                self._build_island_surfaces(ctx, storage, object_view, island, layer_idx == 0)

    def _build_island_surfaces(self, ctx, storage: int, object_view, island, is_first_layer: bool) -> None:
        grouped_regions = _regions_by_infill_extruder(island)
        if not grouped_regions:
            return

        single_group = len(grouped_regions) == 1
        for extruder_id, regions in grouped_regions.items():
            region_island = island.get_or_create_region_island(regions, extruder_id)
            if region_island is None:
                continue

            if single_group:
                groups = _classify_areas(self.api, storage, object_view, island, island.infill_areas(), is_first_layer)
                try:
                    ctx.set_fill_surface_groups(region_island, groups)
                finally:
                    _free_surface_groups(groups)
                continue

            clipped_areas = _clip_infill_areas_to_regions(self.api, storage, island, regions)
            try:
                groups = _classify_areas(self.api, storage, object_view, island, clipped_areas, is_first_layer)
                try:
                    ctx.set_fill_surface_groups(region_island, groups)
                finally:
                    _free_surface_groups(groups)
            finally:
                clipped_areas.free_from_storage()


def register_plugin(api):
    return PythonInitialTypedSurfaceBuilderPlugin(api)
