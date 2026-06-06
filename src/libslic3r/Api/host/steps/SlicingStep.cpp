///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "SlicingStep.hpp"

#include <memory>
#include "libslic3r/Print.hpp"

namespace Slic3r::ApiHost::Steps {

std::unique_ptr<SlicingRunContext> make_slicing_run_context(Print &print, size_t object_idx)
{
    std::unique_ptr<SlicingRunContext> out = std::make_unique<SlicingRunContext>();
    out->context_step.print = reinterpret_cast<const print_handle *>(&print);
    out->context_step.object = reinterpret_cast<const object_handle *>(&print.object(object_idx));
    out->context_step.layer_region_borrow_mutable_slices = layer_region_borrow_mutable_slices;
    out->context_step.layer_range_count = object_count_slicing_layer_range;
    out->context_step.layer_range_at = object_get_slicing_layer_range;
    out->context_step.layer_range_z_min = slicing_layer_range_get_z_min;
    out->context_step.layer_range_z_max = slicing_layer_range_get_z_max;
    out->context_step.layer_range_config = slicing_layer_range_get_config;
    out->context_step.layer_range_volume_region_count = slicing_layer_range_count_volume_region;
    out->context_step.layer_range_volume_region_at = slicing_layer_range_get_volume_region;
    out->context_step.volume_region_volume = slicing_volume_region_get_volume;
    out->context_step.volume_region_parent = slicing_volume_region_get_parent;
    out->context_step.volume_region_layer_region_idx = slicing_volume_region_get_layer_region_idx;
    out->context_step.volume_region_bbox = slicing_volume_region_get_bbox;
    return out;
}

} // namespace Slic3r::ApiHost::Steps
