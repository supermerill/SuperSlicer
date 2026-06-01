#/|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
#/|/
#/|/ SuperSlicer is released under the terms of the AGPLv3 or higher
#/|/

"""
Python helper for STEP_POST_INFILL plugins.

Post-infill plugins run after infill pattern generation has written extrusion
trees into each LayerRegionIsland. They are cleanup or finishing passes: gap
fill, ironing preparation, local extrusion edits, or other algorithms that need
the final infill geometry as input.

Context contents
----------------

PostInfillContext exposes:

* read-only print() and object() views;
* get_or_create_region_island(), which returns the LayerRegionIsland associated
  with one island, a compatible set of LayerRegions, and an extrusion role;
* mutable_extrusion(), the explicit callback that borrows one infill-owned
  extrusion bucket from a LayerRegionIsland;
* plugin_storage(), cancellation, progress, warning, and error helpers.

Typical use
-----------

    ctx = api.post_infill(run_ctx_address)
    for layer in ctx.object().layers():
        for island in layer.islands():
            region_island = ctx.get_or_create_region_island(island, island.regions())
            if region_island is not None:
                root = ctx.mutable_extrusion(region_island, RAW_EXTRUSION_ROLE_INTERNAL_INFILL)
                if root is not None:
                    # Edit the generated infill tree in place.
                    root.disable_reverse()

The mutable handle is borrowed from the host and remains valid only for the
current PluginBase.run() call. For infill, gap-fill, and ironing roles the
callback returns a root collection, creating an empty root when needed. It
returns None for roles owned by another step, such as perimeter or support
roles.
"""

from __future__ import annotations

import ctypes

from slic3r_api_generated import (
    PLUGIN_IS_CANCELLED,
    PLUGIN_REPORT,
    PLUGIN_REPORT_PROGRESS,
    PluginRunContext,
    RAW_EXTRUSION_ROLE_INTERNAL_INFILL,
    RunCtxPostInfillGeneration,
    STEP_POST_INFILL,
)
from slic3r_datatree_views import LayerIsland, LayerRegion, LayerRegionIsland, Object, Print
from slic3r_extrusion_views import MutableExtrusionEntity


def _address(handle) -> int:
    if handle is None:
        return 0
    if isinstance(handle, ctypes.c_void_p):
        return int(handle.value or 0)
    if hasattr(handle, "c_handle"):
        return _address(handle.c_handle())
    return int(handle)


def _as_bytes(text: str) -> bytes:
    return text.encode("utf-8")


def _optional_bytes(text: str | None) -> bytes | None:
    return None if text is None else text.encode("utf-8")


def _region_handle(region) -> int:
    if isinstance(region, LayerRegion):
        return region.address
    return _address(region)


class PostInfillContext:
    """
    High-level wrapper around the STEP_POST_INFILL payload.

    The context keeps normal data-tree traversal read-only, then exposes the
    one host-approved mutation for this step: borrowing a mutable extrusion root
    for infill, gap-fill, or ironing output already present on a
    LayerRegionIsland.
    """

    def __init__(self, api, common: PluginRunContext, payload_ptr) -> None:
        self.api = api
        self.common = common
        self._payload_ptr = payload_ptr
        self.payload = payload_ptr.contents

    @classmethod
    def from_run_context(cls, api, run_ctx_address: int) -> "PostInfillContext | None":
        if not run_ctx_address:
            return None
        common = PluginRunContext.from_address(int(run_ctx_address))
        if common.step != STEP_POST_INFILL or not common.data:
            return None
        payload_ptr = ctypes.cast(common.data, ctypes.POINTER(RunCtxPostInfillGeneration))
        payload = payload_ptr.contents
        if not payload.print or not payload.object:
            return None
        if not payload.get_or_create_region_island or not payload.get_region_island_mutable_extrusion:
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
        regions,
        role: int = RAW_EXTRUSION_ROLE_INTERNAL_INFILL,
    ) -> LayerRegionIsland | None:
        """
        Return the destination LayerRegionIsland for one island/region group.

        ``regions`` is normally a list returned by ``island.regions()`` or by
        ``region_island.regions()``. Passing None or an empty list asks the host
        to use all regions of the island. The returned view is borrowed from
        the host and should be used only during the current plugin run.

        ``role`` chooses the extruder used in the LayerRegionIsland key. If the
        selected regions do not share one extruder for that role, the host
        returns None and leaves the data tree unchanged.
        """
        region_addresses = [] if regions is None else [_region_handle(region) for region in regions]
        region_array = None
        if region_addresses:
            region_array = (ctypes.c_void_p * len(region_addresses))(*region_addresses)
        handle = self.payload.get_or_create_region_island(
            island.c_handle(),
            region_array,
            len(region_addresses),
            int(role),
        )
        return None if not handle else LayerRegionIsland(self.api, handle)

    def mutable_extrusion(self, region_island: LayerRegionIsland, role: int) -> MutableExtrusionEntity | None:
        handle = self.payload.get_region_island_mutable_extrusion(region_island.c_handle(), int(role))
        if not handle:
            return None
        return MutableExtrusionEntity(self.api, handle)


__all__ = ["PostInfillContext"]
