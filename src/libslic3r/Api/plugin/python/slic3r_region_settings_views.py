#/|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
#/|/
#/|/ SuperSlicer is released under the terms of the AGPLv3 or higher
#/|/

"""
Python helper for region-dependent settings.

Some plugin algorithms work on one whole LayerIsland because it is much faster
than processing every LayerRegion independently. RegionSettings lets those
algorithms still respect region/modifier settings by splitting the island into
the largest areas where a group of config keys has one stable value.

Typical use
-----------

Build the helper once for the current island and setting group:

    settings = RegionSettings(api, storage, island, [["extra_perimeters"]])
    settings.segregate(island.slice())

Fast path when the setting has one value for the whole island:

    if not settings.has_many_config("extra_perimeters"):
        value = settings.get_solo_config("extra_perimeters")
        if value.get_bool():
            ...

Area path when modifiers or regions produce different values:

    for value, clip in settings.get_areas("extra_perimeters"):
        if value.get_bool():
            selected = clip.intersections(candidate_areas)

An accept-all clip stores no geometry. It means "this value applies everywhere
inside the area passed to the algorithm". When a ClipperOperand is passed to
RegionSettingsClip.intersections(), the accept-all case returns the same operand
instead of materializing ExPolygons and rebuilding a Clipper operand.

Public API quick reference
--------------------------

RegionSettingsValue stores the config options for one value bucket:

* empty(), size(): inspect the option group.
* option(key=None): return the ConfigOption for key, or the first option.
* get_bool/int/float(...): typed shortcuts over option().
* is_percent(), get_effective_value(...): helpers for float-or-percent options.
* is_enabled(...): test whether a nullable option is enabled at an index.

RegionSettingsClip stores the geometry where one value applies:

* is_accept_all(): true when this value applies to the whole subject.
* expolygons(): explicit clip geometry; invalid for accept-all clips.
* intersections(subject): clip an ExPolygon collection, ExPolygon, Polyline, or
  ClipperOperand to the inside of this clip.
* diff(subject): keep the outside of this clip.
* split_to_inside_outside(subject): return both pieces at once.
* append_intersections_to(dst, subject): convenience for accumulation.
* make_accept_all(), union_self(): maintenance helpers used by RegionSettings.

RegionSettings builds value buckets for one island:

* add_region(), add_regions(), clear_regions(): choose the regions to compare.
* segregate(area): build the value -> clip map for a subject ExPolygon.
* has_many_config(key): true when more than one value/clip exists for key.
* get_areas(key): list of (RegionSettingsValue, RegionSettingsClip).
* get_regions(key, value): source LayerRegions that produced a value.
* get_solo_config(key): single RegionSettingsValue for the fast path.

ClipperOperand convention
-------------------------

Python has no rvalue references, so RegionSettingsClip cannot know whether a
ClipperOperand is safe to consume. For ClipperOperand inputs, always write the
result back to the same variable and stop using the old value:

    operand = area_clip.intersections(operand)
    operand = area_clip.diff(operand)

This is required because accept-all clips return the same ClipperOperand object.
Keeping another name for the old operand creates an alias, not an independent
copy. Use polygon/expolygon collection overloads when you need a non-consuming
operation.
"""

from __future__ import annotations

import ctypes
from typing import Iterable

from slic3r_clipper_views import (
    ClipperContext,
    ClipperOperand,
    clipper_diff,
    clipper_intersection,
    clipper_offset,
    clipper_union,
)
from slic3r_datatree_views import Config, ConfigOption, LayerIsland, LayerRegion
from slic3r_geometry_views import (
    ExPolygon,
    ExPolygonCollection,
    Polyline,
    StoredExPolygonCollection,
    StoredPolylineCollection,
)


def _address(handle) -> int:
    if handle is None:
        return 0
    if isinstance(handle, ctypes.c_void_p):
        return int(handle.value or 0)
    return int(handle)


def _void_p(handle) -> ctypes.c_void_p:
    return ctypes.c_void_p(_address(handle))


class RegionSettingsValue:
    """
    Config values for one RegionSettings entry.

    The value may hold one option or a group of options. Grouping is useful when
    an algorithm needs several settings to stay synchronized in one clipped
    area, for example an enable flag plus a numeric parameter.
    """

    def __init__(self, api, keys: Iterable[str], options: Iterable[ConfigOption]) -> None:
        """
        Store already-resolved ConfigOption views.

        Most plugin code should use RegionSettingsValue.create() or receive
        values from RegionSettings.get_areas(), not call this constructor
        directly.
        """
        self.api = api
        self._keys = [str(key) for key in keys]
        self._options = list(options)

    @classmethod
    def create(cls, default_config: Config, actual_config: Config, keys: Iterable[str]) -> "RegionSettingsValue":
        """
        Resolve a group of keys against an actual config with default fallback.

        Region modifiers may omit a key. In that case the option from
        default_config is used, matching the C++ helper behavior.
        """
        normalized_keys = [str(key) for key in keys if key is not None]
        options: list[ConfigOption] = []
        for key in normalized_keys:
            actual = actual_config.get(key)
            fallback = default_config.get(key)
            option = actual if actual is not None else fallback
            if option is None:
                raise KeyError(key)
            options.append(option)
        return cls(default_config.api, normalized_keys, options)

    def empty(self) -> bool:
        """Return true when this value contains no config options."""
        return not self._options

    def size(self) -> int:
        """Return the number of options stored in this grouped value."""
        return len(self._options)

    def _option_for_key(self, key: str | None = None) -> ConfigOption:
        if key is None:
            if not self._options:
                raise IndexError("RegionSettingsValue has no option")
            return self._options[0]
        for idx, stored_key in enumerate(self._keys):
            if stored_key == key:
                return self._options[idx]
        raise KeyError(key)

    def option(self, key: str | None = None) -> ConfigOption:
        """
        Return one ConfigOption from this value.

        key == None returns the first option in the group. Use a named key when
        a RegionSettingsValue was created with multiple related settings.
        """
        return self._option_for_key(key)

    def get_bool(self, key: str | None = None, idx: int = 0) -> bool:
        """Read a boolean option from this value."""
        return self.option(key).get_bool(idx)

    def get_int(self, key: str | None = None, idx: int = 0) -> int:
        """Read an integer option from this value."""
        return self.option(key).get_int(idx)

    def get_float(self, key: str | None = None, idx: int = 0) -> float:
        """Read a floating-point option from this value."""
        return self.option(key).get_float(idx)

    def is_percent(self, key: str | None = None, idx: int = 0) -> bool:
        """Return true when a float-or-percent option is currently percent-based."""
        return self.option(key).is_percent(idx)

    def get_effective_value(self, ratio: float, key: str | None = None, idx: int = 0) -> float:
        """
        Resolve a float-or-percent option into a concrete value.

        ratio is the reference value used when the option is expressed as a
        percentage, for example a nozzle diameter or line width.
        """
        return self.option(key).get_effective_value(ratio, idx)

    def is_enabled(self, key: str | None = None, idx: int = 0) -> bool:
        """Return true when a nullable option is enabled at idx."""
        return self.option(key).is_enabled(idx)

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, RegionSettingsValue):
            return NotImplemented
        if len(self._options) != len(other._options):
            return False
        for lhs, rhs in zip(self._options, other._options):
            if bool(self.api.host.config_option_equals(lhs.c_handle(), rhs.c_handle())):
                continue
            return False
        return True

    def __lt__(self, other: "RegionSettingsValue") -> bool:
        if len(self._options) != len(other._options):
            return len(self._options) < len(other._options)
        for lhs, rhs in zip(self._options, other._options):
            if bool(self.api.host.config_option_equals(lhs.c_handle(), rhs.c_handle())):
                continue
            return bool(self.api.host.config_option_less(lhs.c_handle(), rhs.c_handle()))
        return False


class RegionSettingsClip:
    """
    Geometry clip attached to one RegionSettingsValue.

    ``is_accept_all()`` means there is no geometry to clip with: the value
    applies to the whole subject area. Explicit geometry means the value applies
    only inside that ExPolygon collection.
    """

    def __init__(self, api, storage, expolygons: StoredExPolygonCollection | None = None) -> None:
        """
        Create a clip.

        expolygons == None creates an accept-all clip. Passing an explicit
        StoredExPolygonCollection means only that geometry is accepted.
        """
        self.api = api
        self._storage = _address(storage)
        self._expolygons = expolygons

    def storage(self) -> int:
        """Return the storage_handle address used by temporary output geometry."""
        return self._storage

    def is_accept_all(self) -> bool:
        """Return true when this clip accepts the whole subject geometry."""
        return self._expolygons is None

    def has_explicit_empty_geometry(self) -> bool:
        """Return true when this clip has explicit geometry but it is empty."""
        return self._expolygons is not None and self._expolygons.empty()

    def expolygons(self) -> ExPolygonCollection:
        """
        Return the explicit clip geometry.

        Accept-all clips deliberately have no finite geometry; call
        is_accept_all() before using this method when the caller may receive
        the single-value fast path.
        """
        if self._expolygons is None:
            raise ValueError("accept-all clip has no explicit ExPolygon geometry")
        return self._expolygons

    def handle_or_none(self) -> int | None:
        """Return the raw ExPolygonCollection handle, or None for accept-all."""
        return None if self._expolygons is None else self._expolygons.handle()

    def intersections(self, subject):
        """
        Return the part of subject accepted by this clip.

        Passing a ClipperOperand keeps the result in Clipper form. In the
        accept-all case, the same Python object is returned. Therefore use this
        form only as ``operand = clip.intersections(operand)`` and do not keep
        another live name for the old operand.
        """
        if isinstance(subject, ClipperOperand):
            if subject.storage() != self._storage:
                raise ValueError("clipper operand storage does not match RegionSettingsClip storage")
            if self.is_accept_all():
                return subject
            clip = ClipperContext(self.api, self._storage)
            return clipper_intersection(self._storage, subject, clip(self._expolygons))

        if isinstance(subject, ExPolygonCollection):
            if self.is_accept_all():
                return subject.clone(self._storage)
            clip = ClipperContext(self.api, self._storage)
            return clipper_intersection(clip(subject), clip(self._expolygons)).to_expolygon_collection()

        if isinstance(subject, ExPolygon):
            if self.is_accept_all():
                out = StoredExPolygonCollection(self.api, self._storage)
                out.push_back(subject)
                return out
            single = StoredExPolygonCollection(self.api, self._storage)
            single.push_back(subject)
            return self.intersections(single)

        if isinstance(subject, Polyline):
            if self.is_accept_all():
                out = StoredPolylineCollection(self.api, self._storage)
                out.push_back(subject)
                return out
            handle = self.api.host.clipper_intersection_polyline_expolygons(
                _void_p(self._storage), subject.c_handle(), self._expolygons.c_handle()
            )
            return StoredPolylineCollection.adopt_owned(self.api, self._storage, handle)

        raise TypeError(f"unsupported RegionSettingsClip subject: {type(subject)!r}")

    def intersections_offset(self, offset: int, subject: ExPolygonCollection) -> StoredExPolygonCollection:
        """
        Offset this clip before intersecting a polygonal subject.

        This mirrors the C++ helper used by algorithms that grow or shrink a
        region-specific setting area before applying it to candidate geometry.
        The subject is still returned unchanged in the accept-all case because
        there is no finite clip geometry to offset.
        """
        if self.is_accept_all():
            return subject.clone(self._storage)
        clip = ClipperContext(self.api, self._storage)
        clip_area = clip(self._expolygons) if int(offset) == 0 else clipper_offset(clip(self._expolygons), float(offset))
        return clipper_intersection(clip(subject), clip_area).to_expolygon_collection()

    def diff(self, subject):
        """
        Return the part of subject rejected by this clip.

        For accept-all clips, polygonal subjects return an empty collection and
        ClipperOperand subjects return an empty Clipper operand. For
        ClipperOperand inputs, use this only as ``operand = clip.diff(operand)``
        so call sites follow the same consuming convention as intersections().
        """
        if isinstance(subject, ClipperOperand):
            if subject.storage() != self._storage:
                raise ValueError("clipper operand storage does not match RegionSettingsClip storage")
            if self.is_accept_all():
                return ClipperOperand.create_empty(self.api, self._storage)
            clip = ClipperContext(self.api, self._storage)
            return clipper_diff(self._storage, subject, clip(self._expolygons))

        if isinstance(subject, ExPolygonCollection):
            if self.is_accept_all():
                return StoredExPolygonCollection(self.api, self._storage)
            clip = ClipperContext(self.api, self._storage)
            return clipper_diff(clip(subject), clip(self._expolygons)).to_expolygon_collection()

        if isinstance(subject, ExPolygon):
            if self.is_accept_all():
                return StoredExPolygonCollection(self.api, self._storage)
            single = StoredExPolygonCollection(self.api, self._storage)
            single.push_back(subject)
            return self.diff(single)

        if isinstance(subject, Polyline):
            if self.is_accept_all():
                return StoredPolylineCollection(self.api, self._storage)
            handle = self.api.host.clipper_diff_polyline_expolygons(
                _void_p(self._storage), subject.c_handle(), self._expolygons.c_handle()
            )
            return StoredPolylineCollection.adopt_owned(self.api, self._storage, handle)

        raise TypeError(f"unsupported RegionSettingsClip subject: {type(subject)!r}")

    def split_to_inside_outside(self, subject):
        """
        Split subject into accepted and rejected geometry.

        The return types match the subject family: polygonal subjects return
        StoredExPolygonCollection objects, polylines return
        StoredPolylineCollection objects, and Clipper operands stay in Clipper
        form. When subject is a ClipperOperand, treat the input as consumed and
        do not use the original variable after the split.
        """
        if isinstance(subject, ClipperOperand) and self.is_accept_all():
            return subject, ClipperOperand.create_empty(self.api, self._storage)
        return self.intersections(subject), self.diff(subject)

    def append_intersections_to(self, dst, subject) -> None:
        """
        Intersect subject and append the result to dst by move.

        dst must be a compatible stored collection. This is useful in loops that
        accumulate many RegionSettings clips into one output collection.
        """
        clipped = self.intersections(subject)
        dst.append_move_from(clipped)

    def append_copy_from(self, expolygons: ExPolygonCollection) -> None:
        """Append explicit clip geometry by copy, creating the collection if needed."""
        self._ensure_collection().append_copy_from(expolygons)

    def append_move_from(self, expolygons: StoredExPolygonCollection) -> None:
        """Append explicit clip geometry by move, creating the collection if needed."""
        self._ensure_collection().append_move_from(expolygons)

    def make_accept_all(self) -> None:
        """Replace explicit geometry by the implicit accept-all state."""
        self._expolygons = None

    def union_self(self) -> None:
        """
        Normalize explicit clip geometry into a unioned ExPolygon collection.

        RegionSettings calls this after accumulating several region slices for
        the same value so later intersections see one coherent area.
        """
        if self.is_accept_all() or self._expolygons.empty():
            return
        clip = ClipperContext(self.api, self._storage)
        self._expolygons = clipper_union(clip(self._expolygons)).to_expolygon_collection()

    def _ensure_collection(self) -> StoredExPolygonCollection:
        if self._expolygons is None:
            self._expolygons = StoredExPolygonCollection(self.api, self._storage)
        return self._expolygons


class RegionSettings:
    """
    Split an island into areas where a group of options has one value.

    The helper does not edit the data tree. It only builds a read-side map:
    config value -> geometry clip. A plugin can then intersect its candidate
    geometry with the clips it cares about.
    """

    def __init__(self, api, storage, source, option_groups: Iterable[Iterable[str]]) -> None:
        """
        Create a helper from a LayerIsland or a default Config.

        With a LayerIsland source, all island regions are added automatically
        and the first region config becomes the default. With a Config source,
        call add_region() or add_regions() before segregate().
        """
        self.api = api
        self._storage = _address(storage)
        self._option_groups = self._normalize_groups(option_groups)
        self._regions: list[LayerRegion] = []
        self._key_areas: dict[str, list[tuple[RegionSettingsValue, RegionSettingsClip]]] = {}
        self._key_regions: dict[str, list[tuple[RegionSettingsValue, list[LayerRegion]]]] = {}

        if isinstance(source, LayerIsland):
            self._default_config = self._first_region_config(source)
            self.add_regions(source)
        elif isinstance(source, Config):
            self._default_config = source
        else:
            raise TypeError("RegionSettings source must be a Config or LayerIsland")

    def clear_regions(self) -> None:
        """Remove all regions from this helper before adding a new set."""
        self._regions.clear()

    def add_region(self, region: LayerRegion) -> None:
        """Add one LayerRegion whose config and slices participate in segregation."""
        self._regions.append(region)

    def add_regions(self, island: LayerIsland) -> None:
        """Add every LayerRegion referenced by an island."""
        for region in island.regions():
            self.add_region(region)

    def segregate(self, area: ExPolygon) -> None:
        """
        Rebuild the value/clip maps for a subject island area.

        Regions are compared against the default config first. If every region
        has the same option group value, the helper stores one accept-all clip.
        If values differ, each region slice is intersected with ``area`` and
        accumulated under the matching RegionSettingsValue.
        """
        self._key_areas.clear()
        self._key_regions.clear()
        clip = ClipperContext(self.api, self._storage)

        for group in self._option_groups:
            if not group:
                continue

            primary_key = group[0]
            default_value = RegionSettingsValue.create(self._default_config, self._default_config, group)

            many_values = False
            for region in self._regions:
                region_value = RegionSettingsValue.create(self._default_config, region.print_region().config(), group)
                if region_value != default_value:
                    many_values = True
                    break

            areas: list[tuple[RegionSettingsValue, RegionSettingsClip]] = []
            regions: list[tuple[RegionSettingsValue, list[LayerRegion]]] = []

            if many_values:
                for region in self._regions:
                    region_value = RegionSettingsValue.create(self._default_config, region.print_region().config(), group)
                    region_clip = self._area_entry(areas, region_value)
                    self._regions_entry(regions, region_value).append(region)

                    if not region.slices().empty():
                        overlap = clipper_intersection(clip(region.slices()), clip(area)).to_expolygon_collection()
                        if not overlap.empty():
                            region_clip.append_move_from(overlap)

                areas = [(value, region_clip) for value, region_clip in areas
                         if not region_clip.has_explicit_empty_geometry()]
                if len(areas) == 1:
                    areas[0][1].make_accept_all()
                else:
                    for _, region_clip in areas:
                        region_clip.union_self()

                areas.sort(key=lambda item: _RegionSettingsValueSortKey(item[0]))
                regions.sort(key=lambda item: _RegionSettingsValueSortKey(item[0]))

            if not areas:
                areas = [(default_value, RegionSettingsClip(self.api, self._storage))]
                regions = [(default_value, list(self._regions))]

            self._key_areas[primary_key] = areas
            self._key_regions[primary_key] = regions

    def has_many_config(self, key: str) -> bool:
        """Return true when key has more than one value bucket after segregate()."""
        areas = self._key_areas.get(key)
        return areas is not None and len(areas) > 1

    def get_areas(self, key: str) -> list[tuple[RegionSettingsValue, RegionSettingsClip]]:
        """
        Return the value/clip pairs for key.

        Call segregate() first. A single pair with an accept-all clip is the
        normal fast path when all regions share the same value.
        """
        if key not in self._key_areas:
            raise KeyError(key)
        return self._key_areas[key]

    def get_regions(self, key: str, value: RegionSettingsValue) -> list[LayerRegion]:
        """
        Return the source regions that produced a value bucket.

        This is useful when clipped output has to be published into a
        LayerRegionIsland with matching region ownership.
        """
        if key not in self._key_regions:
            raise KeyError(key)
        for stored_value, regions in self._key_regions[key]:
            if stored_value == value:
                return regions
        raise KeyError(key)

    def get_solo_config(self, key: str) -> RegionSettingsValue:
        """
        Return the only value for key.

        Use this after has_many_config(key) returned False. It raises if the key
        still has multiple values, because the caller would otherwise ignore
        region-specific settings.
        """
        areas = self.get_areas(key)
        if len(areas) != 1:
            raise ValueError(f"{key} has {len(areas)} RegionSettings entries")
        return areas[0][0]

    @staticmethod
    def _normalize_groups(option_groups: Iterable[Iterable[str]]) -> list[list[str]]:
        return [[str(key) for key in group if key is not None] for group in option_groups]

    @staticmethod
    def _first_region_config(island: LayerIsland) -> Config:
        if island.region_count() == 0:
            raise ValueError("RegionSettings requires an island with at least one region")
        return island.region(0).print_region().config()

    def _area_entry(self,
                    areas: list[tuple[RegionSettingsValue, RegionSettingsClip]],
                    value: RegionSettingsValue) -> RegionSettingsClip:
        for stored_value, region_clip in areas:
            if stored_value == value:
                return region_clip
        region_clip = RegionSettingsClip(self.api, self._storage)
        areas.append((value, region_clip))
        return region_clip

    @staticmethod
    def _regions_entry(regions: list[tuple[RegionSettingsValue, list[LayerRegion]]],
                       value: RegionSettingsValue) -> list[LayerRegion]:
        for stored_value, stored_regions in regions:
            if stored_value == value:
                return stored_regions
        stored_regions: list[LayerRegion] = []
        regions.append((value, stored_regions))
        return stored_regions


class _RegionSettingsValueSortKey:
    def __init__(self, value: RegionSettingsValue) -> None:
        self.value = value

    def __lt__(self, other: "_RegionSettingsValueSortKey") -> bool:
        return self.value < other.value


__all__ = [
    "RegionSettings",
    "RegionSettingsClip",
    "RegionSettingsValue",
]
