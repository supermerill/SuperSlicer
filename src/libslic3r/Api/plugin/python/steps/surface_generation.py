#/|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
#/|/
#/|/ SuperSlicer is released under the terms of the AGPLv3 or higher
#/|/

"""
Python helper for the surface generation plugin step.

STEP_SURFACE_GENERATION runs after perimeters. The perimeter step has already
computed island-level infill areas; a surface generator turns those areas into
LayerRegionIsland fill surfaces that later steps classify as top, bottom,
bridge, sparse, solid, and so on.

Context contents
----------------

SurfaceGenerationContext exposes:

* read-only print() and object() views; the plugin iterates layers, islands, and
  LayerRegionIslands through the data-tree views;
* get_or_create_region_island(), which returns the LayerRegionIsland associated
  with one island and a compatible set of LayerRegions;
* set_fill_surfaces() and set_fill_surface_groups(), which build host-owned
  Surface objects from ExPolygon areas and move them into a LayerRegionIsland;
* append_surface_like(), for copying the non-geometric attributes of an
  existing Surface while replacing its geometry;
* clear_fill_surfaces(), plugin_storage(), cancellation, progress, warning, and
  error helpers.

Typical use
-----------

    ctx = api.surface_generation(run_ctx_address)
    obj = ctx.object()
    for layer in obj.layers():
        for island in layer.islands():
            regions = list(island.regions())
            region_island = ctx.get_or_create_region_island(island, regions)
            ctx.set_fill_surfaces(region_island, island.infill_areas(),
                                  RAW_SURFACE_TYPE_POS_INTERNAL | RAW_SURFACE_TYPE_DENS_SPARSE)

set_fill_surfaces() builds a temporary SurfaceCollection in plugin storage and
asks the host callback to move it into the LayerRegionIsland. The source
ExPolygon collection may be borrowed or storage-owned; it only needs to stay
alive until set_fill_surfaces() returns.
"""

from __future__ import annotations

import ctypes
from typing import Iterable

from slic3r_api_generated import (
    PLUGIN_IS_CANCELLED,
    PLUGIN_REPORT,
    PLUGIN_REPORT_PROGRESS,
    PluginRunContext,
    RunCtxSurfaceGeneration,
    STEP_SURFACE_GENERATION,
)
from slic3r_datatree_views import LayerIsland, LayerRegion, LayerRegionIsland, Object, Print, Surface
from slic3r_geometry_views import ExPolygonCollection


def _address(handle) -> int:
    if handle is None:
        return 0
    if isinstance(handle, ctypes.c_void_p):
        return int(handle.value or 0)
    if hasattr(handle, "c_handle"):
        return _address(handle.c_handle())
    return int(handle)


def _void_p(handle) -> ctypes.c_void_p:
    return ctypes.c_void_p(_address(handle))


def _as_bytes(text: str) -> bytes:
    return text.encode("utf-8")


def _optional_bytes(text: str | None) -> bytes | None:
    return None if text is None else text.encode("utf-8")


def _region_handle(region) -> int:
    if isinstance(region, LayerRegion):
        return region.address
    return _address(region)


def _areas_handle(areas) -> int:
    if areas is None:
        return 0
    if isinstance(areas, ExPolygonCollection):
        return areas.address
    return _address(areas)


class SurfaceGenerationContext:
    """
    High-level wrapper around the STEP_SURFACE_GENERATION payload.

    The context is valid only for the current PluginBase.run() call. It exposes
    read-only print/object views plus publishing callbacks for the fill surfaces
    owned by LayerRegionIsland.
    """

    def __init__(self, api, common: PluginRunContext, payload_ptr) -> None:
        self.api = api
        self.common = common
        self._payload_ptr = payload_ptr
        self.payload = payload_ptr.contents

    @classmethod
    def from_run_context(cls, api, run_ctx_address: int) -> "SurfaceGenerationContext | None":
        if not run_ctx_address:
            return None
        common = PluginRunContext.from_address(int(run_ctx_address))
        if common.step != STEP_SURFACE_GENERATION or not common.data:
            return None
        payload_ptr = ctypes.cast(common.data, ctypes.POINTER(RunCtxSurfaceGeneration))
        payload = payload_ptr.contents
        if not payload.print or not payload.object:
            return None
        if (not payload.get_or_create_region_island or
                not payload.set_region_island_fill_surfaces or
                not payload.append_surface_like):
            return None
        return cls(api, common, payload_ptr)

    def plugin_storage(self) -> int:
        return _address(self.common.plugin_storage)

    def print(self) -> Print:
        return Print(self.api, self.payload.print)

    def object(self) -> Object:
        return Object(self.api, self.payload.object)

    def is_cancelled(self) -> bool:
        if not self.common.is_cancelled:
            return False
        return bool(PLUGIN_IS_CANCELLED(self.common.is_cancelled)(self.common.host_context))

    def report_warning(self, message: str) -> None:
        if self.common.report_warning:
            PLUGIN_REPORT(self.common.report_warning)(self.common.host_context, _as_bytes(message))

    def report_error(self, message: str) -> None:
        if self.common.report_error:
            PLUGIN_REPORT(self.common.report_error)(self.common.host_context, _as_bytes(message))

    def report_progress(self, progress: float, message: str | None = None) -> None:
        if self.common.report_progress:
            PLUGIN_REPORT_PROGRESS(self.common.report_progress)(
                self.common.host_context, float(progress), _optional_bytes(message)
            )

    def get_or_create_region_island(
        self,
        island: LayerIsland,
        regions: Iterable[LayerRegion],
    ) -> LayerRegionIsland | None:
        region_addresses = [_region_handle(region) for region in regions]
        region_array = None
        if region_addresses:
            region_array = (ctypes.c_void_p * len(region_addresses))(*region_addresses)
        handle = self.payload.get_or_create_region_island(
            island.c_handle(),
            region_array,
            len(region_addresses),
        )
        return None if not handle else LayerRegionIsland(self.api, handle)

    def set_fill_surfaces(self, region_island: LayerRegionIsland, areas, surface_type: int) -> bool:
        return self.set_fill_surface_groups(region_island, [(areas, surface_type)])

    def set_fill_surface_groups(self, region_island: LayerRegionIsland, groups) -> bool:
        """
        Move a complete SurfaceCollection into a LayerRegionIsland.

        ``groups`` is an iterable of ``(areas, surface_type)`` pairs. All groups
        are appended to one temporary collection before the callback is called,
        so top/bottom/internal classification does not overwrite itself.
        """
        storage = _void_p(self.plugin_storage())
        surfaces = self.api.host.storage_new_surface_collection(storage)
        if not surfaces:
            return False
        try:
            for areas, surface_type in groups:
                if areas is None:
                    continue
                if hasattr(areas, "empty") and areas.empty():
                    continue
                self.api.host.surface_collection_append_expolygons_copy(
                    surfaces,
                    _void_p(_areas_handle(areas)),
                    int(surface_type),
                )
            return bool(self.payload.set_region_island_fill_surfaces(region_island.c_handle(), surfaces))
        finally:
            self.api.host.storage_free(storage, surfaces)

    def append_surface_like(self, surface_collection, source: Surface, areas) -> None:
        """
        Append clipped pieces of an existing Surface to a temporary collection.

        Use this when a surface-generation plugin splits an existing Surface and
        must keep the host-side metadata attached to it. New surfaces with only
        a type bitmask should use surface_collection_append_expolygons_copy()
        instead.
        """
        if areas is None:
            return
        if hasattr(areas, "empty") and areas.empty():
            return
        self.payload.append_surface_like(
            _void_p(surface_collection),
            source.c_handle(),
            _void_p(_areas_handle(areas)),
        )

    def clear_fill_surfaces(self, region_island: LayerRegionIsland) -> bool:
        return bool(self.payload.set_region_island_fill_surfaces(region_island.c_handle(), None))


__all__ = [
    "SurfaceGenerationContext",
]
