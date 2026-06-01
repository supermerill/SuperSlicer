#/|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
#/|/
#/|/ SuperSlicer is released under the terms of the AGPLv3 or higher
#/|/

"""
Python Clipper helpers over the SuperSlicer plugin C ABI.

Clipper operations work on a temporary ``ClipperOperand``. An operand is a
storage-owned adapter over a polygon, polyline, polygon collection, ExPolygon,
or ExPolygon collection. Creating an operand from existing geometry is cheap:
the source geometry is referenced, not copied, so keep that source alive while
the operand is used.

Typical use
-----------

Create a small clipping context for the current storage:

    clip = api.clipper(storage_handle)
    unsupported = clip.diff(clip(island.slice()), clip(lower_slices))
    unsupported_polygons = unsupported.to_expolygon_collection()

Build an accumulator, then union it when geometric merging is needed:

    acc = clip.empty()
    acc += clip(first_area)
    acc += clip(second_area)
    merged = clip.union(acc)

Write a result into an already-owned destination collection:

    result = clip.offset(clip(input_polygons), scale_i(0.2))
    result.write_expolygons_to(output_expolygons)

Lifetime model
--------------
- ClipperOperand objects returned by this module own their handle unless they
  were created with ``ClipperOperand.null()``.
- Owned handles are released with ``storage_free()`` when the object is closed,
  exits a ``with`` block, or is garbage-collected.
- Collections returned by ``to_polygon_collection()`` and
  ``to_expolygon_collection()`` are also storage-owned wrappers and follow the
  geometry helper lifetime rules.
"""

from __future__ import annotations

import ctypes
from typing import Callable

from slic3r_api_generated import (
    CLIPPER_END_CLOSED_LINE,
    CLIPPER_END_CLOSED_POLYGON,
    CLIPPER_END_OPEN_BUTT,
    CLIPPER_END_OPEN_ROUND,
    CLIPPER_END_OPEN_SQUARE,
    CLIPPER_JOIN_MITER,
    CLIPPER_JOIN_ROUND,
    CLIPPER_JOIN_SQUARE,
    CLIPPER_OPERATION_DIFFERENCE,
    CLIPPER_OPERATION_INTERSECTION,
    CLIPPER_OPERATION_UNION,
    CLIPPER_OPERATION_XOR,
)

from slic3r_geometry_views import (
    ExPolygon,
    ExPolygonCollection,
    Polygon,
    PolygonCollection,
    Polyline,
    StoredExPolygonCollection,
    StoredPolygonCollection,
)


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


def _same_storage(lhs: "ClipperOperand", rhs: "ClipperOperand") -> bool:
    return lhs.storage() == rhs.storage()


def _check_same_storage(lhs: "ClipperOperand", rhs: "ClipperOperand") -> None:
    if not _same_storage(lhs, rhs):
        raise ValueError("clipper operands must use the same storage")


def _binary_args(storage_or_subject, subject_or_clip, clip=None):
    if clip is None:
        subject = storage_or_subject
        other = subject_or_clip
        _check_same_storage(subject, other)
        return subject.api, subject.storage(), subject, other
    subject = subject_or_clip
    other = clip
    storage = _require_handle(storage_or_subject, "storage")
    if subject.storage() != storage or other.storage() != storage:
        raise ValueError("explicit storage must match both clipper operands")
    return subject.api, storage, subject, other


def _unary_args(storage_or_subject, subject=None):
    if subject is None:
        operand = storage_or_subject
        return operand.api, operand.storage(), operand
    storage = _require_handle(storage_or_subject, "storage")
    if subject.storage() != storage:
        raise ValueError("explicit storage must match the clipper operand")
    return subject.api, storage, subject


class ClipperOperand:
    """
    Storage-owned Clipper input or result.

    Use ``api.clipper(storage)`` to build operands from geometry and chain
    operations. A borrowed geometry view may be wrapped as an operand, but the
    operand does not copy it. If the source geometry is modified or freed while
    the operand exists, the operand becomes invalid too.
    """

    def __init__(self, api, storage, source=None) -> None:
        self.api = api
        self._storage = _require_handle(storage, "storage")
        self._handle = 0
        self._owns_handle = False

        if source is None:
            self._handle = _address(self.api.host.clipper_shapes_create_empty(self.c_storage()))
            self._owns_handle = True
        else:
            self._handle = self._create_from_source(source)
            self._owns_handle = True

    @classmethod
    def null(cls, api, storage) -> "ClipperOperand":
        """Create an empty view with no storage handle allocated."""
        out = cls.__new__(cls)
        out.api = api
        out._storage = _require_handle(storage, "storage")
        out._handle = 0
        out._owns_handle = False
        return out

    @classmethod
    def adopt_owned(cls, api, storage, handle) -> "ClipperOperand":
        """Take ownership of a Clipper handle allocated in storage."""
        out = cls.__new__(cls)
        out.api = api
        out._storage = _require_handle(storage, "storage")
        out._handle = _require_handle(handle)
        out._owns_handle = True
        return out

    @classmethod
    def create_empty(cls, api, storage) -> "ClipperOperand":
        """Create a storage-owned operand that contains no paths."""
        return cls(api, storage)

    def _create_from_source(self, source) -> int:
        if isinstance(source, Polygon):
            return _address(self.api.host.clipper_shapes_from_polygon(self.c_storage(), source.c_handle()))
        if isinstance(source, Polyline):
            return _address(self.api.host.clipper_shapes_from_polyline(self.c_storage(), source.c_handle()))
        if isinstance(source, PolygonCollection):
            return _address(self.api.host.clipper_shapes_from_polygons(self.c_storage(), source.c_handle()))
        if isinstance(source, ExPolygon):
            return _address(self.api.host.clipper_shapes_from_expolygon(self.c_storage(), source.c_handle()))
        if isinstance(source, ExPolygonCollection):
            return _address(self.api.host.clipper_shapes_from_expolygons(self.c_storage(), source.c_handle()))
        raise TypeError(f"unsupported Clipper source type: {type(source)!r}")

    @property
    def address(self) -> int:
        return self._handle

    @property
    def storage_address(self) -> int:
        return self._storage

    def storage(self) -> int:
        return self._storage

    def c_storage(self) -> ctypes.c_void_p:
        return _void_p(self._storage)

    def handle(self) -> int:
        return self._handle

    def c_handle(self) -> ctypes.c_void_p:
        return _void_p(self._handle)

    def is_valid_handle(self) -> bool:
        return self._handle != 0

    def empty(self) -> bool:
        return bool(self.api.host.clipper_shapes_empty(self.c_handle()))

    def same_handle(self, other: "ClipperOperand") -> bool:
        return self._handle == other.handle()

    def release(self) -> int:
        handle = self._handle
        self._handle = 0
        self._owns_handle = False
        return handle

    def free_from_storage(self) -> bool:
        if not self._owns_handle or not self._handle:
            return False
        freed = bool(self.api.host.storage_free(self.c_storage(), self.c_handle()))
        if freed:
            self._handle = 0
            self._owns_handle = False
        return freed

    def replace(self, other: "ClipperOperand") -> "ClipperOperand":
        """Release this operand and take ownership of ``other``'s handle."""
        _check_same_storage(self, other)
        if self is other:
            return self
        self.free_from_storage()
        self._handle = other.release()
        self._owns_handle = self._handle != 0
        return self

    def concat(self, other: "ClipperOperand") -> "ClipperOperand":
        """Return a new flat path accumulator without running a geometric union."""
        _check_same_storage(self, other)
        handle = self.api.host.clipper_concat(self.c_storage(), self.c_handle(), other.c_handle())
        return ClipperOperand.adopt_owned(self.api, self.storage(), handle)

    def concat_replace(self, other: "ClipperOperand") -> "ClipperOperand":
        """
        Append raw paths into this operand when the host can do so efficiently.

        This is not a geometric union. Use ``clipper_union()`` or
        ``ClipperContext.union()`` afterwards if overlapping paths must be
        merged.
        """
        _check_same_storage(self, other)
        handle = self.api.host.clipper_concat_replace(self.c_storage(), self.c_handle(), other.c_handle())
        self._handle = _address(handle)
        self._owns_handle = self._handle != 0
        return self

    def to_polygon_collection(self) -> StoredPolygonCollection:
        handle = self.api.host.clipper_shapes_to_polygons(self.c_storage(), self.c_handle())
        return StoredPolygonCollection.adopt_owned(self.api, self.storage(), handle)

    def to_expolygon_collection(self) -> StoredExPolygonCollection:
        handle = self.api.host.clipper_shapes_to_expolygons(self.c_storage(), self.c_handle())
        return StoredExPolygonCollection.adopt_owned(self.api, self.storage(), handle)

    def write_polygons_to(self, dst: StoredPolygonCollection) -> None:
        self.api.host.clipper_shapes_replace_polygons(dst.mutable_c_handle(), self.c_handle())

    def write_expolygons_to(self, dst: StoredExPolygonCollection) -> None:
        self.api.host.clipper_shapes_replace_expolygons(dst.mutable_c_handle(), self.c_handle())

    def for_each_expolygon(self, fn: Callable[[ExPolygon], object]) -> None:
        with self.to_expolygon_collection() as expolygons:
            for expolygon in expolygons:
                fn(expolygon)

    def __add__(self, other: "ClipperOperand") -> "ClipperOperand":
        return self.concat(other)

    def __iadd__(self, other: "ClipperOperand") -> "ClipperOperand":
        return self.concat_replace(other)

    def __bool__(self) -> bool:
        return self.is_valid_handle()

    def __enter__(self) -> "ClipperOperand":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.free_from_storage()

    def __del__(self) -> None:
        try:
            self.free_from_storage()
        except Exception:
            pass


def clipper_execute(storage_or_subject, operation: int, subject=None, clip=None) -> ClipperOperand:
    if isinstance(storage_or_subject, ClipperOperand):
        subject_operand = storage_or_subject
        storage = subject_operand.storage()
        api = subject_operand.api
        clip_operand = subject
    else:
        subject_operand = subject
        storage = _require_handle(storage_or_subject, "storage")
        api = subject_operand.api
        clip_operand = clip
        if subject_operand.storage() != storage:
            raise ValueError("explicit storage must match the clipper operand")
    if clip_operand is not None:
        _check_same_storage(subject_operand, clip_operand)
    clip_handle = ctypes.c_void_p(0) if clip_operand is None else clip_operand.c_handle()
    handle = api.host.clipper_execute(
        _void_p(storage), int(operation), subject_operand.c_handle(), clip_handle
    )
    return ClipperOperand.adopt_owned(api, storage, handle)


def clipper_diff(storage_or_subject, subject_or_clip, clip=None) -> ClipperOperand:
    api, storage, subject, other = _binary_args(storage_or_subject, subject_or_clip, clip)
    handle = api.host.clipper_diff(_void_p(storage), subject.c_handle(), other.c_handle())
    return ClipperOperand.adopt_owned(api, storage, handle)


def clipper_intersection(storage_or_subject, subject_or_clip, clip=None) -> ClipperOperand:
    api, storage, subject, other = _binary_args(storage_or_subject, subject_or_clip, clip)
    handle = api.host.clipper_intersection(_void_p(storage), subject.c_handle(), other.c_handle())
    return ClipperOperand.adopt_owned(api, storage, handle)


def clipper_diff_with_safety_offset(storage_or_subject, subject_or_clip, clip=None) -> ClipperOperand:
    api, storage, subject, other = _binary_args(storage_or_subject, subject_or_clip, clip)
    handle = api.host.clipper_diff_with_safety_offset(_void_p(storage), subject.c_handle(), other.c_handle())
    return ClipperOperand.adopt_owned(api, storage, handle)


def clipper_intersection_with_safety_offset(storage_or_subject, subject_or_clip, clip=None) -> ClipperOperand:
    api, storage, subject, other = _binary_args(storage_or_subject, subject_or_clip, clip)
    handle = api.host.clipper_intersection_with_safety_offset(_void_p(storage), subject.c_handle(), other.c_handle())
    return ClipperOperand.adopt_owned(api, storage, handle)


def clipper_union(storage_or_subject, subject=None) -> ClipperOperand:
    api, storage, operand = _unary_args(storage_or_subject, subject)
    handle = api.host.clipper_union(_void_p(storage), operand.c_handle())
    return ClipperOperand.adopt_owned(api, storage, handle)


def clipper_union_with_safety_offset(storage_or_subject, subject=None) -> ClipperOperand:
    api, storage, operand = _unary_args(storage_or_subject, subject)
    handle = api.host.clipper_union_with_safety_offset(_void_p(storage), operand.c_handle())
    return ClipperOperand.adopt_owned(api, storage, handle)


def clipper_union2(storage_or_subject, subject_or_second, second=None) -> ClipperOperand:
    api, storage, first, other = _binary_args(storage_or_subject, subject_or_second, second)
    handle = api.host.clipper_union2(_void_p(storage), first.c_handle(), other.c_handle())
    return ClipperOperand.adopt_owned(api, storage, handle)


def clipper_concat(storage_or_first, first_or_second, second=None) -> ClipperOperand:
    api, storage, first, other = _binary_args(storage_or_first, first_or_second, second)
    handle = api.host.clipper_concat(_void_p(storage), first.c_handle(), other.c_handle())
    return ClipperOperand.adopt_owned(api, storage, handle)


def clipper_offset(storage_or_subject,
                   subject_or_delta,
                   delta=None,
                   join_type: int = CLIPPER_JOIN_MITER,
                   miter_limit: float = 3.0,
                   end_type: int = CLIPPER_END_CLOSED_POLYGON) -> ClipperOperand:
    if isinstance(storage_or_subject, ClipperOperand):
        subject = storage_or_subject
        storage = subject.storage()
        api = subject.api
        offset_delta = float(subject_or_delta)
        if delta is not None:
            join_type = int(delta)
    else:
        storage = _require_handle(storage_or_subject, "storage")
        subject = subject_or_delta
        api = subject.api
        if subject.storage() != storage:
            raise ValueError("explicit storage must match the clipper operand")
        offset_delta = float(delta)
    handle = api.host.clipper_offset(
        _void_p(storage), subject.c_handle(), offset_delta, int(join_type), float(miter_limit), int(end_type)
    )
    return ClipperOperand.adopt_owned(api, storage, handle)


def clipper_offset2(storage_or_subject,
                    subject_or_delta1,
                    delta1_or_delta2,
                    delta2=None,
                    join_type: int = CLIPPER_JOIN_MITER,
                    miter_limit: float = 3.0,
                    end_type: int = CLIPPER_END_CLOSED_POLYGON) -> ClipperOperand:
    if isinstance(storage_or_subject, ClipperOperand):
        subject = storage_or_subject
        storage = subject.storage()
        api = subject.api
        offset_delta1 = float(subject_or_delta1)
        offset_delta2 = float(delta1_or_delta2)
        if delta2 is not None:
            join_type = int(delta2)
    else:
        storage = _require_handle(storage_or_subject, "storage")
        subject = subject_or_delta1
        api = subject.api
        if subject.storage() != storage:
            raise ValueError("explicit storage must match the clipper operand")
        offset_delta1 = float(delta1_or_delta2)
        offset_delta2 = float(delta2)
    handle = api.host.clipper_offset2(
        _void_p(storage),
        subject.c_handle(),
        offset_delta1,
        offset_delta2,
        int(join_type),
        float(miter_limit),
        int(end_type),
    )
    return ClipperOperand.adopt_owned(api, storage, handle)


def clipper_clip_shapes_with_subject_bbox(storage_or_subject, subject_or_bbox=None, bbox=None) -> ClipperOperand:
    """
    Clip a ClipperOperand to a local subject bounding box without materializing ExPolygons.

    This is a pre-filter for later boolean operations. The returned operand is a
    flat path-list shape, so use it to reduce the amount of geometry passed to
    ``clipper_intersection``/``clipper_diff`` and materialize only after the final
    operation that rebuilds contour/hole topology.
    """
    if isinstance(storage_or_subject, ClipperOperand):
        subject = storage_or_subject
        storage = subject.storage()
        api = subject.api
        subject_bbox = subject_or_bbox
    else:
        storage = _require_handle(storage_or_subject, "storage")
        subject = subject_or_bbox
        api = subject.api
        if subject.storage() != storage:
            raise ValueError("explicit storage must match the clipper operand")
        subject_bbox = bbox
    if subject.empty():
        return ClipperOperand.create_empty(api, storage)
    handle = api.host.clipper_clip_shapes_with_subject_bbox(
        _void_p(storage), subject.c_handle(), subject_bbox
    )
    return ClipperOperand.adopt_owned(api, storage, handle)


class ClipperContext:
    """
    Convenience object bound to one storage_handle.

    It keeps call sites compact and prevents accidentally mixing operands from
    different storages:

        clip = api.clipper(storage)
        result = clip.diff(clip(subject), clip(mask))
    """

    def __init__(self, api, storage) -> None:
        self.api = api
        self._storage = _require_handle(storage, "storage")

    @property
    def storage_address(self) -> int:
        return self._storage

    def storage(self) -> int:
        return self._storage

    def __call__(self, source=None) -> ClipperOperand:
        if source is None:
            return ClipperOperand.null(self.api, self.storage())
        return ClipperOperand(self.api, self.storage(), source)

    def empty(self) -> ClipperOperand:
        return ClipperOperand.create_empty(self.api, self.storage())

    def diff(self, subject: ClipperOperand, clip: ClipperOperand) -> ClipperOperand:
        return clipper_diff(self.storage(), subject, clip)

    def intersection(self, subject: ClipperOperand, clip: ClipperOperand) -> ClipperOperand:
        return clipper_intersection(self.storage(), subject, clip)

    def diff_with_safety_offset(self, subject: ClipperOperand, clip: ClipperOperand) -> ClipperOperand:
        return clipper_diff_with_safety_offset(self.storage(), subject, clip)

    def intersection_with_safety_offset(self, subject: ClipperOperand, clip: ClipperOperand) -> ClipperOperand:
        return clipper_intersection_with_safety_offset(self.storage(), subject, clip)

    def union(self, subject: ClipperOperand) -> ClipperOperand:
        return clipper_union(self.storage(), subject)

    def union_with_safety_offset(self, subject: ClipperOperand) -> ClipperOperand:
        return clipper_union_with_safety_offset(self.storage(), subject)

    def union2(self, subject1: ClipperOperand, subject2: ClipperOperand) -> ClipperOperand:
        return clipper_union2(self.storage(), subject1, subject2)

    def concat(self, first: ClipperOperand, second: ClipperOperand) -> ClipperOperand:
        return clipper_concat(self.storage(), first, second)

    def offset(self,
               subject: ClipperOperand,
               delta: float,
               join_type: int = CLIPPER_JOIN_MITER,
               miter_limit: float = 3.0,
               end_type: int = CLIPPER_END_CLOSED_POLYGON) -> ClipperOperand:
        return clipper_offset(self.storage(), subject, delta, join_type, miter_limit, end_type)

    def offset2(self,
                subject: ClipperOperand,
                delta1: float,
                delta2: float,
                join_type: int = CLIPPER_JOIN_MITER,
                miter_limit: float = 3.0,
                end_type: int = CLIPPER_END_CLOSED_POLYGON) -> ClipperOperand:
        return clipper_offset2(self.storage(), subject, delta1, delta2, join_type, miter_limit, end_type)

    def clip_shapes_with_subject_bbox(self, subject: ClipperOperand, bbox) -> ClipperOperand:
        return clipper_clip_shapes_with_subject_bbox(self.storage(), subject, bbox)


__all__ = [
    "CLIPPER_END_CLOSED_POLYGON",
    "CLIPPER_END_CLOSED_LINE",
    "CLIPPER_END_OPEN_BUTT",
    "CLIPPER_END_OPEN_ROUND",
    "CLIPPER_END_OPEN_SQUARE",
    "CLIPPER_JOIN_MITER",
    "CLIPPER_JOIN_ROUND",
    "CLIPPER_JOIN_SQUARE",
    "CLIPPER_OPERATION_DIFFERENCE",
    "CLIPPER_OPERATION_INTERSECTION",
    "CLIPPER_OPERATION_UNION",
    "CLIPPER_OPERATION_XOR",
    "ClipperContext",
    "ClipperOperand",
    "clipper_concat",
    "clipper_clip_shapes_with_subject_bbox",
    "clipper_diff",
    "clipper_diff_with_safety_offset",
    "clipper_execute",
    "clipper_intersection",
    "clipper_intersection_with_safety_offset",
    "clipper_offset",
    "clipper_offset2",
    "clipper_union",
    "clipper_union2",
    "clipper_union_with_safety_offset",
]
