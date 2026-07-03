#/|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
#/|/
#/|/ SuperSlicer is released under the terms of the AGPLv3 or higher
#/|/

"""
Python helper for the perimeter generation plugin step.

The STEP_PERIMETER payload gives a plugin one layer island at a time. A normal
perimeter generator groups compatible layer regions, then calls
run_region_group(). The host owns the perimeter-node tree, calls registered
perimeter modules before/after each generated ring, and publishes the final
extrusions and fill areas back into the layer data tree.

Context contents
----------------

PerimeterContext exposes:

* read-only print(), object(), layer(), and island() views for the island being
  processed;
* LayerIsland.get_or_create_region_island(), from the data-tree views, when a
  generator needs an additional destination LayerRegionIsland;
* set_region_island_extrusion(), which publishes a generated extrusion tree to
  the selected LayerRegionIsland;
* run_region_group(), the host-managed perimeter loop. The plugin provides a
  synchronous node callback and the host calls active perimeter modules around
  each generated node.

PerimeterGenerationContextView is passed to the node callback and to perimeter
modules. It exposes the temporary root node, current LayerRegionIsland,
plugin_storage(), layer index, and callbacks to split a node or rebuild its
children. PerimeterNodeView exposes the node area, fill area, mutable extrusion
bucket, perimeter indexes, parent/children, and helpers for requesting more
perimeters.

Typical use
-----------

    ctx = api.perimeter(run_ctx_address)
    if ctx is None:
        return

    island = ctx.island()
    regions = list(island.regions())
    flow = regions[0].flow(RAW_EXTRUSION_ROLE_EXTERNAL_PERIMETER)

    def generate(state, generation, node, inner_areas, inner_fill_areas):
        # Write node.extrusions(), then move generated child areas into
        # inner_areas / inner_fill_areas.
        return True

    ctx.run_region_group(regions, island.slice(), {"flow": flow}, generate)

The generator callback is synchronous. The state object and ctypes callback are
kept alive until run_region_group() returns.
"""

from __future__ import annotations

import ctypes
from typing import Callable, Iterable

from slic3r_api_generated import (
    PERIMETER_GENERATE_NODE,
    PLUGIN_IS_CANCELLED,
    PLUGIN_REPORT,
    PLUGIN_REPORT_PROGRESS,
    PerimeterGenerationContext,
    PerimeterNode,
    PerimeterNodeSpan,
    PluginRunContext,
    RunCtxGeneratePerimeter,
    STEP_PERIMETER,
)
from slic3r_datatree_views import Layer, LayerIsland, LayerRegion, LayerRegionIsland, Object, Print
from slic3r_extrusion_views import ExtrusionEntity, MutableExtrusionEntity
from slic3r_geometry_views import ExPolygon, ExPolygonCollection, MutableExPolygonCollection


def _address(handle) -> int:
    if handle is None:
        return 0
    if isinstance(handle, ctypes.c_void_p):
        return int(handle.value or 0)
    if hasattr(handle, "c_handle"):
        return _address(handle.c_handle())
    if hasattr(handle, "handle"):
        return _address(handle.handle())
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


def _expolygon_handle(expolygon) -> int:
    if expolygon is None:
        return 0
    if isinstance(expolygon, ExPolygon):
        return expolygon.address
    return _address(expolygon)


def _expolygon_collection_handle(collection) -> int:
    if collection is None:
        return 0
    if isinstance(collection, ExPolygonCollection):
        return collection.address
    return _address(collection)


def _extrusion_handle(extrusion) -> int:
    if extrusion is None:
        return 0
    if isinstance(extrusion, ExtrusionEntity):
        return extrusion.address
    return _address(extrusion)


class PerimeterNodeView:
    """
    Borrowed mutable view over one host-owned perimeter node.

    The node and its handles are valid only during the current generator or
    module callback. Store generated perimeter extrusion in extrusions(), then
    return child areas through the output collections passed to the
    generator callback.
    """

    def __init__(self, api, node_ptr: ctypes.POINTER(PerimeterNode)) -> None:
        self.api = api
        self._ptr = node_ptr

    @property
    def raw(self) -> PerimeterNode:
        return self._ptr.contents

    def area(self) -> ExPolygon:
        return ExPolygon(self.api, self.raw.area)

    def fill_area(self) -> ExPolygon:
        return ExPolygon(self.api, self.raw.fill_area)

    def extrusions(self) -> MutableExtrusionEntity:
        return MutableExtrusionEntity(self.api, self.raw.extrusions)

    def perimeter_idx(self) -> int:
        return int(self.raw.perimeter_idx)

    def perimeter_needed(self) -> int:
        return int(self.raw.perimeter_needed)

    def set_perimeter_needed(self, value: int) -> None:
        self.raw.perimeter_needed = int(value)

    def parent(self) -> "PerimeterNodeView | None":
        return None if not self.raw.parent else PerimeterNodeView(
            self.api,
            ctypes.cast(self.raw.parent, ctypes.POINTER(PerimeterNode)),
        )

    def child_count(self) -> int:
        return int(self.raw.child_count)

    def child(self, idx: int) -> "PerimeterNodeView":
        if idx < 0 or idx >= self.child_count():
            raise IndexError(idx)
        if not self.raw.children:
            raise IndexError(idx)
        child_ptr = ctypes.cast(self.raw.children[idx], ctypes.POINTER(PerimeterNode))
        return PerimeterNodeView(self.api, child_ptr)

    def children_snapshot(self) -> list["PerimeterNodeView"]:
        """
        Copy child node pointers before a loop that may structurally edit them.

        split_node() and rebuild_children() may replace the parent child array.
        The returned views point to nodes, not to array slots, so the snapshot
        remains useful for the common split workflow.
        """
        return [self.child(idx) for idx in range(self.child_count())]

    def needs_more_perimeters(self) -> bool:
        return self.perimeter_needed() > 0 and self.perimeter_idx() < self.perimeter_needed()

    def is_last_perimeter(self) -> bool:
        return self.perimeter_needed() == 0 or self.perimeter_idx() + 1 >= self.perimeter_needed()

    def add_perimeters(self, count: int) -> None:
        if count > 0:
            self.raw.perimeter_needed = int(self.raw.perimeter_needed) + int(count)

    def request_current_perimeter(self) -> None:
        self.raw.perimeter_needed = max(int(self.raw.perimeter_needed), int(self.raw.perimeter_idx) + 1)


class PerimeterGenerationContextView:
    """
    Borrowed context shared with one run_region_group() traversal.

    It exposes the same print/object/layer/island handles as the parent
    STEP_PERIMETER payload, plus the plugin storage used for temporary geometry.
    """

    def __init__(self, api, context_ptr: ctypes.POINTER(PerimeterGenerationContext)) -> None:
        self.api = api
        self._ptr = context_ptr

    @property
    def raw(self) -> PerimeterGenerationContext:
        return self._ptr.contents

    @property
    def common(self) -> PluginRunContext:
        return self.raw.run_ctx.contents

    def plugin_storage(self) -> int:
        return _address(self.common.plugin_storage)

    def print(self) -> Print:
        return Print(self.api, self.raw.print)

    def object(self) -> Object:
        return Object(self.api, self.raw.object)

    def layer(self) -> Layer:
        return Layer(self.api, self.raw.layer)

    def island(self) -> LayerIsland:
        return LayerIsland(self.api, self.raw.island)

    def region_island(self) -> LayerRegionIsland:
        return LayerRegionIsland(self.api, self.raw.region_island)

    def root(self) -> PerimeterNodeView:
        return PerimeterNodeView(self.api, self.raw.root)

    def layer_id_from_object(self) -> int:
        current_layer = self.layer()
        for layer_idx, layer in enumerate(self.object().layers()):
            if layer.same_handle(current_layer):
                return layer_idx
        return -1

    def split_node(self, node: PerimeterNodeView, clip) -> list[PerimeterNodeView]:
        """
        Split a node with a clip ExPolygonCollection and return inside nodes.

        clip may be None to mean "accept all". Structural edits are owned by
        the host perimeter loop; callers should refresh child snapshots after
        calling this helper.
        """
        if clip is None:
            return [node]
        if not self.raw.split_node:
            return []

        span = PerimeterNodeSpan()
        self.raw.split_node(
            self._ptr,
            node._ptr,
            _void_p(_expolygon_collection_handle(clip)),
            ctypes.byref(span),
        )
        out: list[PerimeterNodeView] = []
        for idx in range(int(span.count)):
            if span.items and span.items[idx]:
                out.append(PerimeterNodeView(self.api, ctypes.cast(span.items[idx], ctypes.POINTER(PerimeterNode))))
        return out

    def rebuild_children(self, node: PerimeterNodeView, areas, fill_areas=None) -> bool:
        if not self.raw.rebuild_children:
            return False
        self.raw.rebuild_children(
            self._ptr,
            node._ptr,
            _void_p(_expolygon_collection_handle(areas)),
            _void_p(_expolygon_collection_handle(fill_areas)),
        )
        return True


class PerimeterContext:
    """
    High-level wrapper around the STEP_PERIMETER payload.

    The context is valid only for the current PluginBase.run() call. Use
    run_region_group() for new generators; the low-level publication callbacks
    are intentionally not wrapped here.
    """

    def __init__(self, api, common: PluginRunContext, payload_ptr) -> None:
        self.api = api
        self.common = common
        self._payload_ptr = payload_ptr
        self.payload = payload_ptr.contents
        self._callbacks = []
        self._states = {}

    @classmethod
    def from_run_context(cls, api, run_ctx_address: int) -> "PerimeterContext | None":
        if not run_ctx_address:
            return None
        common = PluginRunContext.from_address(int(run_ctx_address))
        if common.step != STEP_PERIMETER or not common.data:
            return None
        payload_ptr = ctypes.cast(common.data, ctypes.POINTER(RunCtxGeneratePerimeter))
        payload = payload_ptr.contents
        if not payload.print or not payload.object or not payload.layer or not payload.island:
            return None
        if not payload.run_region_group:
            return None
        return cls(api, common, payload_ptr)

    def plugin_storage(self) -> int:
        return _address(self.common.plugin_storage)

    def print(self) -> Print:
        return Print(self.api, self.payload.print)

    def object(self) -> Object:
        return Object(self.api, self.payload.object)

    def layer(self) -> Layer:
        return Layer(self.api, self.payload.layer)

    def island(self) -> LayerIsland:
        return LayerIsland(self.api, self.payload.island)

    def set_region_island_extrusion(
        self,
        region_island: LayerRegionIsland,
        role: int,
        extrusion,
    ) -> bool:
        if not self.payload.set_region_island_extrusion:
            return False
        return bool(self.payload.set_region_island_extrusion(
            region_island.c_handle(),
            int(role),
            _void_p(_extrusion_handle(extrusion)),
        ))

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

    def run_region_group(
        self,
        regions: Iterable[LayerRegion],
        root_area: ExPolygon | None,
        generator_state,
        generate_node: Callable[
            [object, PerimeterGenerationContextView, PerimeterNodeView, MutableExPolygonCollection, MutableExPolygonCollection],
            bool | int,
        ],
    ) -> int:
        region_addresses = [_region_handle(region) for region in regions]
        region_array = None
        if region_addresses:
            region_array = (ctypes.c_void_p * len(region_addresses))(*region_addresses)

        state_key = id(generator_state)
        self._states[state_key] = generator_state

        def callback(raw_state, raw_context, raw_node, inner_areas, inner_fill_areas) -> int:
            try:
                state = self._states.get(_address(raw_state))
                if state is None or not raw_context or not raw_node:
                    return 0
                generation = PerimeterGenerationContextView(self.api, raw_context)
                node = PerimeterNodeView(self.api, raw_node)
                inner = MutableExPolygonCollection(self.api, inner_areas)
                inner_fill = MutableExPolygonCollection(self.api, inner_fill_areas)
                return 1 if generate_node(state, generation, node, inner, inner_fill) else 0
            except Exception as exc:
                self.report_error(f"Python perimeter generator callback failed: {exc}")
                return 0

        c_callback = PERIMETER_GENERATE_NODE(callback)
        self._callbacks.append(c_callback)
        try:
            return int(self.payload.run_region_group(
                ctypes.cast(self._payload_ptr, ctypes.c_void_p),
                region_array,
                len(region_addresses),
                _void_p(_expolygon_handle(root_area)),
                ctypes.c_void_p(state_key),
                c_callback,
            ))
        finally:
            self._states.pop(state_key, None)
            self._callbacks.remove(c_callback)


__all__ = [
    "PerimeterContext",
    "PerimeterGenerationContextView",
    "PerimeterNodeView",
]
